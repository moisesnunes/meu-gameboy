# Compatibility ROM Runner

Compatibility ROMs live under `roms/`. The deterministic suite is described by
`tests/compat/manifest.tsv`; entries may point to a single ROM or to a
`glob:` pattern. Run:

```sh
make compat-run
```

By default, `tests/compat/run.sh` uses `tests/compat/manifest.tsv`. The manifest
keeps the suite deterministic and lets each ROM declare its hardware mode,
timeout, expectation type, and optional visual SHA-1.

The runner also writes a compact result file to `tests/compat/out/compat.txt`
and prints a final summary like:

```text
compat TOTAL=60 PASS=53 FAIL=4 TIMEOUT=1 VISUAL=2 UNKNOWN=0 result=tests/compat/out/compat.txt
```

The runner builds `./compat_test` and supports the common result styles:

- Blargg-style serial output: passes when the serial log contains `Passed`, fails on `Failed`.
- Blargg-style tilemap output: also detects `Passed`/`Failed` written as text on the background tilemap.
- Mooneye-style register signature: passes when `B C D E H L` are `03 05 08 0d 15 22`.
- Visual tests such as `dmg-acid`/`cgb-acid`: writes a `.ppm` screenshot under `tests/compat/out/`. If the manifest provides a SHA-1, the screenshot is checked automatically; otherwise the runner prints the generated SHA-1 for a later trusted reference.

Useful overrides:

```sh
COMPAT_MAX_CYCLES=100000000 make compat-run
COMPAT_VISUAL_CYCLES=70224000 make compat-run
COMPAT_MANIFEST=tests/compat/manifest.tsv make compat-run
COMPAT_RESULT=/tmp/compat.txt make compat-run
```

You can also run a single ROM directly:

```sh
./compat_test --mode dmg --expect blargg --max-cycles 503316480 roms/blargg-test-roms-master/cpu_instrs/cpu_instrs.gb
./compat_test --mode dmg --expect visual --ppm tests/compat/out/dmg-acid2.ppm roms/dmg-acid2.gb
```

`compat_test` clears in-memory cartridge RAM/RTC/EEPROM after loading and
disables cartridge save writes for the run, so compatibility checks do not
modify the `.sav` files next to local ROMs.
