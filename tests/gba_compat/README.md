# GBA Compatibility Runner

`run.sh` executes the GBA manifest with `gba_compat_test`. The manifest is a
deliberately curated regression floor: ARM/Thumb execution, BIOS behavior,
memory, save media, DMA, timers, IRQ, and selected PPU timing checks.

## Commands

```sh
# Build and run the curated GBA compatibility manifest
make gba-compat-run

# Build only the headless test executable
make gba_compat_test

# Reproduce one ROM and emit its framebuffer
./gba_compat_test --expect visual --max-cycles 100000000 \
  --ppm /tmp/exact-timing.ppm \
  roms/game-boy-advance-test-roms/hw-test-master/archive/ppu/exact-timing/exact-timing.gba

# Include structured CPU, IRQ, PPU, DMA, timer, and cartridge state in output
./gba_compat_test --dump-state --expect visual --max-cycles 100000000 \
  roms/game-boy-advance-test-roms/gba-tests-master/memory/memory.gba
```

## Configuration and Reports

- `manifest.tsv` contains the curated ROM list. `{gba_root}` expands to
  `roms/game-boy-advance-test-roms` by default.
- `out/compat.txt` contains per-ROM results and the final total.
- `out/compat_summary.txt` adds expectations, screenshot hashes, and stderr.

The shell runner accepts these environment overrides:

```sh
GBA_COMPAT_ROOT=roms/game-boy-advance-test-roms make gba-compat-run
GBA_COMPAT_MANIFEST=tests/gba_compat/manifest.tsv make gba-compat-run
GBA_COMPAT_BIOS=bootroms/gba_bios.bin make gba-compat-run
GBA_COMPAT_MAX_CYCLES=100000000 make gba-compat-run
GBA_COMPAT_OUT=/tmp/gba-compat make gba-compat-run
```

Visual checks with a trusted manifest hash become `PASS` when the generated
framebuffer matches it. Unhashed visual output remains `VISUAL` for review.
