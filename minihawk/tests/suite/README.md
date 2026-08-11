# Witness suite — provenance and licensing

This directory is a vendored snapshot of the `tests/` suite from
[quickerNES](https://github.com/SergioMartin86/quickerNES), pinned so that the
witness gate always runs against exactly what the goldens were recorded from.

- The `.sol` input sequences include movies from
  [TASVideos](https://tasvideos.org), copied into the quickerNES repository
  (and from there, here) with authorization under the
  **Creative Commons Attribution 2.0** license — authorship belongs to the
  original TAS authors credited on their TASVideos publication pages.
- `roms/` contains only freely-licensed homebrew ROMs, as in upstream
  quickerNES; all other ROMs referenced by the `.test` files must be
  provisioned locally (see the harness README) and are never committed.
- The `.test` definitions and `.state` files are from quickerNES
  (GPL v2, by Sergio Martín / eien86).
