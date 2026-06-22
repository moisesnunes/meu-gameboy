# Commercial-ROM Smoke Tests

The smoke runners load real games headlessly, capture early and final frames,
then classify the image progression. They are broad health checks, not proof
of full gameplay compatibility.

## GB and GBC

```sh
# Scan the default GB/GBC collection
make game-smoke

# Run one ROM without rebuilding rom_tester
./tests/games/run_game_smoke.py --no-build \
  --rom 'roms/game-boy-and-game-boy-color-complete-collection/Tetris (World).gb'

# Scan a different root in parallel and keep a separate report
./tests/games/run_game_smoke.py --rom-root roms/my-gb-set --jobs 4 \
  --out /tmp/gb-smoke
```

The default report directory is `out/`. It contains `game_smoke.tsv`,
`summary.md`, `logs/`, and early/probe/final screenshots. The runner normally
skips Blargg, Mooneye, and acid ROMs; use `--include-compat-roms` to include
them deliberately.

## GBA

```sh
# Scan the default GBA collection
make gba-game-smoke

# Run the fixed commercial probe list
./tests/games/run_gba_game_smoke.py --no-build \
  --rom-list tests/games/gba_commercial_probes.tsv \
  --early-cycles 1000000 --cycles 5000000 --timeout 10 --jobs 4 \
  --out /tmp/gba-probes

# Run one ROM
./tests/games/run_gba_game_smoke.py --no-build \
  --rom 'roms/GameboyAdvanceRomCollectionByGhostware/Metroid Fusion (E) [!].gba' \
  --out /tmp/gba-metroid
```

The GBA default report directory is `gba_out/`. It contains
`gba_game_smoke.tsv`, `summary.md`, logs, screenshots, and a cache for ROMs
extracted from ZIP archives. By default a ZIP contributes its preferred GBA
entry; use `--all-zip-entries` to test every entry.

## Verdicts

`OK_CHANGED` and `OK_STATIC` are likely-working signals. `SUSPECT_BLANK` and
`SUSPECT_LOW_DETAIL` require visual or state-based follow-up. Load errors,
timeouts, crashes, and runner errors are failures. The Markdown summary is the
compact status report; the TSV preserves metrics and paths for triage.
