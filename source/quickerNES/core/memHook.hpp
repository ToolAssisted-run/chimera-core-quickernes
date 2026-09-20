// The memory hook: the guest half of Chimera's memory callbacks.
//
// A debugger wants to be told when the machine touches an address. The naive
// way to do that across a sandbox is to call the host on every access and let
// the host decide, which costs a boundary crossing per 6502 cycle and makes a
// hooked run unusable. So the WATCHED ADDRESSES LIVE HERE, in the machine: the
// host tells the core what to watch, the core compares, and only a MATCH
// crosses the boundary.
//
// The table is one byte per CPU address - 64 KiB covers the whole 6502 space -
// so a lookup is a load and never a search. Before the load there is a gate:
// `memHookAny` is the OR of every flag anyone has asked for, and it is zero in
// every run that is not a debugging session. A run with no callbacks pays one
// byte load, one AND and one not-taken branch per access.
//
// The table itself is allocated by the waterbox layer in INVISIBLE memory, so
// it never enters a savestate: a machine that is being watched and one that is
// not produce byte-identical states. The native build leaves the pointer null
// and the gate zero, so nothing here runs at all.
#pragma once

#include <stdint.h>

namespace quickerNES
{

// What an access is. Same numbering as the host's (chimera engine.h).
enum memHookFlag : uint32_t
{
  memHookRead = 1,
  memHookWrite = 2,
  memHookExec = 4,
};

// The gate: the OR of every flag watched anywhere, zero when nothing is.
extern uint8_t memHookAny;

// One byte per CPU address, or null when nobody has installed a table.
extern uint8_t *memHookTable;

// Flags watched at EVERY address (the wildcard registrations).
extern uint8_t memHookWild;

// The host's dispatcher, as the sandbox hands it over: six integers in, one
// out. Null in the native build and until the host installs one.
typedef uint64_t (*memHookBridgeFn)(uint64_t op, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e);
extern memHookBridgeFn memHookBridge;

// op codes for the bridge
enum : uint64_t
{
  memHookOpFire = 1, // (addr, value, flags, scope) -> 0 to leave the value
                     // alone, else (1 << 32) | replacement
};

// Out of line on purpose: the hot path is the gate, and a call the branch
// predictor never takes costs nothing at all.
uint32_t memHookFire(uint32_t addr, uint32_t value, uint32_t flags);

// The hot path. Returns the value the machine should use, which is the value
// it was given unless a callback replaced it.
__attribute__((always_inline)) inline uint32_t memHookCheck(uint32_t addr, uint32_t value, uint32_t flags)
{
  if (__builtin_expect((memHookAny & flags) == 0, 1)) return value;
  const uint32_t watched = (uint32_t)memHookWild | (uint32_t)memHookTable[addr & 0xFFFF];
  if (__builtin_expect((watched & flags) == 0, 1)) return value;
  return memHookFire(addr, value, flags);
}

} // namespace quickerNES

// The forms the CPU's macros use. USABLE ONLY inside a function templated on
// `bool Hooks` (Cpu::runFlat, Cpu::runPaged): `Hooks` is a compile-time
// constant, so the copy compiled with Hooks == false contains no gate, no
// table load and no call - the feature is not switched off in it, it is not in
// it. Cpu::run picks the copy per call.
//
// `addr` is evaluated more than once, which is already the standing rule for
// every memory macro in cpuFlat.cpp.
#define MEMHOOK_R(addr, val) (Hooks ? quickerNES::memHookCheck((uint32_t)(addr), (uint32_t)(val), quickerNES::memHookRead) : (uint32_t)(val))
#define MEMHOOK_W(addr, val) (Hooks ? quickerNES::memHookCheck((uint32_t)(addr), (uint32_t)(val), quickerNES::memHookWrite) : (uint32_t)(val))
#define MEMHOOK_X(addr, val) (Hooks ? (void)quickerNES::memHookCheck((uint32_t)(addr), (uint32_t)(val), quickerNES::memHookExec) : (void)0)
