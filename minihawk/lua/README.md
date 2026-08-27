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
- `api/NESLuaLibrary.cs` — the frontend-side implementation of that API,
  recovered from BizHawk. NOT currently compiled: it targets frontend internals
  rather than the core contract, and miniHawk does not yet support
  package-provided Lua API libraries. It is kept as the reference for wiring
  that up when the contract grows that capability.
