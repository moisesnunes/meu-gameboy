# Test Runners

This directory contains the reproducible checks for the Game Boy, Game Boy
Color, and Game Boy Advance cores. ROM bundles are local inputs and are not
committed to the repository; see [../roms/README.md](../roms/README.md) for
the expected layout.

| Directory | Purpose | Main command |
| --- | --- | --- |
| [compat/](compat/) | Manifest-based GB/GBC CPU, timing, audio, and visual compatibility checks | `make compat-run` |
| [gba_compat/](gba_compat/) | Manifest-based GBA CPU, DMA, timer, PPU, and save checks | `make gba-compat-run` |
| [games/](games/) | Commercial-ROM smoke tests for GB/GBC and GBA | `make game-smoke`, `make gba-game-smoke` |
| [shootout/](shootout/) | Game Boy Test ROMs v7.0 shootout and visual comparisons | `make shootout-run` |

Useful focused commands:

```sh
make blargg-run
make mooneye-run
make gba-memory-test
make shootout-list
make sm83-validate
```

Each runner writes its report below its own `out/` directory. The reports are
generated artifacts: rerunning a command replaces the corresponding default
report unless the runner is given a different output path.
