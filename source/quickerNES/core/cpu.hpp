#pragma once

// NES 6502 CPU emulator
// Emu 0.7.0

#include <stdint.h>
#include <string.h>
#include <limits.h>

namespace quickerNES
{

typedef long nes_time_t;     // clock cycle count
typedef unsigned nes_addr_t; // 16-bit address

#ifdef _QUICKERNES_DETECT_BAD_ACCESS
// Bad-access detector (glitch investigation). "Bad access" = the CPU fetches an instruction that
// legitimate game code never executes: an unofficial/undocumented 6502 opcode, or an opcode fetched
// from RAM/registers ($0000-$7FFF). Either means control flow has derailed into data-as-code -- the
// hallmark of the U+X action-pointer glitch. Emulator-independent at the detection boundary (the
// opcode byte at a given PC is deterministic; only the later *semantics* of unofficial ops diverge
// between cores), and it needs no training set, so novel-but-legit exploration cannot false-positive.
// isOfficialOpcode[b] == 1 for the 151 documented NMOS 6502 opcodes, 0 for the other 105.
inline constexpr uint8_t cpu_isOfficialOpcode[256] = {
  // built from the documented opcode set; 1 = official, 0 = unofficial
  /*0x00*/ 1,1,0,0,0,1,1,0, 1,1,1,0,0,1,1,0, // BRK ORA -   -   -   ORA ASL -   PHP ORA ASL -   -   ORA ASL -
  /*0x10*/ 1,1,0,0,0,1,1,0, 1,1,0,0,0,1,1,0, // BPL ORA -   -   -   ORA ASL -   CLC ORA -   -   -   ORA ASL -
  /*0x20*/ 1,1,0,0,1,1,1,0, 1,1,1,0,1,1,1,0, // JSR AND -   -   BIT AND ROL -   PLP AND ROL -   BIT AND ROL -
  /*0x30*/ 1,1,0,0,0,1,1,0, 1,1,0,0,0,1,1,0, // BMI AND -   -   -   AND ROL -   SEC AND -   -   -   AND ROL -
  /*0x40*/ 1,1,0,0,0,1,1,0, 1,1,1,0,1,1,1,0, // RTI EOR -   -   -   EOR LSR -   PHA EOR LSR -   JMP EOR LSR -
  /*0x50*/ 1,1,0,0,0,1,1,0, 1,1,0,0,0,1,1,0, // BVC EOR -   -   -   EOR LSR -   CLI EOR -   -   -   EOR LSR -
  /*0x60*/ 1,1,0,0,0,1,1,0, 1,1,1,0,1,1,1,0, // RTS ADC -   -   -   ADC ROR -   PLA ADC ROR -   JMP ADC ROR -
  /*0x70*/ 1,1,0,0,0,1,1,0, 1,1,0,0,0,1,1,0, // BVS ADC -   -   -   ADC ROR -   SEI ADC -   -   -   ADC ROR -
  /*0x80*/ 0,1,0,0,1,1,1,0, 1,0,1,0,1,1,1,0, // -   STA -   -   STY STA STX -   DEY -   TXA -   STY STA STX -
  /*0x90*/ 1,1,0,0,1,1,1,0, 1,1,1,0,0,1,0,0, // BCC STA -   -   STY STA STX -   TYA STA TXS -   -   STA -   -
  /*0xA0*/ 1,1,1,0,1,1,1,0, 1,1,1,0,1,1,1,0, // LDY LDA LDX -   LDY LDA LDX -   TAY LDA TAX -   LDY LDA LDX -
  /*0xB0*/ 1,1,0,0,1,1,1,0, 1,1,1,0,1,1,1,0, // BCS LDA -   -   LDY LDA LDX -   CLV LDA TSX -   LDY LDA LDX -
  /*0xC0*/ 1,1,0,0,1,1,1,0, 1,1,1,0,1,1,1,0, // CPY CMP -   -   CPY CMP DEC -   INY CMP DEX -   CPY CMP DEC -
  /*0xD0*/ 1,1,0,0,0,1,1,0, 1,1,0,0,0,1,1,0, // BNE CMP -   -   -   CMP DEC -   CLD CMP -   -   -   CMP DEC -
  /*0xE0*/ 1,1,0,0,1,1,1,0, 1,1,1,0,1,1,1,0, // CPX SBC -   -   CPX SBC INC -   INX SBC NOP -   CPX SBC INC -
  /*0xF0*/ 1,1,0,0,0,1,1,0, 1,1,0,0,0,1,1,0, // BEQ SBC -   -   -   SBC INC -   SED SBC -   -   -   SBC INC -
};
#endif

class Cpu
{
  public:

  void set_tracecb(void (*cb)(unsigned int *data))
  {
    tracecb = cb;
  }

  // Per-instruction trace hook (only invoked in _QUICKERNES_ENABLE_TRACEBACK_SUPPORT builds). Must be
  // value-initialized: an indeterminate pointer here made traceback builds call garbage during boot.
  void (*tracecb)(unsigned int *dest) = nullptr;

  // NES 6502 registers. *Not* kept updated during a call to run().
  struct registers_t
  {
    uint16_t pc; // Should be more than 16 bits to allow overflow detection -- but I (eien86) removed it to maximize performance.
    uint8_t a;
    uint8_t x;
    uint8_t y;
    uint8_t status;
    uint8_t sp;
  };

  // Map code memory (memory accessed via the program counter). Start and size
  // must be multiple of page_size.
  enum
  {
    page_bits = 11
  };
  enum
  {
    page_count = 0x10000 >> page_bits
  };
  enum
  {
    page_size = 1L << page_bits
  };

  inline void set_code_page(int i, uint8_t const *p)
  {
    uint8_t const *newBase = p - (unsigned)i * page_size;
    // In flat mode the page contents are mirrored into flat_code_map. PRG ROM is
    // read-only, so re-mapping a page to the pointer it already holds would copy
    // identical bytes. Skip that redundant memcpy (the common case for games that
    // rewrite the same bank register, and for apply_mapping on every state load).
    if (_useFlatCodeMap == true && code_map[i] != newBase)
      memcpy(&flat_code_map[i*page_size], p, page_size);
    code_map[i] = newBase;
  }

  inline void map_code(nes_addr_t start, unsigned size, const void *data)
  {
    unsigned first_page = start / page_size;
    for (unsigned i = size / page_size; i--;)
      set_code_page(first_page + i, (uint8_t *)data + i * page_size);
  }

  inline void flattenCodePages()
  {
    for (unsigned int i = 0; i < page_count + 1; i++)
    {
      const uint8_t* srcPointer = code_map[i];
      srcPointer += i * page_size;
      memcpy(&flat_code_map[i*page_size], srcPointer, page_size);
    }
  }

  inline void useFlatCodeMap() { _useFlatCodeMap = true; }
  inline void usePagedCodeMap() { _useFlatCodeMap = false; }

  // Access memory as the emulated CPU does.
  int read(nes_addr_t);
  void write(nes_addr_t, int data);

  // Push a byte on the stack
  inline void push_byte(int data)
  {
    int sp = r.sp;
    r.sp = (sp - 1) & 0xFF;
    low_mem[0x100 + sp] = data;
  }

  // Reasons that run() returns
  enum result_t
  {
    result_cycles, // Requested number of cycles (or more) were executed
    result_sei,    // I flag just set and IRQ time would generate IRQ now
    result_cli,    // I flag just cleared but IRQ should occur *after* next instr
    result_badop   // unimplemented/illegal instruction
  };

  // This optimization is only possible with the GNU compiler -- MSVC does not allow function alignment
#if defined(__GNUC__) || defined(__clang__)
  result_t runPaged(nes_time_t end_time) __attribute__((aligned(1024)));
#else
  result_t runPaged(nes_time_t end_time);
#endif

#if defined(__GNUC__) || defined(__clang__)
  result_t runFlat(nes_time_t end_time) __attribute__((aligned(1024)));
#else
  result_t runFlat(nes_time_t end_time);
#endif

  inline result_t run(nes_time_t end_time) 
  {
   if (_useFlatCodeMap == true) return runFlat(end_time);
   return runPaged(end_time);
  }

  nes_time_t time() const { return clock_count; }

  inline void reduce_limit(int offset)
  {
    clock_limit -= offset;
    end_time_ -= offset;
    irq_time_ -= offset;
  }

  inline void set_end_time_(nes_time_t t)
  {
    end_time_ = t;
    update_clock_limit();
  }

  inline void set_irq_time_(nes_time_t t)
  {
    irq_time_ = t;
    update_clock_limit();
  }

  unsigned long error_count() const { return error_count_; }

  // If PC exceeds 0xFFFF and encounters page_wrap_opcode, it will be silently wrapped.
  enum
  {
    page_wrap_opcode = 0xF2
  };

  // One of the many opcodes that are undefined and stop CPU emulation.
  enum
  {
    bad_opcode = 0xD2
  };

  uint8_t const *code_map[page_count + 1];
  bool _useFlatCodeMap = false;
  alignas(1024) uint8_t flat_code_map[(page_count + 1) * page_size];
  nes_time_t clock_limit;
  nes_time_t clock_count;
  nes_time_t irq_time_;
  nes_time_t end_time_;
  unsigned long error_count_;

  enum
  {
    irq_inhibit = 0x04
  };

  inline void update_clock_limit()
  {
    nes_time_t t = end_time_;
    if (t > irq_time_ && !(r.status & irq_inhibit))
      t = irq_time_;
    clock_limit = t;
  }

  registers_t r;
  bool isCorrectExecution = true;

  // Sticky halt latch: set when the CPU executes a KIL/JAM (or any unimplemented) opcode and never
  // cleared by run() -- on real hardware a jammed 6502 stays frozen (NMI/IRQ are not serviced) until
  // RESET, whereas this emulator's frame loop would otherwise revive the game at the next NMI vector.
  // Consumers (e.g. a search driver) must treat a set latch as a dead/invalid state. Serialized with
  // the CPUR block (cpu_state_t.unused[0]) so it travels with savestates.
  uint8_t haltLatch = 0;

#ifdef _QUICKERNES_DETECT_BAD_ACCESS
  // Per-frame bad-access flag (see cpu_isOfficialOpcode). NOT sticky and NOT serialized: reset to 0
  // at the start of every emulate_frame and set the instant a bad fetch is executed, so it reports
  // "did THIS frame derail into data-as-code" -- exactly the win signal for the glitch search, and
  // recomputed from the (serialized) machine state each advance so it never leaks across branches.
  uint8_t badAccessLatch = 0;
#endif

  // low_mem is a full page size so it can be mapped with code_map
  uint8_t low_mem[page_size > 0x800 ? page_size : 0x800];

  inline uint8_t *get_code(nes_addr_t addr)
  {
    return (uint8_t *)code_map[addr >> page_bits] + addr;
  }

  inline const uint8_t *get_code(nes_addr_t addr) const
  {
    return (const uint8_t *)code_map[addr >> page_bits] + addr;
  }

  // status flags
  static constexpr uint8_t clock_table[256] = {
  //  0   1   2   3   4   5   6   7   8   9   A   B   C   D   E   F
      7,  6,  2,  8,  3,  3,  5,  5,  3,  2,  2,  2,  4,  4,  6,  6, // 0
      3,  5,  2,  8,  4,  4,  6,  6,  2,  4,  2,  7,  4,  4,  7,  7, // 1
      6,  6,  2,  8,  3,  3,  5,  5,  4,  2,  2,  2,  4,  4,  6,  6, // 2
      3,  5,  2,  8,  4,  4,  6,  6,  2,  4,  2,  7,  4,  4,  7,  7, // 3
      6,  6,  2,  8,  3,  3,  5,  5,  3,  2,  2,  2,  3,  4,  6,  6, // 4
      3,  5,  2,  8,  4,  4,  6,  6,  2,  4,  2,  7,  4,  4,  7,  7, // 5
      6,  6,  2,  8,  3,  3,  5,  5,  4,  2,  2,  2,  5,  4,  6,  6, // 6
      3,  5,  2,  8,  4,  4,  6,  6,  2,  4,  2,  7,  4,  4,  7,  7, // 7
      2,  6,  2,  6,  3,  3,  3,  3,  2,  2,  2,  2,  4,  4,  4,  4, // 8
      3,  6,  2,  6,  4,  4,  4,  4,  2,  5,  2,  5,  5,  5,  5,  5, // 9
      2,  6,  2,  6,  3,  3,  3,  3,  2,  2,  2,  2,  4,  4,  4,  4, // A
      3,  5,  2,  5,  4,  4,  4,  4,  2,  4,  2,  4,  4,  4,  4,  4, // B
      2,  6,  2,  8,  3,  3,  5,  5,  2,  2,  2,  2,  4,  4,  6,  6, // C
      3,  5,  2,  8,  4,  4,  6,  6,  2,  4,  2,  7,  4,  4,  7,  7, // D
      2,  6,  2,  8,  3,  3,  5,  5,  2,  2,  2,  2,  4,  4,  6,  6, // E
      3,  5,  2,  8,  4,  4,  6,  6,  2,  4,  2,  7,  4,  4,  7,  7  // F
  };

  // Clear registers, unmap memory, and map code pages to unmapped_page.
  inline void reset(void const *unmapped_page)
  {
    r.status = 0;
    r.sp = 0;
    r.pc = 0;
    r.a = 0;
    r.x = 0;
    r.y = 0;

    error_count_ = 0;
    clock_count = 0;
    clock_limit = 0;
    irq_time_ = LONG_MAX / 2 + 1;
    end_time_ = LONG_MAX / 2 + 1;

    set_code_page(0, low_mem);
    set_code_page(1, low_mem);
    set_code_page(2, low_mem);
    set_code_page(3, low_mem);
    for (int32_t i = 4; i < page_count + 1; i++)
      set_code_page(i, (uint8_t *)unmapped_page);

    isCorrectExecution = true;
    haltLatch          = 0; // only RESET recovers a jammed CPU
  }
};

} // namespace quickerNES