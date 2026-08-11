# QuickerNES witness harness for miniHawk

Lives in the quickerNES repository (miniHawk itself carries no core-specific
material). Both drivers locate the miniHawk checkout via --minihawk-root /
-MiniHawkRoot (default: sibling checkout named miniHawk or BizHawk).

Correctness gate for the miniHawk project (see docs/design-principles.md in the miniHawk repository).
The quickerNES regression suite (`suite/*.test` + `.sol` input sequences - a
vendored snapshot of `tests/` from
[TASEmulators/quickerNES](https://github.com/TASEmulators/quickerNES), which
this repo no longer carries as a submodule) must pass at every phase boundary,
at two levels:

## Level A - native core guard

Runs the quickerNES native tester over every test and compares final-RAM
MetroHashes against `goldens/levelA-hashes.txt`. Validates that the native core
payload never drifts. Built and run under WSL:

```
# one-time setup (WSL): clone TASEmulators/quickerNES + ROMs, then
meson setup build -DenableArkanoidInputs=true   # + -Wno-unused-but-set-variable on GCC 13+
ninja -C build
```

`native/dumper.cpp` is a small companion tool (added to the same meson build)
that writes the raw 2KB low-mem instead of a hash - its output is the ground
truth that Level B dumps are byte-compared against (`goldens/native/`).

## Level B - full-stack witness (the real one)

Two equivalent drivers, one per host OS. Both: for each test, write a job file,
launch EmuHawk with `--headless --lua=replay.lua` on a hidden display (no window
appears), replay the `.sol` through the frontend input pipeline, and dump the
final 2KB `RAM` domain. Up to N EmuHawk instances run concurrently, each with
private config/job files. `--headless` makes any modal dialog log its text and
exit with code 64 instead of blocking invisibly on the hidden display.

- **Windows**: `run-level-b.ps1` - instances run on a hidden Windows desktop.
  `hidden-run.ps1` is a standalone helper for one-off hidden runs (diagnostics).
- **Linux**: `run-level-b.sh` - EmuHawk runs under Mono on a private Xvfb
  display (`apt install xvfb`). On first ever run the driver bootstraps its
  config via `bootstrap.lua`. The goldens are shared with the Windows driver:
  Linux runs reproduce the Windows-recorded goldens byte-exactly (proven
  2026-08-11, 26/26 simple + 26/26 rerecord).

Replay semantics that were required for byte-exact agreement with the native
tester (each found the hard way; see miniHawk's docs/design-principles.md "Phase 0 discoveries"):

- `client.reboot_core()` at script start - EmuHawk emulates one frame during
  ROM load before Lua gains control.
- Console Reset/Power flags in `.sol` files are parsed but **not** applied - 
  the native tester ignores them during replay (solarJetman has a reset).
- `OpposingDirPolicy` must be `Allow` (2) in the config - Lua input passes
  through the SOCD filter, and the movies use simultaneous L+R/U+D.
- Arkanoid paddle input is delivered via `joypad.setfrommnemonicstr` - 
  `joypad.setanalog` axis holds never reach the output controller in this
  build.

```
.\run-level-b.ps1                  # verify current build against goldens
.\run-level-b.ps1 -Record          # (re)record goldens
.\run-level-b.ps1 -Mode rerecord   # per-frame savestate save/load variant (IStatable)
.\run-level-b.ps1 -Filter super    # subset by regex

./run-level-b.sh                   # Linux equivalents of the above
./run-level-b.sh --record
./run-level-b.sh --mode rerecord
./run-level-b.sh --filter super
```

For fast smoke checks and CI, use miniHawk's own synthetic witness
(`tests/synth/run-witness.sh` in the miniHawk repository, ~10 seconds) - this
suite is the full determinism gate, not a smoke test. Commits require the
full 26/26 in both modes.

Goldens live in `goldens/levelB/`. A run passes when every dump is
byte-identical to its golden, and goldens must themselves match
`goldens/native/` (cross-validated whenever they are re-recorded).

ROMs are resolved from `suite/roms/` first, then the host's ROM library
(`C:\Users\sergiom\Documents\TAS\roms\nes` on Windows, `~/TAS/roms/nes` on
Linux), and are SHA1-verified against each `.test` before running.

## Witness set

31 tests total; exclusions and rationale are listed in miniHawk's docs/design-principles.md (mapper 5
disabled in the pinned fork, one pinned-core mapper-30 serializeState crash,
one wrong local dump, two initial-`.state` tests that are Level-A-only).
