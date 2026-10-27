# NES Lua material

Shipped inside the core package. miniHawk itself contains nothing NES-specific,
so everything Lua-and-NES lives here.

This directory holds the `nes.*` Lua API and nothing else. BizHawk's collection
of per-game helper scripts (HUDs, hitbox overlays, RNG displays for a couple of
dozen specific titles) used to sit here and has been removed: a core package
ships the interface a core offers, not a library of scripts for individual
games. Anyone wanting those can take them from BizHawk, where they are
maintained.

- `nes.d.lua` — LuaCATS editor annotations for the `nes.*` Lua API. This is the
  API's declaration: what a script may call, and what it gets back.

A core package cannot provide a Lua library yet; when it can, these
declarations are what it would implement.
