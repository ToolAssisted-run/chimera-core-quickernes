# NES Lua material

Shipped inside the core package. miniHawk itself contains nothing NES-specific,
so everything Lua-and-NES lives here:

- `*.lua` — game-specific helper/overlay scripts (HUDs, hitboxes, RNG displays)
  for various NES titles, inherited from BizHawk's script collection (MIT, by
  the BizHawk team and script contributors). Load them from miniHawk's Lua
  console alongside the matching game.
- `nes.d.lua` — LuaCATS editor annotations for the `nes.*` Lua API.
- `api/NESLuaLibrary.cs` — the frontend-side implementation of the `nes.*` Lua
  API, recovered from BizHawk. NOT currently compiled: it targets frontend
  internals rather than the core contract, and miniHawk does not yet support
  package-provided Lua API libraries. It is kept here as the reference for
  wiring that up when the contract grows that capability.
