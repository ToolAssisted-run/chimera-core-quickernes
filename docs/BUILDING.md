# Building the quickerNES core

This repository builds the quickerNES emulator (Nintendo Entertainment
System) as a core for Chimera. The result is one file,
`quickernes.chimeraCore`, which Chimera loads. The steps below are the ones
this repository's CI runs on a fresh clone
(`.github/workflows/minihawk.yml`); when this document and the workflow
disagree, the workflow is right.

Placeholders used below:

- `<repo>` - the checkout of this repository. Commands run from `<repo>`
  unless a block says otherwise.
- `<chimera>` - a checkout of https://github.com/ToolAssisted-run/chimera.
- `<miniBox>` - `<chimera>/extern/chimera-common-minibox`, a git submodule
  of Chimera. It is the sandbox host and the guest toolchain.

The scripts, the workflow and some directory names say "miniHawk". They mean
the Chimera checkout: the workflow's "Check out miniHawk" step checks out
`ToolAssisted-run/chimera`, and `-r <miniHawk root>` takes `<chimera>`.

## Requirements

Cores are built on Linux, x86-64. CI uses GitHub's `ubuntu-latest` runner.

For the package and the core gate (workflow job `core-gate`):

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends meson ninja-build build-essential python3
```

For the frontend witness, which builds all of Chimera (workflow job
`frontend-witness`):

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  meson ninja-build build-essential cmake pkg-config python3 \
  mono-complete xvfb \
  libgl1-mesa-dev libx11-dev libxext-dev libasound2-dev
```

The frontend witness also needs the .NET SDK 8.0. The workflow installs it
with `actions/setup-dotnet@v4` and `dotnet-version: '8.0'`. By hand, Chimera's
README gives the command and says why the distribution's SDK is not enough:

```sh
curl -sSL https://dot.net/v1/dotnet-install.sh | bash -s -- --channel 8.0
```

No compiler version is pinned. The build uses the gcc and g++ that
`build-essential` installs, and the package records that gcc version in its
`build.json`.

What the build fetches or builds by itself:

- The first miniBox build with `-Dguest_cpp=true` downloads the GCC source
  release that matches the host gcc (about 84 MB, with `curl`), and builds
  libstdc++ for the guest from it with `tar` and `make`. It needs network
  access once. This is in `<miniBox>/meson.build`.
- `waterbox/build-package.sh` configures and builds that miniBox tree, and
  configures the guest build, when they are not there yet.

## Get the sources

This repository, with its submodules. The workflow uses `actions/checkout@v6`
with `submodules: recursive`:

```sh
git clone --recursive https://github.com/ToolAssisted-run/chimera-core-quickernes.git
```

In an existing clone:

```sh
git submodule update --init --recursive
```

The submodules are `extern/jaffarCommon` and `source/quickNES/core`. The
Chimera core compiles `source/quickerNES/core` and `waterbox/waterbox.cpp`,
with `extern/jaffarCommon/include` on the include path.

A Chimera checkout. CI builds against Chimera's `main` branch
(`MINIHAWK_REF: main`). The package and the core gate need only the miniBox
submodule:

```sh
git clone https://github.com/ToolAssisted-run/chimera.git <chimera>
cd <chimera>
git submodule update --init extern/chimera-common-minibox
```

The frontend witness builds Chimera itself, and for that the workflow checks
Chimera out with every submodule:

```sh
cd <chimera>
git submodule update --init --recursive
```

Where each script looks for Chimera when it is not told:

| script | option | fallback |
|---|---|---|
| `waterbox/build-package.sh` | `-r <chimera>`, `-m <miniBox>` (or `MINIBOX_DIR`) | `../chimera` beside `<repo>`, `$HOME/chimera`, `../miniHawk` beside `<repo>`, `$HOME/miniHawk`; miniBox defaults to `<chimera>/extern/chimera-common-minibox` |
| `waterbox/setup-guest.sh` | `-m <miniBox>` (or `MINIBOX_DIR`) | `$HOME/chimera/extern/chimera-common-minibox` |
| `meson.build`, native build | `-Dminibox_dir=<miniBox>` | `../chimera/extern/chimera-common-minibox` beside `<repo>`, else an error |
| `waterbox/run-gate.sh` | `CHIMERA_ROOT=<chimera>` | three directories above `<repo>` |
| `minihawk/tests/run-level-b.sh` | `--minihawk-root <chimera>` | a directory named `miniHawk` or `BizHawk` beside `<repo>` |

The fallbacks differ from script to script. Pass the paths.

## Build miniBox

The host library and the C++ guest toolchain (musl plus libstdc++ built for
the sandbox). This core is C++, so it needs the `meson-cpp` flavor:

```sh
mb=<chimera>/extern/chimera-common-minibox
[ -f "$mb/build/meson-cpp/build.ninja" ] || meson setup "$mb/build/meson-cpp" "$mb" -Dguest_cpp=true
meson compile -C "$mb/build/meson-cpp"
```

The workflow caches `<miniBox>/build/meson-cpp` with `actions/cache@v4`,
because the guest toolchain is the slow part of the build and changes only
when miniBox does. By hand, keep the directory: the test in the first line
skips the configure when it is already there.

## Build the core

There are no patches. This repository has no `patches/` directory and no
apply-patches step: the emulator is this repository's own source, under
`source/quickerNES/core`, and `waterbox/waterbox.cpp` is the guest ABI layer
over it.

One `meson.build` describes both builds. A cross configure is the guest; a
native configure with `-Dwaterbox=true` is the reference and the gate's
drivers.

The native reference and the drivers (needed by the gates, not by the
package):

```sh
meson setup build/meson-native -Dwaterbox=true "-Dminibox_dir=$mb"
ninja -C build/meson-native
```

This builds `libquicknes.so`, `run-native`, `run-wbx` and `run-tooling` in
`build/meson-native`. `libquicknes.so` is the original shared library with
the `qn_*` C API (`minihawk/native/bizinterface.cpp`). It is what the gate
compares the sandboxed core against. `run-native` loads it; `run-wbx` and
`run-tooling` run the guest through miniBox's host library.

The guest:

```sh
sh waterbox/setup-guest.sh -m "$mb"
ninja -C build/meson-guest core.wbx
```

`setup-guest.sh` writes the cross file `build/guest-cross.ini` and configures
`build/meson-guest`. The cross file holds absolute paths of this machine; it
is under `build/`, which git ignores. The result is
`build/meson-guest/core.wbx`.

Three compile definitions must be the same in both builds, or the gate
compares two different emulators: `_QUICKERNES_ENABLE_TRACEBACK_SUPPORT`,
`_QUICKERNES_SUPPORT_ARKANOID_INPUTS` and `_QUICKERNES_DETECT_JOYPAD_READS`.
They are set once, in `core_defs` in `meson.build`.

## Build the package

```sh
./waterbox/build-package.sh -r <chimera>
```

Options:

- `-r <chimera>` - the Chimera checkout the package is written into.
- `-m <miniBox>` - the miniBox checkout. Default:
  `<chimera>/extern/chimera-common-minibox`. `MINIBOX_DIR` does the same.
- `-o <dir>` - where the staging directory `package-staging` is made.
  Default: `<repo>/build`. It does not move the package.

What the script does, in order:

1. Configures `<miniBox>/build/meson-cpp` with `-Dguest_cpp=true` if it is not
   configured, and builds it.
2. Runs `waterbox/setup-guest.sh` if `build/meson-guest` is not configured,
   builds `core.wbx`, and checks it with miniBox's
   `source/guest/check-wbx.sh`.
3. Stages `core.wbx`, `waterbox.config`, `default_keybinds.json`,
   `file_slots.json` and the licence files that
   `waterbox/package-licenses.json` declares.
4. Stamps the version into the staged `waterbox.config` and writes
   `build.json` (source commit, toolchain, miniBox commit).
5. Writes `<chimera>/build/Cores/quickernes.chimeraCore`, packs it a second
   time and stops if the two files differ.
6. Removes `<chimera>/build/CoreCache/quickernes-*`, so Chimera does not load
   an older extraction.

It does not build the native reference; the package does not need it.

The version of a package is the commit it was built from. CI passes
`CORE_VERSION` (the full commit) and publishes that package. A package built
by hand, without `CORE_VERSION`, is stamped `<12-character commit>+local`,
or `<commit>-dirty+local` when `git diff --quiet HEAD` finds changes in the
tree. `versionDate` is the date of the commit in UTC, never the date of the
build. Hand-built packages are for testing: Chimera's publishing script
refuses a version that carries `+local` or `-dirty`.

## Install it into Chimera

Chimera ships no cores and downloads nothing: it has no network code. A core
gets into Chimera because somebody puts the file there.

- In a Chimera source checkout the cores folder is `<chimera>/build/Cores/`.
  `build-package.sh -r <chimera>` writes the package there, so there is
  nothing else to do.
- In a release bundle, copy `quickernes.chimeraCore` into the `Cores` folder
  beside `Chimera.exe` (or into the folder chosen in File > Core Manager >
  Change folder...).
- File > Core Manager lists what is in that folder. Refresh List rescans it.

The same package file works on Linux and on Windows. The guest inside it is
run by Chimera's sandbox (miniBox) on either.

Packages built by CI are on this repository's Releases page,
https://github.com/ToolAssisted-run/chimera-core-quickernes/releases : a
rolling `dev` release and dated `nightly-YYYY-MM-DD` releases. The asset is
named `quickernes-<version>.chimeraCore`. Download it and put it in the
`Cores` folder.

To use the core: File > New Project... and pick it. To play a rom with no
project, start Chimera with `--core=<package> <rom>`.

## Run the gates

There are two gates. CI needs both green before it publishes.

### The core gate

```sh
./waterbox/run-gate.sh
```

It needs `build/meson-native` (the native reference and the drivers) and
`build/meson-guest/core.wbx`, and nothing else: no .NET, no Mono, no X.

```
./waterbox/run-gate.sh [-n <native build dir>] [-g <guest build dir>] [-f <frames>] [rom...]
```

The default is 600 frames over the two roms in the repository that are free
to distribute: `tests/roms/sprilo.nes` and
`minihawk/tests/suite/roms/nova.nes`. Other roms can be given by path; a
path that is not there reports SKIP.

| leg | what it proves |
|---|---|
| `<rom>:equivalence` | video, audio, lag and memory-domain digests are identical between `libquicknes.so` and `core.wbx`, under the same per-frame button pattern |
| `<rom>:savestate` | saving and loading the whole machine around every frame (`run-wbx --rerecord`) changes no digest |
| `<rom>:turbo` | with drawing off for the first half of the run (`run-wbx --turbo`), the machine, the sound, the lag count and the pictures of the second half are unchanged - and the whole-run video hash differs, so the frames really went undrawn |
| `<rom>:tooling` | every tooling family (surfaces, registers, buses, trace) answers and lists something |
| `ports:columns` | one recorded frame has the input columns of the peripherals the settings plugged in, and no others |
| `savedata:export`, `savedata:seeded`, `savedata:refused` | battery save data leaves through the save-data channel, a supplied save reaches the cartridge and comes back, and a cartridge with no battery refuses save data |

`ports:columns` runs Chimera's headless runner. It needs
`<chimera>/build/meson-linux/chimera-run` and the installed package
`<chimera>/build/Cores/quickernes.chimeraCore`, and reports SKIP without
them:

```sh
CHIMERA_ROOT=<chimera> ./waterbox/run-gate.sh
```

In CI this leg skips, on purpose: the `core-gate` job builds neither Chimera
nor the package. Run it by hand against a built checkout.

The last line reads `N ok, N failed, N skipped`. Only a FAIL makes the exit
code non-zero.

### The frontend witness

It replays real movies through the whole frontend and compares the final RAM
with goldens recorded from the native core. It needs a built Chimera and the
installed package. Build Chimera as the workflow does:

```sh
cd <chimera>
meson setup build/meson-linux --prefix "$PWD/build" --libdir dll
meson compile -C build/meson-linux
meson install -C build/meson-linux
dotnet build source/gui/Chimera.sln -c Release /nodeReuse:false -p:UseSharedCompilation=false
```

Then, from `<repo>`:

```sh
./waterbox/build-package.sh -r <chimera>
cd minihawk/tests
./run-level-b.sh --minihawk-root <chimera>
```

For each test it starts `<chimera>/build/Chimera.exe` under Mono with
`--headless`, the package, the test's committed movie
(`movies/<test>.chimeraProject`) and the rom, plays the movie to its end, and
compares the final 2 KB of RAM byte for byte with
`goldens/levelB/<test>.simple.ram.bin`. With `DISPLAY` unset it starts its
own Xvfb; with `DISPLAY` set it uses that display.

The default set is `free`: `sprilo.anyPercent` and
`novaTheSquirrel.anyPercent`, whose roms are in `suite/roms`. This is what CI
runs. It does not test savestates; the core gate does.

`--set full` adds the tests over commercial roms. Roms are looked for in
`suite/roms` and then in `$HOME/TAS/roms/nes`. A test reports SKIP when its
rom is not found, when the rom's SHA-1 is not the one the test expects, or
when it has no movie. Only the free set's movies are committed; the others
are made with `tools/make-movies.sh --set full`, which needs a built Chimera
and the staged package.

Other options: `--filter <regex>`, `--parallel <n>` (default 8),
`--timeout <seconds>` per test (default 7200), `--record` (write goldens
from the current build instead of comparing), `--skip-existing`. Logs and
dumps go to `minihawk/tests/work/`, which git ignores.

### What CI runs

`.github/workflows/minihawk.yml` runs `core-gate` and `frontend-witness` on
pull requests and pushes to `main` that touch `waterbox/**`, `minihawk/**`,
`source/**`, `meson.build`, `meson_options.txt` or the workflow file; every
day at 04:00 UTC; and on manual dispatch. `publish` runs when both jobs
passed and the event is not a pull request. A push publishes the rolling
`dev` release. The scheduled run publishes `nightly-YYYY-MM-DD`, and only
when `main` moved since the last nightly. A manual run takes `kind`, `dev` or
`nightly`; blank means `dev`.

A second workflow, `.github/workflows/make.yml`, builds the repository's
standalone tester and runs its tests on every pull request and push to
`main`. It is not part of the package. On a fresh clone it runs:

```sh
mkdir build
python3 -m pip install meson ninja
sudo apt-get update
sudo apt-get install -y libgtest-dev gcovr libtbb-dev libsdl2-dev libsdl2-image-dev
meson setup build -DonlyOpenSource=true -DenableArkanoidInputs=true
ninja -C build
ninja test -C build
```

## Files the core needs at run time

Game files are never in this repository or in the package. The user provides
them. From `waterbox/file_slots.json` and `waterbox/waterbox.config`:

- A cartridge image in iNES format (`.nes`). Required, exactly one.
- Save data (`.sav` or `.srm`). Optional, at most one. It is the cartridge's
  battery-backed RAM, the file Emulator > Export Save Data... writes. A
  cartridge with no battery keeps no saves and refuses one.

This core declares no firmware: no BIOS is needed.

The two `.nes` files in the repository (`sprilo.nes`, `nova.nes`) are free to
distribute homebrew. They are there for the gates.

## Troubleshooting

- `miniHawk checkout not found; pass -r <path>` (`build-package.sh`) or
  `pass --minihawk-root <path>` (`run-level-b.sh`): pass the Chimera
  checkout. The two scripts do not look in the same places.
- `miniBox C++ guest toolchain missing under <miniBox>/build/meson-cpp`
  (`setup-guest.sh`): build miniBox with `-Dguest_cpp=true` first. The plain
  miniBox build has no libstdc++ for the guest.
- `pass -Dminibox_dir=<miniBox checkout>` (meson, native build): the
  fallback is `../chimera` beside `<repo>`; give the path.
- `could not download the GCC <version> source from any mirror` (miniBox
  build): the C++ guest toolchain needs network access the first time.
- `native build missing` or `guest build missing` (`run-gate.sh`): the gate
  builds nothing. Build both flavors first; the message prints the commands.
- `ports:columns SKIP needs chimera-run and a built package`: set
  `CHIMERA_ROOT` to a Chimera checkout that is built and has the package in
  `build/Cores`.
- `packaging is not deterministic` (`build-package.sh`): two packings of the
  same staging directory differed. The package's SHA-1 is the core's
  identity, so the script stops.
- `core package not found at ...` (`run-level-b.sh`): run
  `waterbox/build-package.sh -r <chimera>` first.
- `Xvfb not found (apt install xvfb), and no DISPLAY set`
  (`run-level-b.sh`): install `xvfb`.
- A witness test fails with `no meta produced`: read
  `minihawk/tests/work/run/<test>.log`. A first run that cannot make its
  configuration says `bootstrap failed`; read
  `minihawk/tests/work/run/bootstrap.log`.
- Every witness test desyncs at once: the controller declaration in
  `waterbox.config`, or what a port holds, changed and the movies were not
  regenerated. Run `minihawk/tests/tools/make-movies.sh`.
- The Chimera checkout moved: run `waterbox/setup-guest.sh` again. The cross
  file holds absolute paths; the script rewrites it and reconfigures.
