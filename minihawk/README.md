# miniHawk material (retired package format)

This directory used to hold the QuickerNES core package for
[miniHawk](https://github.com/SergioMartin86/miniHawk): a managed adapter DLL
plus a prebuilt `libquicknes` native, described by a `minihawk-core.json`
manifest.

**That package format is gone.** miniHawk is waterbox-only: the only loadable
package is `core.wbx` (a sandboxed guest binary) plus `waterbox.config`, built
by [`../waterbox/`](../waterbox), which is where the live port lives. The
adapter source, the manifest, the prebuilt natives and the package build scripts
were deleted because nothing can load them any more; they remain in git history.

What is still here, and why:

- `native/` — `bizinterface.cpp` and its Makefile, which build `libquicknes`
  as an ordinary shared library. **Still load-bearing**: this is the *native
  reference* the waterbox equivalence gate runs against
  (`waterbox/run-native.c` dlopens `native/libquicknes.so`), proving the
  sandboxed core emulates identically to the unsandboxed one.
- `lua/` — the `nes.*` Lua API (declarations plus the recovered frontend-side
  implementation). Parked: a waterbox package has no way to ship a Lua API yet.
  See [`lua/README.md`](lua/README.md).
- `palettes/` — NES palettes. Parked for the same reason: no palette contract
  in the waterbox package format yet.
- `tests/` — the frontend replay witness (movies, goldens, Xvfb/Mono driver).
  **Live**: it now replays the suite against the waterbox package and compares
  against the same goldens it always used, which is what proves the sandboxed
  core emulates identically to the retired native one. See
  [`tests/README.md`](tests/README.md).

## Licensing and authorship

See [LICENSE](LICENSE) in this directory. In short:

- `native/bizinterface.cpp` is **derived from BizHawk** — original work by the
  **BizHawk team**, MIT License — with subsequent modifications for
  quickerNES/miniHawk under the same license.
- The quickerNES core itself is **GPL v2** (repository root LICENSE), so the
  compiled `libquicknes` binaries are distributed under GPL v2 as combined
  works.
- `palettes/` are NES palettes by various emulator authors, redistributed as
  previously shipped with BizHawk.
