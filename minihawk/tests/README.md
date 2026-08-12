# QuickerNES witness harness for miniHawk

Lives in the quickerNES repository (miniHawk itself carries no core-specific
material). Both drivers locate the miniHawk checkout via `--minihawk-root` /
`-MiniHawkRoot` (default: sibling checkout named `miniHawk` or `BizHawk`).

The core under test is the **waterbox package** — `core.wbx` plus
`waterbox.config`, built by [`../../waterbox/build-package.sh`](../../waterbox)
and loaded through miniHawk's single generic adapter. The goldens predate that
port: they were recorded through the retired managed package and cross-validated
against the native tester's own dumps (`goldens/native/`). Reproducing them
byte-for-byte is precisely the proof that sandboxing changed no emulation.

The suite (`suite/*.test` + `.sol` input sequences) is a vendored snapshot of
`tests/` from [TASEmulators/quickerNES](https://github.com/TASEmulators/quickerNES).

## Which tests to run

| set | contents | when |
|---|---|---|
| `free` (**default**) | tests whose roms are free to distribute and vendored in `suite/roms` | every run, and in CI — it needs nothing provisioned |
| `full` | adds the commercial-rom tests, resolved from a local rom library | significant changes: the core, the guest ABI, the adapter, before a push |

```sh
./run-level-b.sh                    # free set, simple mode
./run-level-b.sh --set full         # the whole witness set
./run-level-b.sh --mode rerecord    # per-frame savestate round-trip variant
./run-level-b.sh --set full --parallel 26   # one instance per test, if you have the cores
./run-level-b.sh --record           # (re)record goldens from the current build
./run-level-b.sh --filter super     # subset by regex
```

`run-level-b.ps1` is the Windows equivalent (`-Set full`, `-Mode rerecord`, …);
it runs instances on a hidden Windows desktop instead of Xvfb, and
`hidden-run.ps1` is a standalone helper for one-off hidden runs.

The free set is currently one movie (`sprilo.anyPercent`). The other free rom,
`nova.nes`, is **rejected at load** by the core — "Unsupported mapper",
identically in the native and waterboxed flavors, so that is faithful behaviour
and not something this gate can cover. miniHawk's own synthetic witness
(`tests/synth/run-witness.sh` there) is fully redistributable and covers the
frontend contract; treat the two as complementary CI, and the `full` set as the
determinism gate.

## How a test runs

For each test: write a job file, launch EmuHawk with `--headless --lua=replay.lua`
on a hidden display (no window appears), replay the `.sol` through the frontend
input pipeline, and dump the final 2KB `RAM` domain. Up to `--parallel` of them
run concurrently, each with private config/job files. `--headless` makes any
modal dialog log its text and exit with code 64 instead of blocking invisibly.

Ports (Four Score, Arkanoid paddles) are **user settings** under the waterbox
package: the driver writes `port1`/`port2` into the config's sync settings, the
adapter mounts them as JSON, and the guest reads them at Init. The frontend
never learns what any of them mean.

Replay semantics that were required for byte-exact agreement with the native
tester (each found the hard way):

- `client.reboot_core()` at script start — EmuHawk emulates one frame during
  ROM load before Lua gains control.
- Console Reset/Power flags in `.sol` files are parsed but **not** applied — the
  native tester ignores them during replay (solarJetman has a reset).
- `OpposingDirPolicy` must be `Allow` (2) in the config — Lua input passes
  through the SOCD filter, and the movies use simultaneous L+R/U+D.
- Paddle values go through `joypad.setaxis`, which pushes an axis into the
  override adapter for the current frame. `joypad.setanalog` is a different
  thing — a sticky autohold — and never reaches the core from a script.

## Timing

Per-test wall clock is dominated by the frontend, not the core: the sandboxed
core alone runs ~2,600 fps, the full frontend ~450. The per-test timeout
defaults to 7200s because `superOffroad.anyPercent` is 182,180 frames and
`nigelMansell.anyPercent` is 296,590.

Goldens live in `goldens/levelB/`. A run passes when every dump is
byte-identical to its golden, and goldens must themselves match
`goldens/native/` (cross-validated whenever they are re-recorded).

## Witness set

26 tests run of 31; exclusions and rationale are listed in the drivers'
`excluded` list (mapper 5 disabled in the pinned fork, one rom the core rejects
at load, one wrong local dump, two tests that start from a quickerNES-native
`.state` and so are Level-A-only).
