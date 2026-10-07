# AGENTS.md - quickerNES core for Chimera

This repository is the quickerNES emulator (Nintendo Entertainment System)
and its port to Chimera, a frontend for tool-assisted speedruns. It produces
one file, `quickernes.chimeraCore`: the emulator built as a sandboxed guest
(`core.wbx`) plus the declarations Chimera reads. Chimera's sandbox, miniBox,
runs that guest on Linux and on Windows. The scripts say "miniHawk" where
they mean the Chimera checkout.

## Layout

- `source/quickerNES/core/` - the emulator. This repository's own source.
- `waterbox/waterbox.cpp` - the guest ABI layer over the emulator.
- `waterbox/waterbox.config` - what Chimera is told about the machine:
  video, audio, the controller, settings, memory layout.
- `waterbox/file_slots.json` - the files a project asks the user for.
- `waterbox/default_keybinds.json` - default key bindings.
- `waterbox/package-licenses.json` - the licence terms the package carries.
- `waterbox/setup-guest.sh` - configures the guest build.
- `waterbox/build-package.sh` - builds and installs the package.
- `waterbox/run-gate.sh` - the core gate; `run-native.c`, `run-wbx.c` and
  `run-tooling.c` are its drivers.
- `minihawk/native/bizinterface.cpp` - the `qn_*` C API of `libquicknes`, the
  native reference the gate compares against.
- `minihawk/tests/` - the frontend witness: `run-level-b.sh`, `movies/`,
  `goldens/`, `suite/`, `tools/make-movies.sh`.
- `meson.build` - one description, three builds: a cross configure is the
  guest, `-Dwaterbox=true` is the native reference and the drivers, a plain
  configure is the standalone tester.
- `extern/jaffarCommon/`, `source/quickNES/core/` - git submodules.
  `extern/hqn/` is vendored.
- `tests/` - the standalone tester's suite and the free roms.
- `.github/workflows/minihawk.yml` - the Chimera CI: core gate, frontend
  witness, publish. `make.yml` builds and tests the standalone tester.
- `build/` - every build tree. Ignored by git.

## Set up the build environment

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends meson ninja-build build-essential python3

git submodule update --init --recursive

CHIMERA=$HOME/chimera
[ -d "$CHIMERA" ] || git clone https://github.com/ToolAssisted-run/chimera.git "$CHIMERA"
git -C "$CHIMERA" submodule update --init extern/chimera-common-minibox
MB=$CHIMERA/extern/chimera-common-minibox

[ -f "$MB/build/meson-cpp/build.ninja" ] || meson setup "$MB/build/meson-cpp" "$MB" -Dguest_cpp=true
meson compile -C "$MB/build/meson-cpp"
```

The first miniBox build downloads the GCC source that matches the host gcc
(about 84 MB) to build libstdc++ for the guest. The frontend witness needs
more: Mono, Xvfb, the .NET SDK 8.0 and a built Chimera. See
`docs/BUILDING.md`.

## Build

The shortest path to a package is one command. It builds miniBox and the
guest when they are missing, and writes
`$CHIMERA/build/Cores/quickernes.chimeraCore`:

```sh
./waterbox/build-package.sh -r "$CHIMERA"

# the native reference and the guest, which the gates need
meson setup build/meson-native -Dwaterbox=true "-Dminibox_dir=$MB"
ninja -C build/meson-native
sh waterbox/setup-guest.sh -m "$MB"
ninja -C build/meson-guest core.wbx
```

## Install the core into Chimera

`build-package.sh -r "$CHIMERA"` installs it: `$CHIMERA/build/Cores/` is the
cores folder of a Chimera source checkout. For a release bundle, copy the
`.chimeraCore` file into the `Cores` folder beside `Chimera.exe` (or the
folder chosen in File > Core Manager > Change folder...). Chimera downloads
nothing. File > Core Manager lists the folder; Refresh List rescans it.

A package built by hand is stamped `<commit>+local` (`-dirty` when the tree
has changes) and is for testing. Only CI sets `CORE_VERSION` and publishes.

## Test before you commit

```sh
./waterbox/run-gate.sh                            # the core gate
CHIMERA_ROOT="$CHIMERA" ./waterbox/run-gate.sh    # the same, with ports:columns
(cd minihawk/tests && ./run-level-b.sh --minihawk-root "$CHIMERA")   # the frontend witness
```

The core gate must end with `0 failed`. It compares the sandboxed core with
`libquicknes.so` (video, audio, lag, memory), round-trips a savestate around
every frame, and checks turbo, the tooling exports and save data. Its
`ports:columns` leg skips unless `CHIMERA_ROOT` names a built Chimera that
has the package installed.

The frontend witness needs a built Chimera (`$CHIMERA/build/Chimera.exe`)
and the installed package. Both free-set tests must report PASS. CI
publishes only when the core gate and the witness are green. CI also runs
`make.yml`, the standalone tester's own tests.

## Rules of this repository

- There is no `patches/` directory and no apply-patches step. The emulator
  is this repository's own source: edit `source/quickerNES/core` in place.
  `extern/jaffarCommon` and `source/quickNES/core` are pinned submodules; do
  not commit inside them from here.
- Determinism is the product. The guest must not read host time, host
  randomness or anything else that differs between runs, and a savestate
  must round-trip. The gate checks it; a change that breaks it is a bug.
- The three `_QUICKERNES_*` definitions in `core_defs` (`meson.build`) must
  be the same for the guest and for `libquicknes`, or the gate compares two
  different emulators.
- Run the gate before committing. A new leg needs a negative control: break
  the thing it checks, watch it fail, and say so in the commit.
- When the controller declaration in `waterbox.config` changes, or what a
  port holds, regenerate the witness movies with
  `minihawk/tests/tools/make-movies.sh`, or every witness test desyncs.
- `run-level-b.sh --record` rewrites the witness goldens. They must match
  `minihawk/tests/goldens/native/`; do not re-record to make a test pass.
- Never commit game files, BIOS or firmware. The only roms in the tree are
  the free-to-distribute `sprilo.nes` and `nova.nes`. Never add network
  access.
- Every component compiled into the package is declared in
  `waterbox/package-licenses.json`.
- Shell scripts stay executable (git mode 100755). CI runs them directly.
- New documentation is plain ASCII. Some older files (`README.md`,
  `waterbox/README.md`) are not; leave them unless asked.
- Commit messages: a type and an optional scope (`fix(gate):`,
  `feat(package):`, `docs(issues):`, `ci:`), then a full sentence that states
  the outcome, such as `fix(ci): the nightly no longer cancels a push's
  gate`. The body says why and what was measured. A fix for a reported
  problem cites `ToolAssisted-run/chimera#N`: problems with this core are
  reported in the Chimera repository.
- Do not edit `.github/workflows` unless the task is the workflow. A push
  or a pull request starts the Chimera workflow only when it touches
  `waterbox/`, `minihawk/`, `source/`, `meson.build`, `meson_options.txt` or
  the workflow file.

## Where to read more

- `docs/BUILDING.md` - the full build, every option, troubleshooting.
- `waterbox/README.md` - the port's files and its settings.
- `minihawk/tests/README.md` - how the witness runs and what it covers.
- `.github/workflows/minihawk.yml` - the authoritative build recipe.
- In the Chimera repository: `docs/porting-a-core.md`, `docs/gates.md` and
  `docs/core-manager.md`.
