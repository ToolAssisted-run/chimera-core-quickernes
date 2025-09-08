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

class Cpu
{
  public:

  void set_tracecb(void (*cb)(unsigned int *data))
  {
    tracecb = cb;
  }

  void (*tracecb)(unsigned int *dest);

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
    code_map[i] = p - (unsigned)i * page_size;
    if (_useFlatCodeMap == true) memcpy(&flat_code_map[i*page_size], p, page_size);
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
  }
};

} // namespace quickerNES