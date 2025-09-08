#pragma once

// NES cartridge data (PRG, CHR, mapper)

/* Copyright (C) 2004-2006 Shay Green. This module is free software; you
can redistribute it and/or modify it under the terms of the GNU Lesser
General Public License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version. This
module is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
more details. You should have received a copy of the GNU Lesser General
Public License along with this module; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA */

// Emu 0.7.0. http://www.slack.net/~ant/

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

namespace quickerNES
{

class Cart
{
  public:
  Cart() = default;

  ~Cart()
  {
    free(prg_);
  }

  struct ines_header_t
  {
    uint8_t signature[4];
    uint8_t prg_count; // number of 16K PRG banks
    uint8_t chr_count; // number of 8K CHR banks
    uint8_t flags;     // MMMM FTBV Mapper low, Four-screen, Trainer, Battery, V mirror
    uint8_t flags2;    // MMMM NNPV Mapper high 4 bits, NES 2.0 (below bytes are NES 2.0 only), Playchoice-10, Vs. System
    uint8_t ex_mapper; // SSSS MMMM Submapper, mapper extended high 4 bits
    uint8_t ex_rom;    // CCCC PPPP CHR bank count high 4 bits, PRG bank count high 4 bits
    uint8_t prg_ram;   // NNNN RRRR PRG NVRAM size, PRG RAM size
    uint8_t chr_ram;   // NNNN RRRR CHR NVRAM size, CHR RAM size
    uint8_t region;    // XXXX XXRR Region timing required
    uint8_t ex_flags;  // Extended console flags (meaning varies depending on console type in flags2)
    uint8_t misc_roms; // Number of miscellaneous ROMs present
    uint8_t ex_device; // XXDD DDDD Default expansion device
  };
  static_assert(sizeof(ines_header_t) == 16);

  // Load iNES file
  const char *load_ines(const uint8_t *buffer, const uint32_t length)
  {
    if (length < sizeof(ines_header_t))
      return "Not an iNES file";

    ines_header_t h;

    size_t bufferPos = 0;
    {
      size_t copySize = sizeof(ines_header_t);
      memcpy(&h, &buffer[bufferPos], copySize);
      bufferPos += copySize;
    }

    if (memcmp(h.signature, "NES\x1A", 4) != 0)
      return "Not an iNES file";

    // Only plain NES is supported
    if (h.flags2 & 0x03)
      return "Unsupported console type";

    int mapper_code = (h.flags2 & 0xF0) | ((h.flags >> 4) & 0x0F);
    int submapper_code = 0;
    if ((h.flags2 & 0x0C) == 0x08)
    {
      mapper_code |= (h.ex_mapper << 8) & 0xF00;
      submapper_code = (h.ex_mapper >> 4) & 0xF;
    }

    #ifdef _QUICKERNES_PRINT_CART_INFO
    printf("[QuickerNES] Cart Mapper Info: Mapper: %d - Submapper: %d\n", mapper_code, submapper_code);
    #endif

    // check if NES 2.0 is present
    if ((h.flags2 & 0x0C) == 0x08)
    {
      if (h.ex_mapper != 0)
      {
        if ((h.ex_mapper & 0x0F) != 0)
          return "Unsupported mapper";

        bool has_compatible_submapper = false;

        // For some submappers, functionality doesn't actually change in an incompatible way
        // For these submappers, we can allow them through

        // https://www.nesdev.org/wiki/INES_Mapper_019
        // Mapper 19 submappers mostly just specify expansion sound volume
        // Submapper 1 and 2 indicate no expansion sound
        // However, having such doesn't add compatibility issues
        if (mapper_code == 19)
          has_compatible_submapper = true;

        if (!has_compatible_submapper)
          return "Unsupported mapper";
      }

       // iNES normally dictates PRG RAM is hardcoded to be 8K
       // NES 2.0 allows for specifying the size, but >8K is unsupported here
       long prg_ram_size = 0;
       if (h.prg_ram & 0x0F) prg_ram_size += 64L << (h.prg_ram & 0x0F);
       if (h.prg_ram & 0xF0) prg_ram_size += 64L << (h.prg_ram >> 4);
       if (prg_ram_size > 0x2000)
         return "Unsupported mapper";

       // Ensure the iNES battery flag is set if NES 2.0 indicates there should be a battery
       if (h.prg_ram & 0xF0)
         h.flags |= 0x02;

       // iNES normally dictates CHR RAM is hardcoded to be 0 or 8K depending on if CHR ROM is present
       // NES 2.0 allows for specifying CHR RAM size, regardless of CHR ROM
       // Anything outside of iNES behavior is unsupported however
       long chr_ram_size = 0;
       if (h.chr_ram & 0x0F) chr_ram_size += 64L << (h.chr_ram & 0x0F);
       if (h.chr_ram & 0xF0) chr_ram_size += 64L << (h.chr_ram >> 4);
       if (chr_ram_size > 0x2000)
         return "Unsupported mapper";
       if (chr_ram_size != 0 && (h.chr_count != 0 || (h.ex_rom & 0xF0)))
         return "Unsupported mapper";

       // iNES can't reliably indicate the game's region, but NES 2.0 can
       // Only NTSC timing is emulated, everything else is unsupported
       if (h.region & 0x01)
         return "Unsupported region";

       if (h.misc_roms > 0)
         return "Unsupported mapper";
    }

    set_mapper(h.flags, h.flags2);

    // skip trainer
    if (h.flags & 0x04) bufferPos += 512;

    // Allocating memory for prg and chr

    unsigned prg_count, chr_count;

    // NES 2.0 allows for even larger ROMs than iNES allows
    if ((h.flags2 & 0x0C) == 0x08)
    {
      // Don't bother supporting exponent-multiplier notation (mapping system can't really handle this)
      if ((h.ex_rom & 0x0F) == 0x0F || (h.ex_rom & 0xF0) == 0xF0)
        return "Unsupported ROM size";

      prg_count = ((h.ex_rom & 0x0F) << 8) | h.prg_count;
      chr_count = ((h.ex_rom & 0xF0) << 4) | h.chr_count;
    }
    else
    {
      prg_count = h.prg_count ? h.prg_count : 256;
      chr_count = h.chr_count;
    }

    prg_size_ = prg_count * 16 * 1024L;
    chr_size_ = chr_count * 8 * 1024L;

    size_t rom_size = prg_size_ + chr_size_;
    if (prg_size_ == 0 || rom_size + bufferPos > length)
      return "Malformed iNES file";

    auto p = malloc(rom_size);
    if (!p)
      return "Out of memory";

    prg_ = (uint8_t *)p;
    chr_ = &prg_[prg_size_];

    {
      size_t copySize = prg_size();
      memcpy(prg(), &buffer[bufferPos], copySize);
      bufferPos += copySize;
    }

    {
      size_t copySize = chr_size();
      memcpy(chr(), &buffer[bufferPos], copySize);
      bufferPos += copySize;
    }

    return nullptr;
  }

  inline bool has_battery_ram() const { return mapper & 0x02; }

  // Set mapper and information bytes. LSB and MSB are the standard iNES header
  // bytes at offsets 6 and 7.
  inline void set_mapper(int mapper_lsb, int mapper_msb)
  {
    mapper = mapper_msb * 0x100 + mapper_lsb;
  }

  inline int mapper_code() const { return ((mapper >> 8) & 0xf0) | ((mapper >> 4) & 0x0f); }

  // Size of PRG data
  long prg_size() const { return prg_size_; }

  // Size of CHR data
  long chr_size() const { return chr_size_; }

  unsigned mapper_data() const { return mapper; }

  // Initial mirroring setup
  int mirroring() const { return mapper & 0x09; }

  // Pointer to beginning of PRG data
  inline uint8_t *prg() { return prg_; }
  inline uint8_t const *prg() const { return prg_; }

  // Pointer to beginning of CHR data
  inline uint8_t *chr() { return chr_; }
  inline uint8_t const *chr() const { return chr_; }

  // End of public interface
  private:
  uint8_t *prg_;
  uint8_t *chr_;
  long prg_size_;
  long chr_size_;
  unsigned mapper;
};

} // namespace quickerNES
