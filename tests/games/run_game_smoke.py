#!/usr/bin/env python3
"""Batch smoke runner for real game ROMs.

The script uses ./rom_tester for each ROM, captures screenshots, analyzes the
images, and writes a TSV plus a Markdown summary.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import hashlib
import math
from pathlib import Path
import re
import subprocess
import sys
from collections import Counter, defaultdict


DEFAULT_ROM_ROOT = Path("roms/game-boy-and-game-boy-color-complete-collection")


CART_TYPES = {
    0x00: "ROM ONLY",
    0x01: "MBC1",
    0x02: "MBC1+RAM",
    0x03: "MBC1+RAM+BATTERY",
    0x05: "MBC2",
    0x06: "MBC2+BATTERY",
    0x08: "ROM+RAM",
    0x09: "ROM+RAM+BATTERY",
    0x0B: "MMM01",
    0x0C: "MMM01+RAM",
    0x0D: "MMM01+RAM+BATTERY",
    0x0F: "MBC3+TIMER+BATTERY",
    0x10: "MBC3+TIMER+RAM+BATTERY",
    0x11: "MBC3",
    0x12: "MBC3+RAM",
    0x13: "MBC3+RAM+BATTERY",
    0x19: "MBC5",
    0x1A: "MBC5+RAM",
    0x1B: "MBC5+RAM+BATTERY",
    0x1C: "MBC5+RUMBLE",
    0x1D: "MBC5+RUMBLE+RAM",
    0x1E: "MBC5+RUMBLE+RAM+BATTERY",
    0x20: "MBC6",
    0x22: "MBC7+SENSOR+RUMBLE+RAM+BATTERY",
    0xFC: "POCKET CAMERA",
    0xFD: "BANDAI TAMA5",
    0xFE: "HuC3",
    0xFF: "HuC1+RAM+BATTERY",
}

ROM_SIZE_KIB = {
    0x00: 32,
    0x01: 64,
    0x02: 128,
    0x03: 256,
    0x04: 512,
    0x05: 1024,
    0x06: 2048,
    0x07: 4096,
    0x08: 8192,
    0x52: 1152,
    0x53: 1280,
    0x54: 1536,
}

RAM_SIZE_KIB = {
    0x00: 0,
    0x01: 2,
    0x02: 8,
    0x03: 32,
    0x04: 128,
    0x05: 64,
}

VERDICT_LABELS = {
    "OK_CHANGED": "Works - screen changed",
    "OK_STATIC": "Works - stable screen",
    "SUSPECT_BLANK": "Review - blank screen",
    "SUSPECT_LOW_DETAIL": "Review - low visual detail",
    "LOAD_FAIL": "Failed - load error",
    "RUN_TIMEOUT": "Failed - runtime timeout",
    "EMULATOR_CRASH": "Failed - emulator crash",
    "EMULATOR_ABORT": "Failed - emulator abort",
    "RUNNER_ERROR": "Tool error",
    "NO_SCREENSHOT": "No screenshot",
}

GOOD_VERDICTS = {"OK_CHANGED", "OK_STATIC"}
REVIEW_VERDICTS = {"SUSPECT_BLANK", "SUSPECT_LOW_DETAIL", "NO_SCREENSHOT"}
FAILED_VERDICTS = {
    "LOAD_FAIL",
    "RUN_TIMEOUT",
    "EMULATOR_CRASH",
    "EMULATOR_ABORT",
    "RUNNER_ERROR",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run many game ROMs through rom_tester and classify screenshots."
    )
    parser.add_argument(
        "--rom-root",
        action="append",
        type=Path,
        default=None,
        help="Directory to scan for .gb/.gbc files. Can be passed more than once.",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=Path("tests/games/out"),
        help="Output directory for report, logs, and screenshots.",
    )
    parser.add_argument("--frames", type=int, default=1200, help="Final frame count.")
    parser.add_argument(
        "--early-frames", type=int, default=120, help="Early screenshot frame count."
    )
    parser.add_argument(
        "--probe-frames",
        type=int,
        default=0,
        help=(
            "Optional probe screenshot frame count. Defaults to one third of "
            "the early-to-final window, and is only used to triage OK->BLANK cases."
        ),
    )
    parser.add_argument("--limit", type=int, default=0, help="Limit number of ROMs.")
    parser.add_argument(
        "--rom",
        action="append",
        type=Path,
        default=None,
        help="Run one explicit ROM path. Can be passed more than once.",
    )
    parser.add_argument("--jobs", type=int, default=1, help="Parallel rom_tester jobs.")
    parser.add_argument(
        "--timeout",
        type=int,
        default=45,
        help="Wall-clock timeout, in seconds, passed to rom_tester for each run.",
    )
    parser.add_argument(
        "--no-build", action="store_true", help="Do not run make rom_tester first."
    )
    parser.add_argument(
        "--pattern",
        action="append",
        default=["*.gb", "*.gbc"],
        help="Glob pattern relative to each ROM root. Can be repeated.",
    )
    parser.add_argument(
        "--include-compat-roms",
        action="store_true",
        help="Do not filter out blargg/mooneye/acid compatibility ROMs.",
    )
    return parser.parse_args()


def discover_roms(roots: list[Path], patterns: list[str], include_compat: bool) -> list[Path]:
    roms: list[Path] = []
    skip_parts = {
        "blargg-test-roms-master",
        "mooneye-test-suite",
    }
    skip_names = {"dmg-acid2.gb", "cgb-acid2.gbc"}

    for root in roots:
        if not root.exists():
            continue
        for pattern in patterns:
            for path in root.rglob(pattern):
                if not path.is_file():
                    continue
                if not include_compat:
                    if any(part in skip_parts for part in path.parts):
                        continue
                    if path.name in skip_names:
                        continue
                roms.append(path)

    return sorted(set(roms), key=lambda p: str(p).casefold())


def sanitize_stem(path: Path) -> str:
    stem = re.sub(r"[^A-Za-z0-9._+-]+", "_", path.stem).strip("_")
    if not stem:
        stem = "rom"
    digest = hashlib.sha1(str(path).encode("utf-8", "replace")).hexdigest()[:10]
    return f"{stem[:70]}-{digest}"


def read_header(path: Path) -> dict[str, str]:
    try:
        data = path.read_bytes()
    except OSError:
        data = b""

    def b(off: int, default: int = 0) -> int:
        return data[off] if off < len(data) else default

    title_bytes = data[0x134:0x144] if len(data) >= 0x144 else b""
    title = title_bytes.split(b"\0", 1)[0].decode("ascii", "replace").strip()
    cgb = b(0x143)
    cart_type_id = b(0x147)
    rom_size_id = b(0x148)
    ram_size_id = b(0x149)
    sgb = b(0x146)

    if cgb == 0x80:
        cgb_flag = "enhanced"
    elif cgb == 0xC0:
        cgb_flag = "only"
    else:
        cgb_flag = "none"

    return {
        "title": title,
        "cgb_flag": cgb_flag,
        "sgb": "yes" if sgb == 0x03 else "no",
        "cart_type": CART_TYPES.get(cart_type_id, f"UNKNOWN_0x{cart_type_id:02X}"),
        "cart_type_id": f"0x{cart_type_id:02X}",
        "rom_kib": str(ROM_SIZE_KIB.get(rom_size_id, 0)),
        "ram_kib": str(RAM_SIZE_KIB.get(ram_size_id, 0)),
        "header_checksum": f"0x{b(0x14D):02X}",
    }


def read_ppm(path: Path) -> tuple[int, int, bytes] | None:
    try:
        data = path.read_bytes()
    except OSError:
        return None
    if not data.startswith(b"P6\n"):
        return None

    idx = 3
    header: list[bytes] = []
    while len(header) < 2 and idx < len(data):
        line_end = data.find(b"\n", idx)
        if line_end < 0:
            return None
        line = data[idx:line_end]
        idx = line_end + 1
        if line.startswith(b"#"):
            continue
        header.extend(line.split())

    if len(header) < 2:
        return None

    line_end = data.find(b"\n", idx)
    if line_end < 0:
        return None
    maxval = int(data[idx:line_end])
    if maxval != 255:
        return None
    idx = line_end + 1

    width, height = int(header[0]), int(header[1])
    pixels = data[idx:]
    if len(pixels) < width * height * 3:
        return None
    return width, height, pixels[: width * height * 3]


def image_metrics(path: Path) -> dict[str, str | float | int]:
    ppm = read_ppm(path)
    if not ppm:
        return {
            "image_ok": "no",
            "mean": 0.0,
            "stddev": 0.0,
            "edge": 0.0,
            "unique_sample": 0,
        }

    width, height, pixels = ppm
    channels = len(pixels)
    total = sum(pixels)
    total_sq = sum(v * v for v in pixels)
    mean = total / channels
    variance = max(0.0, total_sq / channels - mean * mean)
    stddev = math.sqrt(variance)

    sample_step = max(1, (width * height) // 4096)
    colors = set()
    for i in range(0, width * height, sample_step):
        j = i * 3
        colors.add(pixels[j : j + 3])

    edge_sum = 0
    edge_count = 0
    for y in range(0, height, 2):
        row = y * width * 3
        for x in range(0, width - 1, 2):
            a = row + x * 3
            b = a + 3
            edge_sum += abs(pixels[a] - pixels[b])
            edge_sum += abs(pixels[a + 1] - pixels[b + 1])
            edge_sum += abs(pixels[a + 2] - pixels[b + 2])
            edge_count += 3
    edge = edge_sum / edge_count if edge_count else 0.0

    return {
        "image_ok": "yes",
        "mean": mean,
        "stddev": stddev,
        "edge": edge,
        "unique_sample": len(colors),
    }


def image_delta(a: Path, b: Path) -> float:
    ppm_a = read_ppm(a)
    ppm_b = read_ppm(b)
    if not ppm_a or not ppm_b:
        return 0.0
    if ppm_a[0] != ppm_b[0] or ppm_a[1] != ppm_b[1]:
        return 0.0
    pa = ppm_a[2]
    pb = ppm_b[2]
    count = min(len(pa), len(pb))
    if count == 0:
        return 0.0
    return sum(abs(pa[i] - pb[i]) for i in range(count)) / count


def classify(final_status: str, final_metrics: dict[str, str | float | int], delta: float) -> str:
    if final_status == "LOAD_FAIL":
        return "LOAD_FAIL"
    if final_status == "RUN_TIMEOUT":
        return "RUN_TIMEOUT"
    if final_status == "CRASH":
        return "EMULATOR_CRASH"
    if final_status == "EMULATOR_ABORT":
        return "EMULATOR_ABORT"
    if final_status not in {"OK", "BLANK"}:
        return "RUNNER_ERROR"
    if final_metrics["image_ok"] != "yes":
        return "NO_SCREENSHOT"

    stddev = float(final_metrics["stddev"])
    edge = float(final_metrics["edge"])
    unique = int(final_metrics["unique_sample"])

    if final_status == "BLANK" or unique <= 1 or stddev < 1.0:
        return "SUSPECT_BLANK"
    if unique <= 3 or (stddev < 6.0 and edge < 2.0):
        return "SUSPECT_LOW_DETAIL"
    if delta >= 2.5:
        return "OK_CHANGED"
    return "OK_STATIC"


def verdict_label(verdict: str) -> str:
    return VERDICT_LABELS.get(verdict, verdict.replace("_", " ").title())


def pct(count: int, total: int) -> str:
    if total <= 0:
        return "0.0%"
    return f"{(count * 100.0 / total):.1f}%"


def format_counter(counter: Counter[str], total: int) -> str:
    return ", ".join(
        f"{verdict_label(key)}={value} ({pct(value, total)})"
        for key, value in sorted(counter.items(), key=lambda item: verdict_label(item[0]))
    )


def run_rom_tester(
    rom: Path, frames: int, out_dir: Path, timeout: int, subprocess_timeout: int
) -> dict[str, str]:
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [
        "./rom_tester",
        "--frames",
        str(frames),
        "--out",
        str(out_dir),
        "--timeout",
        str(timeout),
        str(rom),
    ]
    try:
        proc = subprocess.run(
            cmd,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=subprocess_timeout,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return {
            "status": "RUN_TIMEOUT",
            "stdout": "",
            "stderr": "subprocess timeout",
            "ppm": "",
            "returncode": "124",
        }

    stdout = proc.stdout.strip()
    parts = stdout.split("\t")
    status = parts[0] if parts and parts[0] else "UNKNOWN"
    ppm = parts[2] if len(parts) >= 3 else ""
    if proc.returncode != 0 and status not in {
        "OK",
        "BLANK",
        "LOAD_FAIL",
        "RUN_TIMEOUT",
        "CRASH",
    }:
        status = "EMULATOR_ABORT"
    return {
        "status": status,
        "stdout": stdout,
        "stderr": proc.stderr.strip(),
        "ppm": ppm,
        "returncode": str(proc.returncode),
    }


def run_one(rom: Path, args: argparse.Namespace) -> dict[str, str]:
    slug = sanitize_stem(rom)
    rom_out = args.out / "screenshots" / slug
    early_out = rom_out / "early"
    probe_out = rom_out / "probe"
    final_out = rom_out / "final"
    log_path = args.out / "logs" / f"{slug}.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)

    early = run_rom_tester(
        rom, args.early_frames, early_out, args.timeout, args.timeout + 10
    )
    final = run_rom_tester(
        rom, args.frames, final_out, args.timeout, args.timeout + 10
    )
    probe = {
        "status": "",
        "stdout": "",
        "stderr": "",
        "ppm": "",
        "returncode": "",
    }
    if (
        args.probe_frames > 0
        and final["status"] == "BLANK"
        and early["status"] in {"OK", "BLANK"}
        and args.probe_frames not in {args.early_frames, args.frames}
    ):
        probe = run_rom_tester(
            rom, args.probe_frames, probe_out, args.timeout, args.timeout + 10
        )

    early_ppm = Path(early["ppm"]) if early["ppm"] else Path()
    probe_ppm = Path(probe["ppm"]) if probe["ppm"] else Path()
    final_ppm = Path(final["ppm"]) if final["ppm"] else Path()
    final_metrics = image_metrics(final_ppm) if final_ppm else image_metrics(Path(""))
    early_metrics = image_metrics(early_ppm) if early_ppm else image_metrics(Path(""))
    probe_metrics = image_metrics(probe_ppm) if probe_ppm else image_metrics(Path(""))
    delta = image_delta(early_ppm, final_ppm) if early_ppm and final_ppm else 0.0
    assess_status = final["status"]
    assess_metrics = final_metrics
    assess_delta = delta
    assess_frame = args.frames
    assess_ppm = final_ppm
    if final["status"] == "BLANK" and probe["status"] == "OK":
        assess_status = probe["status"]
        assess_metrics = probe_metrics
        assess_delta = image_delta(early_ppm, probe_ppm) if early_ppm and probe_ppm else 0.0
        assess_frame = args.probe_frames
        assess_ppm = probe_ppm
    verdict = classify(assess_status, assess_metrics, assess_delta)
    header = read_header(rom)

    log_path.write_text(
        "\n".join(
            [
                f"rom={rom}",
                f"early_stdout={early['stdout']}",
                f"early_stderr={early['stderr']}",
                f"early_returncode={early['returncode']}",
                f"probe_stdout={probe['stdout']}",
                f"probe_stderr={probe['stderr']}",
                f"probe_returncode={probe['returncode']}",
                f"final_stdout={final['stdout']}",
                f"final_stderr={final['stderr']}",
                f"final_returncode={final['returncode']}",
            ]
        )
        + "\n",
        encoding="utf-8",
    )

    return {
        "verdict": verdict,
        "verdict_label": verdict_label(verdict),
        "rom": str(rom),
        "title": header["title"],
        "cgb_flag": header["cgb_flag"],
        "sgb": header["sgb"],
        "cart_type": header["cart_type"],
        "cart_type_id": header["cart_type_id"],
        "rom_kib": header["rom_kib"],
        "ram_kib": header["ram_kib"],
        "early_status": early["status"],
        "probe_status": probe["status"],
        "final_status": final["status"],
        "assess_status": assess_status,
        "assess_frame": str(assess_frame),
        "early_returncode": early["returncode"],
        "probe_returncode": probe["returncode"],
        "final_returncode": final["returncode"],
        "early_ppm": str(early_ppm) if early_ppm else "",
        "probe_ppm": str(probe_ppm) if probe_ppm else "",
        "final_ppm": str(final_ppm) if final_ppm else "",
        "assessment_ppm": str(assess_ppm) if assess_ppm else "",
        "delta": f"{assess_delta:.3f}",
        "final_delta": f"{delta:.3f}",
        "final_mean": f"{float(final_metrics['mean']):.3f}",
        "final_stddev": f"{float(final_metrics['stddev']):.3f}",
        "final_edge": f"{float(final_metrics['edge']):.3f}",
        "final_unique_sample": str(final_metrics["unique_sample"]),
        "assessment_mean": f"{float(assess_metrics['mean']):.3f}",
        "assessment_stddev": f"{float(assess_metrics['stddev']):.3f}",
        "assessment_edge": f"{float(assess_metrics['edge']):.3f}",
        "assessment_unique_sample": str(assess_metrics["unique_sample"]),
        "log": str(log_path),
    }


def write_reports(rows: list[dict[str, str]], out_dir: Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    fields = [
        "verdict",
        "verdict_label",
        "rom",
        "title",
        "cgb_flag",
        "sgb",
        "cart_type",
        "cart_type_id",
        "rom_kib",
        "ram_kib",
        "early_status",
        "probe_status",
        "final_status",
        "assess_status",
        "assess_frame",
        "early_returncode",
        "probe_returncode",
        "final_returncode",
        "delta",
        "final_delta",
        "final_mean",
        "final_stddev",
        "final_edge",
        "final_unique_sample",
        "assessment_mean",
        "assessment_stddev",
        "assessment_edge",
        "assessment_unique_sample",
        "early_ppm",
        "probe_ppm",
        "final_ppm",
        "assessment_ppm",
        "log",
    ]
    with (out_dir / "game_smoke.tsv").open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fields, delimiter="\t", extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)

    verdicts = Counter(row["verdict"] for row in rows)
    total = len(rows)
    good = sum(verdicts[key] for key in GOOD_VERDICTS)
    review = sum(verdicts[key] for key in REVIEW_VERDICTS)
    failed = sum(verdicts[key] for key in FAILED_VERDICTS)
    by_cart: dict[str, Counter[str]] = defaultdict(Counter)
    by_cgb: dict[str, Counter[str]] = defaultdict(Counter)
    for row in rows:
        by_cart[row["cart_type"]][row["verdict"]] += 1
        by_cgb[row["cgb_flag"]][row["verdict"]] += 1

    suspect = [
        row
        for row in rows
        if row["verdict"].startswith("SUSPECT")
        or row["verdict"]
        in {
            "LOAD_FAIL",
            "RUN_TIMEOUT",
            "EMULATOR_CRASH",
            "EMULATOR_ABORT",
            "RUNNER_ERROR",
            "NO_SCREENSHOT",
        }
    ]

    lines = [
        "# Game Smoke Summary",
        "",
        f"Total ROMs: {total}",
        "",
        "## Readiness",
        "",
        f"- Likely working: {good} ({pct(good, total)})",
        f"- Needs review: {review} ({pct(review, total)})",
        f"- Failed/tool error: {failed} ({pct(failed, total)})",
        "",
        "## Verdicts",
        "",
    ]
    for key, count in sorted(verdicts.items(), key=lambda item: verdict_label(item[0])):
        lines.append(f"- {verdict_label(key)} (`{key}`): {count} ({pct(count, total)})")

    lines.extend(["", "## By CGB Flag", ""])
    for key in sorted(by_cgb):
        group_total = sum(by_cgb[key].values())
        parts = format_counter(by_cgb[key], group_total)
        lines.append(f"- {key}: {parts}")

    lines.extend(["", "## By Cartridge Type", ""])
    for key in sorted(by_cart):
        group_total = sum(by_cart[key].values())
        parts = format_counter(by_cart[key], group_total)
        lines.append(f"- {key}: {parts}")

    lines.extend(["", "## Suspect ROMs", ""])
    if not suspect:
        lines.append("- none")
    else:
        for row in suspect[:200]:
            lines.append(
                f"- {row['verdict_label']} (`{row['verdict']}`): {row['rom']} "
                f"({row['cart_type']}, cgb={row['cgb_flag']}, "
                f"stddev={row['assessment_stddev']}, edge={row['assessment_edge']}, "
                f"delta={row['delta']}, "
                f"status={row['early_status']}"
                f"{'->' + row['probe_status'] if row['probe_status'] else ''}"
                f"->{row['final_status']}, "
                f"assess={row['assess_status']}@{row['assess_frame']}, "
                f"rc={row['early_returncode']}/{row['final_returncode']})"
            )
        if len(suspect) > 200:
            lines.append(f"- ... {len(suspect) - 200} more")

    (out_dir / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_progress_reports(rows: list[dict[str, str]], out_dir: Path) -> None:
    ordered = sorted(rows, key=lambda row: row["rom"].casefold())
    write_reports(ordered, out_dir)


def main() -> int:
    args = parse_args()
    roots = args.rom_root if args.rom_root else [DEFAULT_ROM_ROOT]

    if args.early_frames >= args.frames:
        args.early_frames = max(1, args.frames // 2)
    if args.probe_frames <= 0 and args.frames > args.early_frames + 1:
        args.probe_frames = args.early_frames + (args.frames - args.early_frames) // 3
    if args.probe_frames <= args.early_frames or args.probe_frames >= args.frames:
        args.probe_frames = 0

    if not args.no_build:
        subprocess.run(["make", "rom_tester"], check=True)

    if args.rom:
        roms = [rom for rom in args.rom if rom.is_file()]
        missing = [rom for rom in args.rom if not rom.is_file()]
        for rom in missing:
            print(f"missing ROM: {rom}", file=sys.stderr, flush=True)
    else:
        roms = discover_roms(roots, args.pattern, args.include_compat_roms)
    if args.limit > 0:
        roms = roms[: args.limit]

    if not roms:
        print("No ROMs found.", file=sys.stderr)
        return 2

    args.out.mkdir(parents=True, exist_ok=True)
    print(
        f"game-smoke roms={len(roms)} frames={args.frames} "
        f"early_frames={args.early_frames} probe_frames={args.probe_frames} "
        f"out={args.out}",
        flush=True,
    )

    rows: list[dict[str, str]] = []
    if args.jobs <= 1:
        for index, rom in enumerate(roms, 1):
            row = run_one(rom, args)
            rows.append(row)
            write_progress_reports(rows, args.out)
            print(f"[{index}/{len(roms)}] {row['verdict']}\t{rom}", flush=True)
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            future_to_rom = {pool.submit(run_one, rom, args): rom for rom in roms}
            for index, future in enumerate(concurrent.futures.as_completed(future_to_rom), 1):
                rom = future_to_rom[future]
                try:
                    row = future.result()
                except Exception as exc:  # pragma: no cover - defensive batch reporting
                    row = {
                        "verdict": "RUNNER_ERROR",
                        "verdict_label": verdict_label("RUNNER_ERROR"),
                        "rom": str(rom),
                        "title": "",
                        "cgb_flag": "",
                        "sgb": "",
                        "cart_type": "",
                        "cart_type_id": "",
                        "rom_kib": "",
                        "ram_kib": "",
                        "early_status": "",
                        "probe_status": "",
                        "final_status": "",
                        "assess_status": "",
                        "assess_frame": "",
                        "early_returncode": "",
                        "probe_returncode": "",
                        "final_returncode": "",
                        "early_ppm": "",
                        "probe_ppm": "",
                        "final_ppm": "",
                        "assessment_ppm": "",
                        "delta": "0.000",
                        "final_delta": "0.000",
                        "final_mean": "0.000",
                        "final_stddev": "0.000",
                        "final_edge": "0.000",
                        "final_unique_sample": "0",
                        "assessment_mean": "0.000",
                        "assessment_stddev": "0.000",
                        "assessment_edge": "0.000",
                        "assessment_unique_sample": "0",
                        "log": "",
                    }
                    print(f"runner error for {rom}: {exc}", file=sys.stderr, flush=True)
                rows.append(row)
                write_progress_reports(rows, args.out)
                print(f"[{index}/{len(roms)}] {row['verdict']}\t{rom}", flush=True)

    rows.sort(key=lambda row: row["rom"].casefold())
    write_reports(rows, args.out)
    print(f"report={args.out / 'game_smoke.tsv'}", flush=True)
    print(f"summary={args.out / 'summary.md'}", flush=True)

    bad = sum(
        1
        for row in rows
        if row["verdict"]
        in {
            "LOAD_FAIL",
            "RUN_TIMEOUT",
            "EMULATOR_CRASH",
            "EMULATOR_ABORT",
            "RUNNER_ERROR",
            "NO_SCREENSHOT",
            "SUSPECT_BLANK",
        }
    )
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
