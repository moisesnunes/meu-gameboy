# GBEmulatorShootout-style tests

This runner builds a local battery from `roms/game-boy-test-roms-v7.0`, following
the same idea used by GBEmulatorShootout: each ROM is run headlessly, serial or
register-based tests are classified automatically, and screenshot-based tests
are compared against the PNG references shipped with the test bundle.

Useful commands:

```sh
make shootout-list
make shootout-run
make shootout-run SHOOTOUT_ARGS="--suite blargg --filter dmg_sound"
make shootout-run SHOOTOUT_ARGS="--profile all-local --suite gbmicrotest"
```

Outputs are written to `tests/shootout/out/`:

- `shootout.tsv`: full machine-readable result table
- `summary.md`: human-readable totals by suite
- `screenshots/`: captured PPMs for visual tests

The default `shootout` profile focuses on the suites represented in
GBEmulatorShootout and present in the v7.0 ROM bundle. Use
`--profile all-local` to include every local `.gb` / `.gbc` ROM discovered in
the bundle.
