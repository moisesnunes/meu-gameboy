# GB/GBC Compatibility Runner

`run_compat.py` executes the manifest in `manifest.tsv` with the headless
`compat_test` binary. It covers Blargg, Mooneye, and selected visual ROMs for
the Game Boy and Game Boy Color cores.

## Commands

```sh
# Build and run the complete manifest
make compat-run

# Run only the Blargg rows from the main manifest
make blargg-run

# Run the dedicated Mooneye manifest
make mooneye-run
```

For an exact ROM repro, invoke the binary directly:

```sh
make compat_test
./compat_test --mode dmg --expect blargg --max-cycles 503316480 \
  roms/blargg-test-roms-master/oam_bug/rom_singles/7-timing_effect.gb
```

## Manifests and Outputs

- `manifest.tsv`: combined Blargg, Mooneye, and selected visual checks.
- `mooneye.tsv`: Mooneye-only subset.
- `ppu_manifest.tsv`: optional PPU-focused manifest.
- `out/compat.txt`: one line per ROM plus combined and per-suite totals.
- `out/compat_summary.txt`: the same run with command output and stderr.

The Python runner recognizes `COMPAT_SUITE=blargg` or `COMPAT_SUITE=mooneye`.
It also accepts `COMPAT_MANIFEST`, `COMPAT_OUT`, `COMPAT_RESULT`,
`COMPAT_SUMMARY`, `COMPAT_MAX_CYCLES`, and `COMPAT_VISUAL_CYCLES` for custom
runs. For example:

```sh
COMPAT_MAX_CYCLES=100000000 make compat-run
COMPAT_MANIFEST=tests/compat/ppu_manifest.tsv make compat-run
COMPAT_OUT=/tmp/gb-compat make compat-run
```

`PASS` is a confirmed success. `VISUAL` means a screenshot was generated but
the manifest has no trusted expected hash. `FAIL`, `TIMEOUT`, and `UNKNOWN`
make the command fail.
