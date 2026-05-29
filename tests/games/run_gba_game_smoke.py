#!/usr/bin/env python3
"""Batch smoke runner for real GBA game ROMs.

The script uses ./gba_compat_test in headless visual mode, captures early/final
PPM frames, classifies the images, and writes a TSV plus a Markdown summary.
ZIP archives are treated as one game by default; the best-looking .gba entry is
extracted to a local cache before running.
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
import zipfile
from collections import Counter


DEFAULT_ROM_ROOT = Path("roms/GameboyAdvanceRomCollectionByGhostware")

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
    "ZIP_EMPTY": "Archive without GBA",
}

GOOD_VERDICTS = {"OK_CHANGED", "OK_STATIC"}
REVIEW_VERDICTS = {"SUSPECT_BLANK", "SUSPECT_LOW_DETAIL", "NO_SCREENSHOT"}
FAILED_VERDICTS = {
    "LOAD_FAIL",
    "RUN_TIMEOUT",
    "EMULATOR_CRASH",
    "EMULATOR_ABORT",
    "RUNNER_ERROR",
    "ZIP_EMPTY",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run many GBA game ROMs through gba_compat_test and classify screenshots."
    )
    parser.add_argument(
        "--rom-root",
        action="append",
        type=Path,
        default=None,
        help="Directory to scan for .gba/.zip files. Can be passed more than once.",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=Path("tests/games/gba_out"),
        help="Output directory for report, logs, screenshots, and extracted cache.",
    )
    parser.add_argument(
        "--cycles",
        type=int,
        default=100_000_000,
        help="Final max-cycles value passed to gba_compat_test.",
    )
    parser.add_argument(
        "--early-cycles",
        type=int,
        default=10_000_000,
        help="Early max-cycles value used for image-delta classification.",
    )
    parser.add_argument("--limit", type=int, default=0, help="Limit number of games.")
    parser.add_argument(
        "--rom",
        action="append",
        type=Path,
        default=None,
        help="Run one explicit .gba or .zip path. Can be passed more than once.",
    )
    parser.add_argument("--jobs", type=int, default=1, help="Parallel gba_compat_test jobs.")
    parser.add_argument(
        "--timeout",
        type=int,
        default=60,
        help="Wall-clock timeout, in seconds, for each gba_compat_test invocation.",
    )
    parser.add_argument(
        "--no-build", action="store_true", help="Do not run make gba_compat_test first."
    )
    parser.add_argument(
        "--pattern",
        action="append",
        default=["*.gba", "*.zip"],
        help="Glob pattern relative to each ROM root. Can be repeated.",
    )
    parser.add_argument(
        "--all-zip-entries",
        action="store_true",
        help="Run every .gba inside each ZIP instead of selecting one preferred entry.",
    )
    parser.add_argument(
        "--include-extracted-duplicates",
        action="store_true",
        help="Also scan .gba files inside folders that have a sibling ZIP with the same name.",
    )
    return parser.parse_args()


def has_sibling_zip_dir(path: Path, root: Path) -> bool:
    for parent in path.parents:
        if parent == root or root not in parent.parents:
            break
        if parent.with_suffix(".zip").is_file():
            return True
    return False


def discover_roms(
    roots: list[Path], patterns: list[str], include_extracted_duplicates: bool
) -> list[Path]:
    roms: list[Path] = []
    for root in roots:
        if not root.exists():
            continue
        for pattern in patterns:
            for path in root.rglob(pattern):
                if not path.is_file():
                    continue
                if (
                    not include_extracted_duplicates
                    and path.suffix.casefold() == ".gba"
                    and has_sibling_zip_dir(path, root)
                ):
                    continue
                roms.append(path)
    return sorted(set(roms), key=lambda p: str(p).casefold())


def sanitize_text(text: str, fallback: str = "rom") -> str:
    stem = re.sub(r"[^A-Za-z0-9._+-]+", "_", text).strip("_")
    return stem[:90] if stem else fallback


def sanitize_stem(path: Path, entry_name: str = "") -> str:
    basis = f"{path}!{entry_name}" if entry_name else str(path)
    stem = sanitize_text(Path(entry_name).stem if entry_name else path.stem)
    digest = hashlib.sha1(basis.encode("utf-8", "replace")).hexdigest()[:10]
    return f"{stem}-{digest}"


def zip_entry_score(name: str) -> tuple[int, str]:
    lower = name.casefold()
    base = Path(name).name.casefold()
    score = 0
    if "[!]" in base:
        score += 1000
    if re.search(r"\((u|usa|us|world)\)", lower):
        score += 500
    elif re.search(r"\((e|eur|europe)\)", lower):
        score += 300
    elif re.search(r"\((j|jpn|japan)\)", lower):
        score += 100
    bad_tokens = [
        "[b",
        "[t",
        "[f",
        "[h",
        "[o",
        "[p",
        "[t+",
        "[t-",
        "(pd)",
        "bios",
        "trainer",
        "intro",
        "demo",
    ]
    score -= 80 * sum(1 for token in bad_tokens if token in lower)
    return -score, base


def zip_gba_entries(path: Path) -> list[str]:
    try:
        with zipfile.ZipFile(path) as zf:
            return sorted(
                [
                    info.filename
                    for info in zf.infolist()
                    if not info.is_dir() and info.filename.casefold().endswith(".gba")
                ],
                key=zip_entry_score,
            )
    except zipfile.BadZipFile:
        return []


def selected_rom_specs(paths: list[Path], all_zip_entries: bool) -> list[tuple[Path, str]]:
    specs: list[tuple[Path, str]] = []
    for path in paths:
        if path.suffix.casefold() != ".zip":
            specs.append((path, ""))
            continue
        entries = zip_gba_entries(path)
        if all_zip_entries:
            specs.extend((path, entry) for entry in entries)
        elif entries:
            specs.append((path, entries[0]))
        else:
            specs.append((path, ""))
    return specs


def extract_zip_entry(zip_path: Path, entry_name: str, cache_dir: Path) -> Path | None:
    if not entry_name:
        return None
    slug = sanitize_stem(zip_path, entry_name)
    out_path = cache_dir / f"{slug}.gba"
    if out_path.exists() and out_path.stat().st_size > 0:
        return out_path
    cache_dir.mkdir(parents=True, exist_ok=True)
    try:
        with zipfile.ZipFile(zip_path) as zf:
            with zf.open(entry_name) as src, out_path.open("wb") as dst:
                while True:
                    chunk = src.read(1024 * 1024)
                    if not chunk:
                        break
                    dst.write(chunk)
        return out_path
    except (OSError, KeyError, zipfile.BadZipFile):
        try:
            out_path.unlink()
        except OSError:
            pass
        return None


def read_header(path: Path) -> dict[str, str]:
    try:
        data = path.read_bytes()
    except OSError:
        data = b""

    def b(off: int, default: int = 0) -> int:
        return data[off] if off < len(data) else default

    title = data[0xA0:0xAC].split(b"\0", 1)[0].decode("ascii", "replace").strip()
    game_code = data[0xAC:0xB0].decode("ascii", "replace").strip() if len(data) >= 0xB0 else ""
    maker = data[0xB0:0xB2].decode("ascii", "replace").strip() if len(data) >= 0xB2 else ""
    return {
        "title": title,
        "game_code": game_code,
        "maker": maker,
        "unit_code": f"0x{b(0xB3):02X}",
        "device_type": f"0x{b(0xB4):02X}",
        "version": str(b(0xBC)),
        "complement": f"0x{b(0xBD):02X}",
        "rom_mib": f"{(len(data) / (1024 * 1024)):.1f}" if data else "0.0",
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
        if not line.startswith(b"#"):
            header.extend(line.split())

    if len(header) < 2:
        return None
    line_end = data.find(b"\n", idx)
    if line_end < 0 or int(data[idx:line_end]) != 255:
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
        return {"image_ok": "no", "mean": 0.0, "stddev": 0.0, "edge": 0.0, "unique_sample": 0}

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

    return {
        "image_ok": "yes",
        "mean": mean,
        "stddev": stddev,
        "edge": edge_sum / edge_count if edge_count else 0.0,
        "unique_sample": len(colors),
    }


def image_delta(a: Path, b: Path) -> float:
    ppm_a = read_ppm(a)
    ppm_b = read_ppm(b)
    if not ppm_a or not ppm_b or ppm_a[:2] != ppm_b[:2]:
        return 0.0
    pa, pb = ppm_a[2], ppm_b[2]
    count = min(len(pa), len(pb))
    return sum(abs(pa[i] - pb[i]) for i in range(count)) / count if count else 0.0


def classify(status: str, metrics: dict[str, str | float | int], delta: float) -> str:
    if status in {"LOAD_FAIL", "RUN_TIMEOUT", "EMULATOR_ABORT", "ZIP_EMPTY"}:
        return status
    if status == "CRASH":
        return "EMULATOR_CRASH"
    if status not in {"VISUAL", "PASS", "FAIL"}:
        return "RUNNER_ERROR"
    if metrics["image_ok"] != "yes":
        return "NO_SCREENSHOT"

    stddev = float(metrics["stddev"])
    edge = float(metrics["edge"])
    unique = int(metrics["unique_sample"])
    if unique <= 1 or stddev < 1.0:
        return "SUSPECT_BLANK"
    if unique <= 3 or (stddev < 6.0 and edge < 2.0):
        return "SUSPECT_LOW_DETAIL"
    if delta >= 2.5:
        return "OK_CHANGED"
    return "OK_STATIC"


def verdict_label(verdict: str) -> str:
    return VERDICT_LABELS.get(verdict, verdict.replace("_", " ").title())


def pct(count: int, total: int) -> str:
    return f"{(count * 100.0 / total):.1f}%" if total else "0.0%"


def run_gba_compat(
    rom: Path, cycles: int, ppm_path: Path, timeout: int, subprocess_timeout: int
) -> dict[str, str]:
    ppm_path.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        "./gba_compat_test",
        "--expect",
        "visual",
        "--max-cycles",
        str(cycles),
        "--ppm",
        str(ppm_path),
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
        return {"status": "RUN_TIMEOUT", "stdout": "", "stderr": "subprocess timeout", "ppm": "", "returncode": "124"}

    stdout = proc.stdout.strip()
    parts = stdout.split("\t")
    status = parts[0] if parts and parts[0] else "UNKNOWN"
    ppm = ""
    for part in parts:
        if part.startswith("ppm="):
            ppm = part[4:]
            break
    if not ppm and ppm_path.exists():
        ppm = str(ppm_path)
    if proc.returncode != 0 and status not in {"VISUAL", "PASS", "FAIL", "LOAD_FAIL", "RUN_TIMEOUT", "CRASH"}:
        status = "EMULATOR_ABORT"
    return {
        "status": status,
        "stdout": stdout,
        "stderr": proc.stderr.strip(),
        "ppm": ppm,
        "returncode": str(proc.returncode),
    }


def run_one(spec: tuple[Path, str], args: argparse.Namespace) -> dict[str, str]:
    source, entry = spec
    slug = sanitize_stem(source, entry)
    log_path = args.out / "logs" / f"{slug}.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)

    if source.suffix.casefold() == ".zip":
        rom = extract_zip_entry(source, entry, args.out / "extracted")
        if rom is None:
            row = empty_row(source, entry, "ZIP_EMPTY", log_path)
            log_path.write_text(f"source={source}\nzip_entry={entry}\nerror=no gba entry\n", encoding="utf-8")
            return row
    else:
        rom = source

    early_ppm = args.out / "screenshots" / slug / "early.ppm"
    final_ppm = args.out / "screenshots" / slug / "final.ppm"
    early = run_gba_compat(rom, args.early_cycles, early_ppm, args.timeout, args.timeout + 10)
    final = run_gba_compat(rom, args.cycles, final_ppm, args.timeout, args.timeout + 10)

    early_path = Path(early["ppm"]) if early["ppm"] else Path()
    final_path = Path(final["ppm"]) if final["ppm"] else Path()
    final_metrics = image_metrics(final_path) if final_path else image_metrics(Path(""))
    delta = image_delta(early_path, final_path) if early_path and final_path else 0.0
    verdict = classify(final["status"], final_metrics, delta)
    header = read_header(rom)

    log_path.write_text(
        "\n".join(
            [
                f"source={source}",
                f"zip_entry={entry}",
                f"extracted_rom={rom}",
                f"early_stdout={early['stdout']}",
                f"early_stderr={early['stderr']}",
                f"early_returncode={early['returncode']}",
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
        "source": str(source),
        "zip_entry": entry,
        "rom": str(rom),
        "title": header["title"],
        "game_code": header["game_code"],
        "maker": header["maker"],
        "unit_code": header["unit_code"],
        "device_type": header["device_type"],
        "version": header["version"],
        "complement": header["complement"],
        "rom_mib": header["rom_mib"],
        "early_status": early["status"],
        "final_status": final["status"],
        "early_returncode": early["returncode"],
        "final_returncode": final["returncode"],
        "early_ppm": str(early_path) if early_path else "",
        "final_ppm": str(final_path) if final_path else "",
        "delta": f"{delta:.3f}",
        "final_mean": f"{float(final_metrics['mean']):.3f}",
        "final_stddev": f"{float(final_metrics['stddev']):.3f}",
        "final_edge": f"{float(final_metrics['edge']):.3f}",
        "final_unique_sample": str(final_metrics["unique_sample"]),
        "log": str(log_path),
    }


def empty_row(source: Path, entry: str, verdict: str, log_path: Path) -> dict[str, str]:
    return {
        "verdict": verdict,
        "verdict_label": verdict_label(verdict),
        "source": str(source),
        "zip_entry": entry,
        "rom": "",
        "title": "",
        "game_code": "",
        "maker": "",
        "unit_code": "",
        "device_type": "",
        "version": "",
        "complement": "",
        "rom_mib": "",
        "early_status": "",
        "final_status": verdict,
        "early_returncode": "",
        "final_returncode": "",
        "early_ppm": "",
        "final_ppm": "",
        "delta": "0.000",
        "final_mean": "0.000",
        "final_stddev": "0.000",
        "final_edge": "0.000",
        "final_unique_sample": "0",
        "log": str(log_path),
    }


def report_fields() -> list[str]:
    return [
        "verdict",
        "verdict_label",
        "source",
        "zip_entry",
        "rom",
        "title",
        "game_code",
        "maker",
        "unit_code",
        "device_type",
        "version",
        "complement",
        "rom_mib",
        "early_status",
        "final_status",
        "early_returncode",
        "final_returncode",
        "delta",
        "final_mean",
        "final_stddev",
        "final_edge",
        "final_unique_sample",
        "early_ppm",
        "final_ppm",
        "log",
    ]


def write_reports(rows: list[dict[str, str]], out_dir: Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    with (out_dir / "gba_game_smoke.tsv").open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, report_fields(), delimiter="\t", extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)

    verdicts = Counter(row["verdict"] for row in rows)
    total = len(rows)
    good = sum(verdicts[key] for key in GOOD_VERDICTS)
    review = sum(verdicts[key] for key in REVIEW_VERDICTS)
    failed = sum(verdicts[key] for key in FAILED_VERDICTS)
    suspect = [row for row in rows if row["verdict"] not in GOOD_VERDICTS]

    lines = [
        "# GBA Game Smoke Summary",
        "",
        f"Total games: {total}",
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

    lines.extend(["", "## Suspect Games", ""])
    if not suspect:
        lines.append("- none")
    else:
        for row in suspect[:200]:
            entry = f" ! {row['zip_entry']}" if row["zip_entry"] else ""
            lines.append(
                f"- {row['verdict_label']} (`{row['verdict']}`): {row['source']}{entry} "
                f"(title={row['title']}, code={row['game_code']}, "
                f"stddev={row['final_stddev']}, edge={row['final_edge']}, "
                f"delta={row['delta']}, status={row['early_status']}->{row['final_status']}, "
                f"rc={row['early_returncode']}/{row['final_returncode']})"
            )
        if len(suspect) > 200:
            lines.append(f"- ... {len(suspect) - 200} more")

    (out_dir / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_progress_reports(rows: list[dict[str, str]], out_dir: Path) -> None:
    write_reports(sorted(rows, key=lambda row: (row["source"].casefold(), row["zip_entry"].casefold())), out_dir)


def main() -> int:
    args = parse_args()
    roots = args.rom_root if args.rom_root else [DEFAULT_ROM_ROOT]

    if args.early_cycles >= args.cycles:
        args.early_cycles = max(1, args.cycles // 10)

    if not args.no_build:
        subprocess.run(["make", "gba_compat_test"], check=True)

    if args.rom:
        paths = [rom for rom in args.rom if rom.is_file()]
        for rom in args.rom:
            if not rom.is_file():
                print(f"missing ROM: {rom}", file=sys.stderr, flush=True)
    else:
        paths = discover_roms(roots, args.pattern, args.include_extracted_duplicates)

    specs = selected_rom_specs(paths, args.all_zip_entries)
    if args.limit > 0:
        specs = specs[: args.limit]
    if not specs:
        print("No GBA ROMs found.", file=sys.stderr)
        return 2

    args.out.mkdir(parents=True, exist_ok=True)
    print(
        f"gba-game-smoke games={len(specs)} cycles={args.cycles} "
        f"early_cycles={args.early_cycles} out={args.out}",
        flush=True,
    )

    rows: list[dict[str, str]] = []
    if args.jobs <= 1:
        for index, spec in enumerate(specs, 1):
            row = run_one(spec, args)
            rows.append(row)
            write_progress_reports(rows, args.out)
            source, entry = spec
            suffix = f" ! {entry}" if entry else ""
            print(f"[{index}/{len(specs)}] {row['verdict']}\t{source}{suffix}", flush=True)
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            future_to_spec = {pool.submit(run_one, spec, args): spec for spec in specs}
            for index, future in enumerate(concurrent.futures.as_completed(future_to_spec), 1):
                spec = future_to_spec[future]
                source, entry = spec
                try:
                    row = future.result()
                except Exception as exc:  # pragma: no cover - defensive batch reporting
                    log_path = args.out / "logs" / f"{sanitize_stem(source, entry)}.log"
                    row = empty_row(source, entry, "RUNNER_ERROR", log_path)
                    print(f"runner error for {source}: {exc}", file=sys.stderr, flush=True)
                rows.append(row)
                write_progress_reports(rows, args.out)
                suffix = f" ! {entry}" if entry else ""
                print(f"[{index}/{len(specs)}] {row['verdict']}\t{source}{suffix}", flush=True)

    rows.sort(key=lambda row: (row["source"].casefold(), row["zip_entry"].casefold()))
    write_reports(rows, args.out)
    print(f"report={args.out / 'gba_game_smoke.tsv'}", flush=True)
    print(f"summary={args.out / 'summary.md'}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
