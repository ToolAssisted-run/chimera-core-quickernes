/* core.wbx - quickerNES as a miniHawk waterbox core.
 *
 * The emulator itself is the unmodified quickerNES core; this file is only the
 * thin waterbox ABI layer over it (the same role minihawk/native/bizinterface.cpp
 * plays for the native build).
 *
 * The whole machine lives in guest memory, so the miniBox host savestates it
 * automatically: there is no serialize/deserialize export here, and quickerNES's
 * own serializeState/jaffarCommon path is not used at all. That is the point of
 * the waterbox flavor - reproducibility by construction.
 *
 * The rom arrives as a mounted file "rom" (read at Init), per the side-effect
 * rule: all data crosses the host interface, never a host path.
 */
#include <emulibc.h>
#include <waterbox_settings.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <emu.hpp>

namespace
{
	constexpr int FbWidth = 256;
	constexpr int FbHeight = 240;
	// quickerNES renders into a buffer with 8 pixels of slack per row (see
	// bizinterface.cpp's DEFAULT_WIDTH + 8).
	constexpr int VideoBufferSize = 65536;
	// NES audio: ~44.1kHz mono. A frame is ~735 samples; keep generous headroom.
	constexpr int SampleRate = 44100;
	constexpr int MaxSamplesPerFrame = 4096;

	quickerNES::Emu *g_emu = nullptr;
	uint32_t g_video[FbWidth * FbHeight];
	int16_t g_audio[MaxSamplesPerFrame];
	int g_audioSamples = 0;
	uint32_t g_palette[512];
	int g_lastJoypadReadCount = 0;
	int g_joypadReadThisFrame = 0;

	/* The cartridge-dependent memory map, mirroring the native adapter's
	 * qn_get_memory_area (minihawk/native/bizinterface.cpp) so both flavors
	 * expose exactly the same domains under the same names. */
	bool memoryArea(int which, const void **data, int *size, int *writable, const char **name)
	{
		if (!g_emu) return false;
		*writable = 1;
		switch (which)
		{
			case 0: *data = g_emu->get_low_mem();   *size = (int)g_emu->low_mem_size;      *name = "RAM";                return true;
			case 1: *data = g_emu->high_mem();      *size = (int)g_emu->high_mem_size;     *name = "WRAM";               return true;
			case 2: *data = g_emu->chr_mem();       *size = (int)g_emu->chr_size();        *name = "CHR";                return true;
			case 3: *data = g_emu->nametable_mem(); *size = (int)g_emu->nametable_size();  *name = "CIRAM (nametables)"; return true;
			case 4: *data = g_emu->cart()->prg();   *size = (int)g_emu->cart()->prg_size();*name = "PRG ROM";            return true;
			case 5: *data = g_emu->cart()->chr();   *size = (int)g_emu->cart()->chr_size();*name = "CHR VROM";           return true;
			case 6: *data = g_emu->pal_mem();       *size = (int)g_emu->pal_mem_size();    *name = "PALRAM";             return true;
			case 7: *data = g_emu->spr_mem();       *size = (int)g_emu->spr_mem_size();    *name = "OAM";                return true;
			default: return false;
		}
	}

	// Reads the whole mounted "rom" file (caller frees).
	uint8_t *readRom(uint32_t *outLen)
	{
		FILE *f = fopen("rom", "rb");
		if (!f) return nullptr;
		fseek(f, 0, SEEK_END);
		long n = ftell(f);
		fseek(f, 0, SEEK_SET);
		auto *buf = (uint8_t *)malloc(n > 0 ? (size_t)n : 1);
		if (!buf) { fclose(f); return nullptr; }
		if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return nullptr; }
		fclose(f);
		*outLen = (uint32_t)n;
		return buf;
	}
}

extern "C"
{

ECL_EXPORT int Init(void)
{
	uint32_t romLen = 0;
	uint8_t *rom = readRom(&romLen);
	if (!rom) return 0;

	// Zero-initialised, exactly as the native adapter does, so no stale bytes can
	// leak into the sealed baseline.
	void *mem = calloc(1, sizeof(quickerNES::Emu));
	if (!mem) { free(rom); return 0; }
	g_emu = new (mem) quickerNES::Emu();

	auto *videoBuffer = (uint8_t *)malloc(VideoBufferSize);
	if (!videoBuffer) { free(rom); return 0; }
	g_emu->set_pixels(videoBuffer, FbWidth + 8);

	if (g_emu->load_ines(rom, romLen)) { free(rom); return 0; }  // returns an error string
	free(rom);

	if (g_emu->set_sample_rate(SampleRate)) return 0;
	g_emu->set_equalizer(quickerNES::Emu::nes_eq);

	// Resolve the NES palette once into BGRA (0xFFrrggbb) for presentation.
	const quickerNES::Emu::rgb_t *colors = quickerNES::Emu::nes_colors;
	for (int i = 0; i < 512; i++)
	{
		g_palette[i] = 0xFF000000u | ((uint32_t)colors[i].red << 16)
			| ((uint32_t)colors[i].green << 8) | (uint32_t)colors[i].blue;
	}

	g_lastJoypadReadCount = g_emu->get_joypad_read_count();
	return 1;
}

/* Input bit layout must match waterbox.config's input.buttons order:
 * A, B, Select, Start, Up, Down, Left, Right - which is also the NES joypad's
 * own bit order, so the low byte passes straight through. Bits 8..15 are P2. */
ECL_EXPORT void FrameAdvance(uint32_t input)
{
	const uint32_t pad1 = input & 0xFF;
	const uint32_t pad2 = (input >> 8) & 0xFF;

	g_emu->emulate_frame(pad1, pad2, 0, 0);

	// Lag detection: a frame that never polled the joypad is a lag frame.
	const int reads = g_emu->get_joypad_read_count();
	g_joypadReadThisFrame = (reads != g_lastJoypadReadCount);
	g_lastJoypadReadCount = reads;

	// Video: map the paletted frame into BGRA.
	const int pitch = g_emu->frame().pitch;
	const unsigned char *src = g_emu->frame().pixels;
	const short *lut = g_emu->frame().palette;
	uint32_t *dst = g_video;
	for (int y = 0; y < FbHeight; y++, src += pitch)
		for (int x = 0; x < FbWidth; x++) *dst++ = g_palette[lut[src[x]]];

	// Audio: drain this frame's samples.
	g_audioSamples = g_emu->read_samples(g_audio, MaxSamplesPerFrame);
}

ECL_EXPORT uint32_t *GetVideoBgra(void) { return g_video; }
ECL_EXPORT int16_t *GetAudio(void) { return g_audio; }
ECL_EXPORT int GetAudioSampleCount(void) { return g_audioSamples; }
ECL_EXPORT int InputWasRead(void) { return g_joypadReadThisFrame; }

/* --- self-described memory domains (guest ABI v1) ---
 * Queried by the host AFTER Init, because which domains exist and how big they
 * are depends on the loaded cartridge (CHR/PRG sizes, presence of WRAM). This is
 * exactly why the ABI queries them at runtime instead of taking them from a
 * static config. Backed by quickerNES's own indexed memory-area enumeration. */
ECL_EXPORT int GetMemoryDomainCount(void)
{
	int n = 0;
	const void *data; int size, writable; const char *name;
	while (n < 32 && memoryArea(n, &data, &size, &writable, &name)) n++;
	return n;
}

ECL_EXPORT const char *GetMemoryDomainName(int i)
{
	const void *data; int size, writable; const char *name = nullptr;
	return memoryArea(i, &data, &size, &writable, &name) ? name : nullptr;
}

ECL_EXPORT uint8_t *GetMemoryDomainPtr(int i)
{
	const void *data = nullptr; int size, writable; const char *name;
	return memoryArea(i, &data, &size, &writable, &name) ? (uint8_t *)data : nullptr;
}

ECL_EXPORT int64_t GetMemoryDomainSize(int i)
{
	const void *data; int size = 0, writable; const char *name;
	return memoryArea(i, &data, &size, &writable, &name) ? size : 0;
}

ECL_EXPORT int GetMemoryDomainWritable(int i)
{
	const void *data; int size, writable = 0; const char *name;
	return memoryArea(i, &data, &size, &writable, &name) ? writable : 0;
}

} // extern "C"

int main() { return 0; }
