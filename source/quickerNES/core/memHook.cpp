// See memHook.hpp. Everything here is off until something installs a table.
#include "memHook.hpp"

namespace quickerNES
{

/* INVISIBLE in the guest (emulibc's .ldata.invis, which savestates skip). The
 * gate is a debugging tool, not machine state: a watched machine and an
 * unwatched one must save byte-identical states, and a state made while
 * watching must not quietly switch the hook on when it is loaded somewhere
 * else. Invisible memory also SURVIVES a load untouched, which is what keeps
 * the watches the host set still standing afterwards.
 *
 * The native build has no such section and no host to call, so there it is
 * ordinary memory that stays zero. */
#ifdef _QUICKERNES_MEMHOOK_INVISIBLE
#define MEMHOOK_STATE __attribute__((section(".ldata.invis")))
#else
#define MEMHOOK_STATE
#endif

MEMHOOK_STATE uint8_t memHookAny = 0;
MEMHOOK_STATE uint8_t *memHookTable = nullptr;
MEMHOOK_STATE uint8_t memHookWild = 0;
MEMHOOK_STATE memHookBridgeFn memHookBridge = nullptr;

uint32_t memHookFire(uint32_t addr, uint32_t value, uint32_t flags)
{
  if (memHookBridge == nullptr) return value;
  // the 5th argument is the scope index; this core has one, the CPU bus.
  const uint64_t answer = memHookBridge(memHookOpFire, addr, value, flags, 0, 0);
  // 0 means "leave the value alone". Anything else carries a replacement in
  // the low 32 bits, which is how a callback pokes a read on its way past.
  if (answer == 0) return value;
  return (uint32_t)(answer & 0xFFFFFFFFu);
}

} // namespace quickerNES
