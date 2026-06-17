#!/usr/bin/env python3
"""Manifest-based compatibility runner for Gaembuoy."""

from __future__ import annotations

import argparse
import glob
import hashlib
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST = ROOT / "tests" / "compat" / "manifest.tsv"
DEFAULT_OUT = ROOT / "tests" / "compat" / "out"


@dataclass(frozen=True)
class TestCase:
    rom: Path
    mode: str
    expect: str
    max_cycles: int
    expected_sha1: str


def env_path(name: str, default: Path) -> Path:
    value = os.environ.get(name)
    return Path(value) if value else default


def die(message: str, status: int = 2) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(status)


def display_path(path: Path) -> str:
    try:
        return path.relative_to(ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def parse_int(value: str | None, default: int) -> int:
    if value is None or value == "":
        return default
    try:
        return int(value)
    except ValueError:
        die(f"invalid integer value: {value}")


def discover_manifest(manifest: Path) -> list[TestCase]:
    if not manifest.exists():
        die(f"compat manifest not found: {display_path(manifest)}")

    tests: list[TestCase] = []
    with manifest.open() as f:
        for line_no, raw in enumerate(f, start=1):
            line = raw.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            fields = line.split("\t")
            fields += [""] * (5 - len(fields))
            path_text, mode, expect, max_cycles, sha1 = fields[:5]
            mode = mode or "auto"
            expect = expect or "auto"
            cycles = parse_int(max_cycles, 100_000_000)

            if path_text.startswith("glob:"):
                pattern = path_text.removeprefix("glob:")
                for match in sorted(glob.glob(str(ROOT / pattern), recursive=True)):
                    tests.append(TestCase(Path(match), mode, expect, cycles, sha1))
            elif path_text:
                rom = ROOT / path_text
                if rom.exists():
                    tests.append(TestCase(rom, mode, expect, cycles, sha1))
            else:
                die(f"empty ROM path in {display_path(manifest)}:{line_no}")
    return tests


def maybe_build(compat_bin: Path) -> None:
    if compat_bin.exists():
        return
    subprocess.run(["make", "compat_test"], cwd=ROOT, check=True)


def clean_out_dir(out_dir: Path, result_file: Path, summary_file: Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    keep = {result_file.resolve(), summary_file.resolve()}
    for child in out_dir.iterdir():
        if child.is_file() and child.resolve() not in keep:
            child.unlink()


def ppm_sha1(path: Path) -> str:
    h = hashlib.sha1()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def parse_status(output: str) -> str:
    return (output.split("\t", 1)[0] or "UNKNOWN").strip() or "UNKNOWN"


def safe_stem(path: Path) -> str:
    return "".join(ch if ch.isalnum() or ch in "._-" else "_" for ch in path.stem) or "rom"


def run_one(
    test: TestCase,
    compat_bin: Path,
    tmp_dir: Path,
    max_override: int | None,
    visual_override: int | None,
) -> tuple[str, str, str, str, str]:
    max_cycles = max_override if max_override is not None else test.max_cycles
    if test.expect == "visual" and visual_override is not None:
        max_cycles = visual_override

    ppm = tmp_dir / f"{safe_stem(test.rom)}.ppm"
    cmd = [
        str(compat_bin),
        "--mode",
        test.mode,
        "--expect",
        test.expect,
        "--max-cycles",
        str(max_cycles),
    ]
    if test.expect == "visual":
        cmd += ["--ppm", str(ppm)]
    cmd.append(str(test.rom))

    proc = subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True)
    output = proc.stdout.strip()
    stderr = proc.stderr.strip()
    status = parse_status(output)
    final_status = status
    sha1 = ""

    if test.expect == "visual" and ppm.exists():
        sha1 = ppm_sha1(ppm)
        if test.expected_sha1 and sha1 != test.expected_sha1:
            final_status = "FAIL"

    return final_status, output, stderr, sha1, str(max_cycles)


def write_summary_header(summary_file: Path, manifest: Path, result_file: Path) -> None:
    with summary_file.open("w") as f:
        f.write("Gaembuoy compatibility summary\n")
        f.write(f"manifest={display_path(manifest)}\n")
        f.write(f"result={display_path(result_file)}\n\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, default=env_path("COMPAT_MANIFEST", DEFAULT_MANIFEST))
    parser.add_argument("--out", type=Path, default=env_path("COMPAT_OUT", DEFAULT_OUT))
    parser.add_argument("--result", type=Path, default=os.environ.get("COMPAT_RESULT"))
    parser.add_argument("--summary", type=Path, default=os.environ.get("COMPAT_SUMMARY"))
    parser.add_argument("--max-cycles", type=int, default=parse_int(os.environ.get("COMPAT_MAX_CYCLES"), 0) or None)
    parser.add_argument("--visual-cycles", type=int, default=parse_int(os.environ.get("COMPAT_VISUAL_CYCLES"), 0) or None)
    parser.add_argument("--no-build", action="store_true")
    args = parser.parse_args()

    manifest = (ROOT / args.manifest).resolve() if not args.manifest.is_absolute() else args.manifest.resolve()
    out_dir = (ROOT / args.out).resolve() if not args.out.is_absolute() else args.out.resolve()
    args.result = Path(args.result) if args.result else args.out / "compat.txt"
    args.summary = Path(args.summary) if args.summary else args.out / "compat_summary.txt"
    result_file = (ROOT / args.result).resolve() if not args.result.is_absolute() else args.result.resolve()
    summary_file = (ROOT / args.summary).resolve() if not args.summary.is_absolute() else args.summary.resolve()
    compat_bin = ROOT / "compat_test"

    if not args.no_build:
        maybe_build(compat_bin)
    if not compat_bin.exists() or not os.access(compat_bin, os.X_OK):
        die("compat_test is missing; run `make compat_test` first or omit --no-build")

    tests = discover_manifest(manifest)
    clean_out_dir(out_dir, result_file, summary_file)
    result_file.parent.mkdir(parents=True, exist_ok=True)
    summary_file.parent.mkdir(parents=True, exist_ok=True)
    result_file.write_text("")
    write_summary_header(summary_file, manifest, result_file)

    totals = {"PASS": 0, "FAIL": 0, "TIMEOUT": 0, "VISUAL": 0, "UNKNOWN": 0}
    with tempfile.TemporaryDirectory(prefix="gaembuoy-compat.") as tmp:
        tmp_dir = Path(tmp)
        for test in tests:
            final_status, output, stderr, sha1, max_cycles = run_one(
                test,
                compat_bin,
                tmp_dir,
                args.max_cycles,
                args.visual_cycles,
            )
            totals[final_status] = totals.get(final_status, 0) + 1
            if test.expect == "visual":
                line = f"{final_status}\t{display_path(test.rom)}\tsha1={sha1}\t{output}\n"
            else:
                line = f"{output}\n"
            print(line, end="")
            with result_file.open("a") as f:
                f.write(line)

            with summary_file.open("a") as f:
                f.write(f"[{final_status}] {display_path(test.rom)}\n")
                f.write(f"  mode={test.mode} expect={test.expect} max_cycles={max_cycles}\n")
                if test.expect == "visual":
                    f.write(f"  visual_sha1={sha1 or 'none'}\n")
                    if test.expected_sha1:
                        f.write(f"  expected_sha1={test.expected_sha1}\n")
                f.write(f"  output={output}\n")
                if stderr:
                    f.write("  stderr:\n")
                    for line in stderr.splitlines():
                        f.write(f"    {line}\n")
                f.write("\n")

    total = len(tests)
    summary = (
        f"compat TOTAL={total} PASS={totals.get('PASS', 0)} FAIL={totals.get('FAIL', 0)} "
        f"TIMEOUT={totals.get('TIMEOUT', 0)} VISUAL={totals.get('VISUAL', 0)} "
        f"UNKNOWN={totals.get('UNKNOWN', 0)} result={display_path(result_file)}"
    )
    print(summary)
    with result_file.open("a") as f:
        f.write(f"{summary}\n")
    with summary_file.open("a") as f:
        f.write("Totals\n")
        f.write(f"  total={total}\n")
        for key in ("PASS", "FAIL", "TIMEOUT", "VISUAL", "UNKNOWN"):
            f.write(f"  {key.lower()}={totals.get(key, 0)}\n")
        f.write(f"\n{summary}\n")

    failed = totals.get("FAIL", 0) + totals.get("TIMEOUT", 0) + totals.get("UNKNOWN", 0)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
