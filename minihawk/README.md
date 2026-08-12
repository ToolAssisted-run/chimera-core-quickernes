# miniHawk core package

This directory is the QuickerNES core package for
[miniHawk](https://github.com/SergioMartin86/miniHawk) (the core-agnostic TAS
frontend derived from BizHawk). It is the *entire* interface between this
emulator and that frontend — miniHawk itself contains no quickerNES-specific
code.

Contents:

- `source/` — the managed adapter (`ICoreFactory` + `IEmulator` implementation
  and its NES-specific helpers: BootGod cart DB reader, virtual pad schemas,
  palettes)
- `native/` — `bizinterface.cpp` (the C ABI the adapter P/Invokes) and a
  Makefile that builds `libquicknes` from this repository's core sources
- `natives/` — prebuilt `libquicknes.dll` (MSVC) / `libquicknes.so` (gcc)
  shipped in the package
- `minihawk-core.json` — the package manifest (see miniHawk's core-author docs
  for the format)
- `NesCarts.xml`, `palettes/`, `defctrl.json` — data bundled into the package
  (cart DB, NES palettes, default input bindings)
- `lua/` — the `nes.*` Lua API: editor annotations plus the recovered
  frontend-side implementation (see `lua/README.md`)
- `build-package.ps1` — builds the adapter against the miniHawk contract DLLs
  and installs `quickernes.zip` into miniHawk's `build/Cores/`

## Building

```powershell
# prereq: a miniHawk checkout, solution built (dotnet build source/BizHawk.sln -c Release)
./build-package.ps1                       # assumes ../BizHawk sibling checkout
./build-package.ps1 -MiniHawkRoot <path>  # or point at it explicitly
```

The contract surface consumed here: `BizHawk.Emulation.Common.dll`,
`BizHawk.Common.dll`, `BizHawk.BizInvoke.dll`, and the
`BizHawk.SrcGen.SettingsUtil.dll` analyzer, all from `<MiniHawkRoot>/build/dll`.

miniHawk loads the package explicitly: `File > Open Core...` in the UI, or
`EmuHawk.exe --core=<path to quickernes.zip>` on the command line.

## Licensing and authorship

See [LICENSE](LICENSE) in this directory. In short:

- `source/` (the managed adapter), `native/bizinterface.cpp`, and
  `defctrl.json` are **derived from BizHawk** — original work by the
  **BizHawk team**, MIT License — with subsequent modifications for
  quickerNES/miniHawk under the same license.
- The quickerNES core itself is **GPL v2** (repository root LICENSE), so the
  compiled `libquicknes` binaries and the assembled `quickernes.zip` package
  are distributed under GPL v2 as combined works.
- `NesCarts.xml` is BootGod's NES Cart Database (NesDev community);
  `palettes/` are NES palettes by various emulator authors — both
  redistributed as previously shipped with BizHawk.

## Determinism obligations

The package must be frame-exact deterministic and savestate round-trip clean.
miniHawk's witness harness (`tests/` in that repo) replays a vendored
snapshot of this repository's `tests/` suite through the full frontend stack
and byte-compares final RAM against goldens, in both straight-replay and
per-frame-savestate modes. Run it after any change here.
