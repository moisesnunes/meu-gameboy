#!/usr/bin/env python3
"""Local GBEmulatorShootout-style runner for Gaembuoy."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

from PIL import Image, ImageChops


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_ROM_ROOT = ROOT / "roms" / "game-boy-test-roms-v7.0"
DEFAULT_OUT = ROOT / "tests" / "shootout" / "out"
FRAME_CYCLES = 70224

SHOOTOUT_SUITES = {
    "blargg",
    "bully",
    "cgb-acid-hell",
    "cgb-acid2",
    "daid",
    "dmg-acid2",
    "mbc3-tester",
    "mealybug-tearoom-tests",
    "mooneye-test-suite",
    "rtc3test",
    "same-suite",
    "strikethrough",
}

SUITE_LABELS = {
    "blargg": "Blargg",
    "bully": "Bully",
    "cgb-acid-hell": "CGB Acid Hell",
    "cgb-acid2": "CGB Acid2",
    "daid": "daid",
    "dmg-acid2": "DMG Acid2",
    "mbc3-tester": "MBC3 Tester",
    "mealybug-tearoom-tests": "Mealybug Tearoom",
    "mooneye-test-suite": "Mooneye",
    "mooneye-test-suite-wilbertpol": "Mooneye Wilbertpol",
    "rtc3test": "rtc3test",
    "same-suite": "SameSuite",
    "strikethrough": "Strikethrough",
}

SKIPPED_TESTS: set[str] = set()


@dataclass(frozen=True)
class TestCase:
    name: str
    suite: str
    rom: Path
    rel: str
    model: str
    expect: str
    max_cycles: int
    refs: tuple[Path, ...] = field(default_factory=tuple)


def die(message: str, status: int = 2) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(status)


def normalized_name(path: Path, rom_root: Path) -> str:
    rel = path.relative_to(rom_root).as_posix()
    stem = rel.rsplit(".", 1)[0]
    return stem.replace("_", " ")


def suite_for(rel: str) -> str:
    return rel.split("/", 1)[0]


def infer_models(path: Path, rel: str, refs_by_model: dict[str, tuple[Path, ...]]) -> list[str]:
    name = path.name.lower()
    stem = path.stem.lower()
    stem_orig = path.stem
    if rel.startswith("blargg/cgb_sound/"):
        return ["gbc"]
    if rel == "blargg/interrupt_time/interrupt_time.gb":
        return ["gbc"]
    if rel.startswith("blargg/oam_bug/"):
        return ["dmg"]
    if rel.startswith("same-suite/") and not rel.startswith("same-suite/sgb/"):
        return ["gbc"]
    if refs_by_model and set(refs_by_model) != {"auto"}:
        return sorted(refs_by_model)
    # Detect hardware-specific model from filename suffix (before the general sgb check)
    if re.search(r"[-_]dmg0$", stem):
        return ["dmg0"]
    if re.search(r"[-_]mgb$", stem):
        return ["mgb"]
    if re.search(r"[-_]sgb2$", stem) or re.search(r"2-S$", stem_orig):
        return ["sgb2"]
    if re.search(r"[-_]sgb$", stem) or re.search(r"-S$", stem_orig):
        return ["sgb"]
    if re.search(r"[-_]cgb0$|[-_]cgbABCDE$|[-_]cgb$", path.stem):
        return ["gbc"]
    # -C suffix = cgb+agb+ags group (mooneye naming convention)
    if re.search(r"-C$", stem_orig):
        return ["gbc"]
    if rel.startswith("same-suite/sgb/") or "-sgb" in name or "_sgb" in name:
        return ["sgb"]
    if path.suffix.lower() == ".gbc" or "cgb" in name or "gbc" in name:
        return ["gbc"]
    return ["dmg"]


def ref_model(path: Path) -> str | None:
    s = path.stem.lower()
    if re.search(r"(^|[-_.])dmg|dmg[0-9]|xdmg|_dmg_blob", s):
        return "dmg"
    if re.search(r"(^|[-_.])cgb|cgb[0-9]|gbc|xcgb", s):
        return "gbc"
    if re.search(r"(^|[-_.])sgb", s):
        return "sgb"
    return None


def looks_like_reference_suffix(suffix: str) -> bool:
    tokens = [t for t in re.split(r"[-_.]+", suffix.lower()) if t]
    for token in tokens:
        if token in {"a", "b", "c", "d", "e", "blob", "rev"}:
            continue
        if re.fullmatch(r"[0-9]+", token):
            continue
        if re.fullmatch(r"out[0-9a-f]+", token):
            continue
        if re.fullmatch(r"x?dmg[0-9a-z]*", token):
            continue
        if re.fullmatch(r"x?cgb[0-9a-z]*", token):
            continue
        if re.fullmatch(r"gbc[0-9a-z]*", token):
            continue
        if re.fullmatch(r"sgb[0-9a-z]*", token):
            continue
        if re.fullmatch(r"ncm[0-9a-z]*", token):
            continue
        return False
    return True


def find_refs(rom: Path) -> dict[str, tuple[Path, ...]]:
    stem = rom.stem
    exact = rom.with_suffix(".png")
    refs: dict[str, list[Path]] = {}
    if exact.exists():
        refs.setdefault("auto", []).append(exact)

    for png in sorted(rom.parent.glob(f"{stem}*.png")):
        if png == exact:
            continue
        rest = png.stem[len(stem):]
        if rest and rest[0] not in "-_.":
            continue
        if not looks_like_reference_suffix(rest):
            continue
        model = ref_model(png)
        if model:
            refs.setdefault(model, []).append(png)
    if "auto" in refs and len(refs) > 1:
        refs.pop("auto")
    return {k: tuple(v) for k, v in refs.items()}


def infer_expect(suite: str, refs: tuple[Path, ...]) -> str:
    if suite == "daid" and not refs:
        return "info"
    if suite == "blargg":
        return "blargg"
    if suite in {
        "mooneye-test-suite",
        "mooneye-test-suite-wilbertpol",
        "same-suite",
    }:
        return "fib"
    if suite == "gbmicrotest":
        return "gbmicrotest"
    if refs:
        return "visual"
    if suite == "age-test-roms":
        return "fib"
    return "auto"


def infer_cycles(rel: str, expect: str) -> int:
    if expect in {"visual", "info"}:
        return 120 * FRAME_CYCLES
    if expect == "gbmicrotest":
        return 2_000_000
    if rel in {
        "blargg/cpu_instrs/cpu_instrs.gb",
    }:
        return 250_000_000
    if rel == "blargg/oam_bug/rom_singles/7-timing_effect.gb":
        return 503_316_480
    if rel in {
        "blargg/dmg_sound/dmg_sound.gb",
        "blargg/cgb_sound/cgb_sound.gb",
        "blargg/oam_bug/oam_bug.gb",
    }:
        return 503_316_480
    if rel.startswith("blargg/dmg_sound/") or rel.startswith("blargg/cgb_sound/"):
        return 150_000_000
    if rel.startswith("blargg/"):
        return 100_000_000
    if rel.startswith("rtc3test/"):
        return 90_000_000
    return 100_000_000


def discover_tests(rom_root: Path, profile: str, include_manual: bool, include_caution: bool) -> list[TestCase]:
    tests: list[TestCase] = []
    for rom in sorted([*rom_root.rglob("*.gb"), *rom_root.rglob("*.gbc")]):
        rel = rom.relative_to(rom_root).as_posix()
        suite = suite_for(rel)
        if rel in SKIPPED_TESTS:
            continue
        if profile == "shootout" and suite not in SHOOTOUT_SUITES:
            continue
        if not include_manual and "/manual-only/" in f"/{rel}":
            continue
        if not include_caution and "/caution/" in f"/{rel}":
            continue

        refs_by_model = find_refs(rom)
        for model in infer_models(rom, rel, refs_by_model):
            refs = refs_by_model.get(model) or refs_by_model.get("auto") or ()
            expect = infer_expect(suite, refs)
            tests.append(
                TestCase(
                    name=f"{normalized_name(rom, rom_root)} ({model.upper()})",
                    suite=suite,
                    rom=rom,
                    rel=rel,
                    model=model,
                    expect=expect,
                    max_cycles=infer_cycles(rel, expect),
                    refs=refs,
                )
            )
    return tests


def compare_image(actual: Path, expected: Path) -> bool:
    a = Image.open(actual).convert(mode="L", dither=Image.Dither.NONE)
    b = Image.open(expected).convert(mode="L", dither=Image.Dither.NONE)
    if a.size != b.size:
        return False
    diff = ImageChops.difference(a, b)
    for count, value in diff.getcolors(maxcolors=256) or []:
        if value > 50:
            return False
    return True


def parse_status(output: str) -> str:
    first = output.split("\t", 1)[0].strip()
    return first or "UNKNOWN"


def safe_slug(text: str) -> str:
    text = re.sub(r"[^A-Za-z0-9._-]+", "_", text)
    return text.strip("._-")[:160] or "test"


def display_path(path: Path) -> str:
    try:
        return path.relative_to(ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def run_one(test: TestCase, compat_bin: Path, out_dir: Path, host_timeout: float) -> dict[str, str]:
    screenshot = out_dir / "screenshots" / f"{safe_slug(test.name)}.ppm"
    screenshot.parent.mkdir(parents=True, exist_ok=True)
    compat_expect = "visual" if test.expect == "info" else test.expect
    cmd = [
        str(compat_bin),
        "--mode",
        test.model,
        "--expect",
        compat_expect,
        "--max-cycles",
        str(test.max_cycles),
    ]
    if test.expect in {"visual", "info"}:
        cmd += ["--ppm", str(screenshot)]
    cmd.append(str(test.rom))

    started = time.monotonic()
    try:
        proc = subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True, timeout=host_timeout)
    except subprocess.TimeoutExpired as exc:
        elapsed = time.monotonic() - started
        return {
            "status": "TIMEOUT",
            "suite": SUITE_LABELS.get(test.suite, test.suite),
            "name": test.name,
            "model": test.model.upper(),
            "expect": test.expect,
            "rom": test.rom.relative_to(ROOT).as_posix(),
            "refs": ",".join(ref.relative_to(ROOT).as_posix() for ref in test.refs),
            "matched_ref": "",
            "cycles": str(test.max_cycles),
            "seconds": f"{elapsed:.3f}",
            "exit_code": "124",
            "output": (exc.stdout or "").strip() if isinstance(exc.stdout, str) else "",
            "stderr": "host timeout after %.1fs" % host_timeout,
        }
    elapsed = time.monotonic() - started
    output = proc.stdout.strip()
    stderr = proc.stderr.strip()
    status = parse_status(output)
    matched_ref = ""

    if test.expect == "info":
        status = "INFO"
    elif test.expect == "visual" and screenshot.exists():
        status = "FAIL"
        for ref in test.refs:
            if compare_image(screenshot, ref):
                status = "PASS"
                matched_ref = ref.relative_to(ROOT).as_posix()
                break
    elif status == "VISUAL":
        status = "UNKNOWN"

    return {
        "status": status,
        "suite": SUITE_LABELS.get(test.suite, test.suite),
        "name": test.name,
        "model": test.model.upper(),
        "expect": test.expect,
        "rom": test.rom.relative_to(ROOT).as_posix(),
        "refs": ",".join(ref.relative_to(ROOT).as_posix() for ref in test.refs),
        "matched_ref": matched_ref,
        "cycles": str(test.max_cycles),
        "seconds": f"{elapsed:.3f}",
        "exit_code": str(proc.returncode),
        "output": output,
        "stderr": stderr,
    }


def write_reports(rows: list[dict[str, str]], out_dir: Path, rom_root: Path, profile: str) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    tsv = out_dir / "shootout.tsv"
    summary = out_dir / "summary.md"
    fields = [
        "status",
        "suite",
        "name",
        "model",
        "expect",
        "rom",
        "refs",
        "matched_ref",
        "cycles",
        "seconds",
        "exit_code",
        "output",
        "stderr",
    ]
    with tsv.open("w", newline="") as f:
        writer = csv.DictWriter(f, fields, delimiter="\t")
        writer.writeheader()
        writer.writerows(rows)

    totals: dict[str, int] = {}
    by_suite: dict[str, dict[str, int]] = {}
    for row in rows:
        status = row["status"]
        suite = row["suite"]
        totals[status] = totals.get(status, 0) + 1
        by_suite.setdefault(suite, {})
        by_suite[suite][status] = by_suite[suite].get(status, 0) + 1

    total = len(rows)
    passed = totals.get("PASS", 0)
    pct = (passed * 100.0 / total) if total else 0.0

    with summary.open("w") as f:
        f.write("# Gaembuoy Shootout Summary\n\n")
        f.write(f"- profile: `{profile}`\n")
        f.write(f"- rom_root: `{rom_root.relative_to(ROOT).as_posix()}`\n")
        f.write(f"- total: {total}\n")
        f.write(f"- pass: {passed} ({pct:.1f}%)\n")
        for key in sorted(totals):
            if key != "PASS":
                f.write(f"- {key.lower()}: {totals[key]}\n")
        f.write(f"- results: `{display_path(tsv)}`\n\n")
        f.write("## By Suite\n\n")
        f.write("| Suite | Total | PASS | FAIL | TIMEOUT | INFO | UNKNOWN | LOAD_FAIL |\n")
        f.write("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n")
        for suite in sorted(by_suite):
            data = by_suite[suite]
            suite_total = sum(data.values())
            f.write(
                f"| {suite} | {suite_total} | {data.get('PASS', 0)} | "
                f"{data.get('FAIL', 0)} | {data.get('TIMEOUT', 0)} | "
                f"{data.get('INFO', 0)} | {data.get('UNKNOWN', 0)} | "
                f"{data.get('LOAD_FAIL', 0)} |\n"
            )


def maybe_build(compat_bin: Path) -> None:
    if compat_bin.exists():
        return
    subprocess.run(["make", "compat_test"], cwd=ROOT, check=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rom-root", type=Path, default=DEFAULT_ROM_ROOT)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--profile", choices=["shootout", "all-local"], default="shootout")
    parser.add_argument("--suite", action="append", help="Run only suites whose path prefix matches this value")
    parser.add_argument("--filter", action="append", help="Run only tests whose name or path contains this value")
    parser.add_argument("--max-tests", type=int, help="Limit the number of tests after filtering")
    parser.add_argument("--list", action="store_true", help="Print test names without running")
    parser.add_argument("--dump-tests-json", type=Path, help="Write discovered tests as JSON")
    parser.add_argument("--include-manual", action="store_true")
    parser.add_argument("--include-caution", action="store_true")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--host-timeout", type=float, default=20.0,
                        help="Wall-clock seconds before one ROM is killed")
    args = parser.parse_args()

    rom_root = args.rom_root.resolve()
    if not rom_root.exists():
        die(f"ROM root not found: {rom_root}")

    tests = discover_tests(rom_root, args.profile, args.include_manual, args.include_caution)
    if args.suite:
        tests = [t for t in tests if any(t.suite.startswith(s) for s in args.suite)]
    if args.filter:
        needles = [n.lower() for n in args.filter]
        tests = [
            t for t in tests
            if all(n in t.name.lower() or n in t.rel.lower() for n in needles)
        ]
    if args.max_tests is not None:
        tests = tests[:args.max_tests]

    if args.dump_tests_json:
        args.dump_tests_json.parent.mkdir(parents=True, exist_ok=True)
        with args.dump_tests_json.open("w") as f:
            json.dump([
                {
                    "name": t.name,
                    "suite": SUITE_LABELS.get(t.suite, t.suite),
                    "model": t.model.upper(),
                    "expect": t.expect,
                    "rom": t.rom.relative_to(ROOT).as_posix(),
                    "refs": [r.relative_to(ROOT).as_posix() for r in t.refs],
                    "max_cycles": t.max_cycles,
                }
                for t in tests
            ], f, indent=2)

    if args.list:
        for t in tests:
            refs = f" refs={len(t.refs)}" if t.refs else ""
            print(f"{t.name}\t{SUITE_LABELS.get(t.suite, t.suite)}\t{t.expect}{refs}\t{t.rom.relative_to(ROOT).as_posix()}")
        print(f"TOTAL\t{len(tests)}")
        return 0

    compat_bin = ROOT / "compat_test"
    if not args.no_build:
        maybe_build(compat_bin)
    if not compat_bin.exists() or not os.access(compat_bin, os.X_OK):
        die("compat_test is missing; run `make compat_test` first or omit --no-build")

    if args.out.exists():
        shutil.rmtree(args.out)
    args.out.mkdir(parents=True, exist_ok=True)

    rows: list[dict[str, str]] = []
    for index, test in enumerate(tests, start=1):
        print(f"[{index}/{len(tests)}] {test.name}", flush=True)
        row = run_one(test, compat_bin, args.out, args.host_timeout)
        rows.append(row)
        print(f"  {row['status']} {row['seconds']}s", flush=True)

    write_reports(rows, args.out, rom_root, args.profile)
    failed = sum(1 for row in rows if row["status"] in {"FAIL", "TIMEOUT", "UNKNOWN", "LOAD_FAIL"})
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
