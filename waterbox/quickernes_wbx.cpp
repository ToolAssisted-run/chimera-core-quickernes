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

/* ===================== core-managed tooling (guest ABI v2) =====================
 *
 * miniHawk is core-agnostic: it knows nothing about the NES. So rather than
 * handing raw PPU state to a frontend tool that understands nametables (BizHawk's
 * INESPPUViewable model), the CORE owns its own tooling and publishes it through
 * three generic, system-neutral mechanisms:
 *
 *   surfaces  - named images the core renders itself (here: nametables, pattern
 *               tables, sprites). The frontend just displays whatever is declared.
 *   registers - named CPU registers for the generic debugger.
 *   buses     - named address spaces for peek/poke (here: PRG and PPU).
 *   trace     - instruction trace written into a guest ring buffer and drained
 *               per frame, so tracing costs no sandbox crossings per instruction.
 *
 * Every group is OPTIONAL: the host probes for these exports and enables the
 * matching service only if they are present.
 */
namespace
{
	constexpr int NtWidth = 512, NtHeight = 480;   // 2x2 nametables of 256x240
	constexpr int PtWidth = 256, PtHeight = 128;   // 2 pattern tables of 128x128
	constexpr int OamWidth = 256, OamHeight = 128; // 64 sprites, 8 per row

	uint32_t g_surface[NtWidth * NtHeight];

	inline uint32_t nesColor(int palIndex)
	{
		return g_palette[palIndex & 0x1FF];
	}

	/* Renders one 8x8 tile from the pattern table at `patternBase` into dst. */
	void drawTile(uint32_t *dst, int stride, int tile, int patternBase, const uint8_t *palette4)
	{
		for (int row = 0; row < 8; row++)
		{
			const int addr = patternBase + tile * 16 + row;
			const uint8_t lo = g_emu->peek_ppu(addr);
			const uint8_t hi = g_emu->peek_ppu(addr + 8);
			for (int col = 0; col < 8; col++)
			{
				const int bit = 7 - col;
				const int v = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
				dst[row * stride + col] = nesColor(palette4[v]);
			}
		}
	}

	void renderNametables()
	{
		const uint8_t r2000 = g_emu->get_ppu2000();
		const int bgBase = (r2000 & 0x10) ? 0x1000 : 0x0000;
		const uint8_t *palRam = g_emu->pal_mem();

		for (int nt = 0; nt < 4; nt++)
		{
			const int ntBase = 0x2000 + nt * 0x400;
			const int ox = (nt & 1) * 256, oy = (nt >> 1) * 240;
			for (int ty = 0; ty < 30; ty++)
			{
				for (int tx = 0; tx < 32; tx++)
				{
					const int tile = g_emu->peek_ppu(ntBase + ty * 32 + tx);
					// attribute byte: 4x4 tile blocks, 2 bits per 2x2 quadrant
					const int attr = g_emu->peek_ppu(ntBase + 0x3C0 + (ty / 4) * 8 + (tx / 4));
					const int quad = ((ty & 2) << 1) | (tx & 2);
					const int palSel = (attr >> quad) & 3;
					uint8_t pal4[4];
					pal4[0] = palRam[0];
					for (int i = 1; i < 4; i++) pal4[i] = palRam[palSel * 4 + i];
					drawTile(&g_surface[(oy + ty * 8) * NtWidth + ox + tx * 8], NtWidth, tile, bgBase, pal4);
				}
			}
		}
	}

	void renderPatternTables()
	{
		const uint8_t *palRam = g_emu->pal_mem();
		uint8_t pal4[4] = { palRam[0], palRam[1], palRam[2], palRam[3] };
		for (int half = 0; half < 2; half++)
			for (int ty = 0; ty < 16; ty++)
				for (int tx = 0; tx < 16; tx++)
					drawTile(&g_surface[(ty * 8) * PtWidth + half * 128 + tx * 8], PtWidth,
						ty * 16 + tx, half * 0x1000, pal4);
	}

	void renderSprites()
	{
		const uint8_t r2000 = g_emu->get_ppu2000();
		const int spBase = (r2000 & 0x08) ? 0x1000 : 0x0000;
		const uint8_t *oam = g_emu->spr_mem();
		const uint8_t *palRam = g_emu->pal_mem();
		memset(g_surface, 0, sizeof(uint32_t) * OamWidth * OamHeight);
		for (int s = 0; s < 64; s++)
		{
			const uint8_t tile = oam[s * 4 + 1];
			const uint8_t attr = oam[s * 4 + 2];
			uint8_t pal4[4];
			pal4[0] = palRam[0];
			for (int i = 1; i < 4; i++) pal4[i] = palRam[0x10 + (attr & 3) * 4 + i];
			const int ox = (s % 8) * 8 * 4, oy = (s / 8) * 8 * 2;
			drawTile(&g_surface[oy * OamWidth + ox], OamWidth, tile, spBase, pal4);
		}
	}

	struct SurfaceDef { const char *name; int w, h; void (*render)(); };
	const SurfaceDef kSurfaces[] = {
		{ "Nametables",     NtWidth,  NtHeight,  renderNametables },
		{ "Pattern Tables", PtWidth,  PtHeight,  renderPatternTables },
		{ "Sprites (OAM)",  OamWidth, OamHeight, renderSprites },
	};
	constexpr int kSurfaceCount = (int)(sizeof kSurfaces / sizeof kSurfaces[0]);

	const char *const kRegNames[] = { "A", "X", "Y", "SP", "PC", "P" };
	constexpr int kRegCount = 6;

	const char *const kBusNames[] = { "PRG (CPU)", "PPU" };
	constexpr int kBusCount = 2;

	/* Trace ring buffer: the tracer runs inside the guest and appends NUL-
	 * terminated lines here; the host drains it once per frame. Crossing the
	 * sandbox boundary per instruction would be unusably slow. */
	constexpr int TraceBufBytes = 1 << 20;
	char g_traceBuf[TraceBufBytes];
	int g_traceUsed = 0;
	int g_traceLines = 0;
	int g_traceEnabled = 0;
	int g_traceOverflow = 0;

	void traceCallback(unsigned int *regs)
	{
		if (!g_traceEnabled) return;
		// regs: A, X, Y, SP, PC, P (same order as get_regs)
		// regs[6] is the opcode (see cpuFlat.cpp / cpuPaged.cpp)
		char line[128];
		int n = snprintf(line, sizeof line,
			"PC:%04X  OP:%02X  A:%02X X:%02X Y:%02X SP:%02X P:%02X",
			regs[4] & 0xFFFF, regs[6] & 0xFF, regs[0] & 0xFF, regs[1] & 0xFF,
			regs[2] & 0xFF, regs[3] & 0xFF, regs[5] & 0xFF);
		if (n < 0) return;
		if (g_traceUsed + n + 1 > TraceBufBytes) { g_traceOverflow = 1; return; }
		memcpy(g_traceBuf + g_traceUsed, line, (size_t)n + 1);
		g_traceUsed += n + 1;
		g_traceLines++;
	}
}

extern "C"
{

/* ---- surfaces ---- */
ECL_EXPORT int GetSurfaceCount(void) { return kSurfaceCount; }
ECL_EXPORT const char *GetSurfaceName(int i) { return (i >= 0 && i < kSurfaceCount) ? kSurfaces[i].name : nullptr; }
ECL_EXPORT int GetSurfaceWidth(int i) { return (i >= 0 && i < kSurfaceCount) ? kSurfaces[i].w : 0; }
ECL_EXPORT int GetSurfaceHeight(int i) { return (i >= 0 && i < kSurfaceCount) ? kSurfaces[i].h : 0; }
ECL_EXPORT uint32_t *RenderSurface(int i)
{
	if (i < 0 || i >= kSurfaceCount || !g_emu) return nullptr;
	kSurfaces[i].render();
	return g_surface;
}

/* ---- cpu registers ---- */
ECL_EXPORT int GetRegisterCount(void) { return kRegCount; }
ECL_EXPORT const char *GetRegisterName(int i) { return (i >= 0 && i < kRegCount) ? kRegNames[i] : nullptr; }
ECL_EXPORT int64_t GetRegisterValue(int i)
{
	if (i < 0 || i >= kRegCount || !g_emu) return 0;
	unsigned int regs[kRegCount] = {0};
	g_emu->get_regs(regs);
	return regs[i];
}

/* ---- address buses (peek/poke beyond the memory domains) ---- */
ECL_EXPORT int GetBusCount(void) { return kBusCount; }
ECL_EXPORT const char *GetBusName(int i) { return (i >= 0 && i < kBusCount) ? kBusNames[i] : nullptr; }
ECL_EXPORT int PeekBus(int bus, int addr)
{
	if (!g_emu) return -1;
	if (bus == 0) return g_emu->peek_prg(addr & 0xFFFF);
	if (bus == 1) return g_emu->peek_ppu(addr & 0x3FFF);
	return -1;
}
ECL_EXPORT void PokeBus(int bus, int addr, int value)
{
	if (!g_emu) return;
	if (bus == 0) g_emu->poke_prg(addr & 0xFFFF, (uint8_t)value);
}

/* ---- instruction trace ---- */
ECL_EXPORT void TraceSetEnabled(int on)
{
	g_traceEnabled = on;
	g_emu->set_tracecb(on ? traceCallback : nullptr);
	if (!on) { g_traceUsed = 0; g_traceLines = 0; g_traceOverflow = 0; }
}
ECL_EXPORT int TraceGetLineCount(void) { return g_traceLines; }
ECL_EXPORT char *TraceGetBuffer(void) { return g_traceBuf; }
ECL_EXPORT int TraceGetOverflow(void) { return g_traceOverflow; }
ECL_EXPORT void TraceClear(void) { g_traceUsed = 0; g_traceLines = 0; g_traceOverflow = 0; }

} // extern "C"

int main() { return 0; }
