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
#include <waterbox_slots.h>

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
	int g_joypadReadThisFrame = 0;

	/* Turbo. The host sets this to 0 when nobody is going to look at the frame
	 * (seeking, fast-forward), and the PPU stops writing pixels. It is host
	 * policy rather than machine state, so it lives outside the savestate: a
	 * state taken while seeking must not put the machine back into turbo when
	 * it is loaded to be looked at. */
	ECL_INVISIBLE int g_render = 1;

	/* ---- controller ports ----
	 * What is plugged into each port is a machine-shaping choice, so it arrives
	 * as a user setting (mounted "settings" JSON, read at Init) rather than being
	 * baked into the package. The frontend never learns what any of it means: it
	 * ships the setting through and sends the button mask and axis values the
	 * package declares. */
	enum PortMode
	{
		portNone = 0,
		portGamepad,
		portFourScore,
		portArkanoidNES,
		portArkanoidFamicom,
	};

	PortMode g_port1 = portGamepad;
	PortMode g_port2 = portNone;

	/* ---- live (non-sync) settings ----
	 * Settings that do not shape the machine can change while the core runs, so
	 * they cannot arrive through the mounted "settings" file, which is fixed for
	 * the core's lifetime. The host writes fresh JSON into g_settingsBuf and calls
	 * PutSettings; the buffer is ECL_INVISIBLE because settings are not machine
	 * state and must not end up in savestates - a state from before a change would
	 * otherwise restore the old value and make two identical runs differ. */
	ECL_INVISIBLE char g_settingsBuf[4096];

	void applyLiveSettings()
	{
		long limit = wbx_setting_long("spriteLimit", 8);
		if (limit < 0) limit = 0;
		if (limit > 64) limit = 64;
		g_emu->set_sprite_mode((quickerNES::Emu::sprite_mode_t)limit);
	}
	quickerNES::Core::controllerType_t g_controllerType = quickerNES::Core::controllerType_t::joypad_t;
	int g_axis[2] = { 80, 80 }; // paddle positions, neutral per waterbox.config

	PortMode parsePort(const char *key, PortMode dflt)
	{
		char buf[32];
		if (!wbx_setting_str(key, buf, sizeof buf)) return dflt;
		if (!strcmp(buf, "none"))            return portNone;
		if (!strcmp(buf, "gamepad"))         return portGamepad;
		if (!strcmp(buf, "fourScore"))       return portFourScore;
		if (!strcmp(buf, "arkanoidNES"))     return portArkanoidNES;
		if (!strcmp(buf, "arkanoidFamicom")) return portArkanoidFamicom;
		return dflt;
	}

	/* A standard joypad latches 8 button bits; everything above them reads back as
	 * 1s (open bus on the real console), which is what the 0xFFFFFF00 prefix is.
	 * An unplugged port latches 0. Button order is the NES joypad's own -
	 * A,B,Select,Start,Up,Down,Left,Right - and waterbox.config declares the
	 * buttons in that same order, so a byte of the mask passes straight through. */
	uint32_t packGamepad(uint64_t input, int shift)
	{
		return 0xFFFFFF00u | (uint32_t)((input >> shift) & 0xFF);
	}

	/* The Four Score multiplexes two pads per port and appends a signature the
	 * game uses to detect it: after the 16 button bits, port 1 reports 0b1000 and
	 * port 2 reports 0b0100. */
	uint32_t packFourScore(uint64_t input, int shiftNear, int shiftFar, uint32_t signature)
	{
		return signature
			| (uint32_t)((input >> shiftNear) & 0xFF)
			| ((uint32_t)((input >> shiftFar) & 0xFF) << 8);
	}

	/* The Arkanoid paddle reports its position as a serial bit stream, relative to
	 * a calibration point and bit-reversed. See
	 * https://www.nesdev.org/wiki/Arkanoid_controller; 0xAB is NesHawk's
	 * calibration, kept identical so movies agree across emulators. */
	uint32_t arkanoidLatch(uint8_t position)
	{
		const uint8_t centeringPotValue = 0xAB;
		const uint8_t relative = (uint8_t)(centeringPotValue - position);
		uint32_t latch = 0;
		for (int bit = 0; bit < 8; bit++)
			if (relative & (1u << (7 - bit))) latch |= 1u << bit;
		return latch;
	}

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
			case 1: *data = g_emu->high_mem();      *size = (int)g_emu->get_high_mem_size(); *name = "WRAM";             return true;
			case 2: *data = g_emu->chr_mem();       *size = (int)g_emu->chr_size();        *name = "CHR";                return true;
			case 3: *data = g_emu->nametable_mem(); *size = (int)g_emu->nametable_size();  *name = "CIRAM (nametables)"; return true;
			case 4: *data = g_emu->cart()->prg();   *size = (int)g_emu->cart()->prg_size();*name = "PRG ROM";            return true;
			case 5: *data = g_emu->cart()->chr();   *size = (int)g_emu->cart()->chr_size();*name = "CHR VROM";           return true;
			case 6: *data = g_emu->pal_mem();       *size = (int)g_emu->pal_mem_size();    *name = "PALRAM";             return true;
			case 7: *data = g_emu->spr_mem();       *size = (int)g_emu->spr_mem_size();    *name = "OAM";                return true;
			default: return false;
		}
	}

	// Reads the whole mounted cartridge (caller frees). A chimera project
	// mounts "slots" naming the cartridge's canonical file ({"rom":["x.nes"]},
	// see file_slots.json); without the map, the legacy "rom" mount.
	uint8_t *readRom(uint32_t *outLen)
	{
		char romName[256] = "rom";
		wbx_slot_first("rom", romName, sizeof romName);
		FILE *f = fopen(romName, "rb");
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

	// Ports are user settings; they decide both how the button mask is packed and
	// which peripheral protocol the machine speaks.
	g_port1 = parsePort("port1", portGamepad);
	g_port2 = parsePort("port2", portNone);
	g_controllerType = quickerNES::Core::controllerType_t::none_t;
	if (g_port2 == portGamepad || g_port2 == portFourScore)
		g_controllerType = quickerNES::Core::controllerType_t::joypad_t;
	// port 1 wins: an Arkanoid there sets the protocol for the whole machine
	if (g_port1 == portGamepad || g_port1 == portFourScore)
		g_controllerType = quickerNES::Core::controllerType_t::joypad_t;
	else if (g_port1 == portArkanoidNES)
		g_controllerType = quickerNES::Core::controllerType_t::arkanoidNES_t;
	else if (g_port1 == portArkanoidFamicom)
		g_controllerType = quickerNES::Core::controllerType_t::arkanoidFamicom_t;

	// the non-sync settings were mounted alongside the sync ones, so the core
	// starts at the user's chosen values rather than the defaults
	applyLiveSettings();

	return 1;
}

/* The live-settings group: a host that finds all three can change a non-sync
 * setting without rebooting the core. Missing exports just mean a reboot. */
ECL_EXPORT int GetSettingsCapacity(void) { return (int)sizeof g_settingsBuf; }

ECL_EXPORT char *GetSettingsBuffer(void) { return g_settingsBuf; }

ECL_EXPORT void PutSettings(int length)
{
	if (length < 0 || length > (int)sizeof g_settingsBuf) return;
	wbx_settings_use_buffer(g_settingsBuf, length);
	applyLiveSettings();
	wbx_settings_use_file();
}

/* WHICH DECLARED CONTROLS THIS MACHINE HAS.
 *
 * waterbox.config declares the union of every peripheral these two ports can
 * hold - four players' pads, the Arkanoid fire buttons, the Arkanoid paddles -
 * because a declaration is static and cannot know what a project plugged in.
 * That made every NES project show all thirty-four columns whatever was in its
 * ports: players three and four with no Four Score anywhere, a paddle with no
 * Arkanoid controller, a fire button belonging to neither.
 *
 * The port settings are read HERE, so here is the only place the answer is not
 * a copy of somebody else's reasoning. The frontend asks once, after Init.
 *
 * The wire is untouched: index 8 is P2 A whether or not port two holds
 * anything, so packGamepad and every bit position below stay exactly as they
 * are. What changes is only what a person is shown and what a movie writes. */
namespace {
	/* Which players a port answers for, read off the PACKING below rather than
	 * assumed: a port latching a gamepad carries its near player, a Four Score
	 * carries its near player and its far one (shiftNear/shiftFar in
	 * FrameAdvance), an Arkanoid on the NES protocol occupies port one entirely
	 * and latches no pad at all. Anything the packing does not reach is a
	 * column nobody could ever move. */
	bool playerLive(int player) /* 1..4 */
	{
		switch (player)
		{
			case 1: return g_port1 == portGamepad || g_port1 == portFourScore
				|| g_port1 == portArkanoidFamicom;
			case 2: return g_port2 == portGamepad || g_port2 == portFourScore;
			case 3: return g_port1 == portFourScore;
			case 4: return g_port2 == portFourScore;
			default: return false;
		}
	}
}

ECL_EXPORT int IsButtonActive(int index)
{
	/* bits 0-31 are the four players' pads, in declaration order */
	if (index >= 0 && index < 32) return playerLive(index / 8 + 1) ? 1 : 0;
	/* bit 32 is P2 Fire (Arkanoid NES), bit 33 P3 Fire (Arkanoid Famicom) -
	 * the two protocols the machine can speak, one port at a time */
	if (index == 32) return g_port1 == portArkanoidNES ? 1 : 0;
	if (index == 33) return g_port1 == portArkanoidFamicom ? 1 : 0;
	return 0;
}

ECL_EXPORT int IsAxisActive(int index)
{
	/* the paddles, in the same order and for the same two protocols */
	if (index == 0) return g_port1 == portArkanoidNES ? 1 : 0;
	if (index == 1) return g_port1 == portArkanoidFamicom ? 1 : 0;
	return 0;
}

/* Analog controls can't ride in the button mask; the host pushes each declared
 * axis here just before the frame it belongs to. Index order is
 * waterbox.config's input.axes: 0 = P2 Paddle, 1 = P3 Paddle. */
ECL_EXPORT void SetAxis(int index, int value)
{
	if (index >= 0 && index < 2) g_axis[index] = value;
}

/* Input bit layout must match waterbox.config's input.buttons order:
 * bits  0-7  P1 A,B,Select,Start,Up,Down,Left,Right
 * bits  8-15 P2, 16-23 P3, 24-31 P4 (same order within each byte)
 * bit  32    P2 Fire (Arkanoid NES), bit 33 P3 Fire (Arkanoid Famicom)
 * Which of those the machine actually sees depends on the port settings - the
 * declaration is the union of every supported peripheral, since the frontend
 * builds one controller definition per package. */
ECL_EXPORT void FrameAdvance(uint64_t input)
{
	uint32_t pad1 = 0, pad2 = 0;
	uint8_t arkPosition = 0, arkFire = 0;

	switch (g_port1)
	{
		case portGamepad:
		case portArkanoidFamicom: pad1 = packGamepad(input, 0); break;
		case portFourScore:       pad1 = packFourScore(input, 0, 16, 0xFF080000u); break;
		default: break; // arkanoidNES occupies the port; nothing to latch
	}
	switch (g_port2)
	{
		case portGamepad:   pad2 = packGamepad(input, 8); break;
		case portFourScore: pad2 = packFourScore(input, 8, 24, 0xFF040000u); break;
		default: break;
	}
	if (g_port1 == portArkanoidNES)
	{
		arkPosition = (uint8_t)g_axis[0];
		arkFire = (input >> 32) & 1;
	}
	else if (g_port1 == portArkanoidFamicom)
	{
		arkPosition = (uint8_t)g_axis[1];
		arkFire = (input >> 33) & 1;
	}

	g_emu->setControllerType(g_controllerType);
	const uint32_t latch = (g_controllerType == quickerNES::Core::controllerType_t::arkanoidNES_t
		|| g_controllerType == quickerNES::Core::controllerType_t::arkanoidFamicom_t)
		? arkanoidLatch(arkPosition) : 0;
	/* quickerNES has carried a render-off path for years and keeps it honest:
	 * with no pixel buffer the PPU still runs its sprite-zero-hit detection
	 * through a mini offscreen buffer, and the sprite-overflow flag never
	 * depended on drawing at all. So the 6502 sees the same $2002 either way,
	 * which is the whole requirement. */
	g_emu->set_rendering(g_render);
	g_emu->emulate_frame(pad1, pad2, latch, arkFire);

	// Lag detection: a frame that never polled the joypad is a lag frame. The
	// counter is per-frame - emulate_frame zeroes it - so the test is against
	// zero, not against the previous frame's value.
	g_joypadReadThisFrame = (g_emu->get_joypad_read_count() != 0);

	// Video: map the paletted frame into BGRA. In turbo there is nothing to
	// map - the buffer keeps the last frame that was drawn, which is what the
	// video hardware would be scanning out anyway.
	if (g_render)
	{
		const int pitch = g_emu->frame().pitch;
		const unsigned char *src = g_emu->frame().pixels;
		const short *lut = g_emu->frame().palette;
		uint32_t *dst = g_video;
		for (int y = 0; y < FbHeight; y++, src += pitch)
			for (int x = 0; x < FbWidth; x++) *dst++ = g_palette[lut[src[x]]];
	}

	// Audio: drain this frame's samples.
	g_audioSamples = g_emu->read_samples(g_audio, MaxSamplesPerFrame);
}

/* Turbo (optional guest ABI group): while off the core must produce no picture
 * and must otherwise be exactly the machine it would have been. run-gate.sh's
 * turbo leg is the proof - N unrendered frames plus one rendered one come out
 * byte for byte the same machine, and the same picture, as N+1 drawn ones. */
ECL_EXPORT void SetRenderingEnabled(int on) { g_render = on != 0; }

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

	/* Tooling scratch lives in the INVISIBLE section: it is not part of any
	 * savestate, so opening a viewer or the trace logger costs nothing per state.
	 * Nothing in emulation may read it - it is written, handed to the host and
	 * forgotten. */
	ECL_INVISIBLE uint32_t g_surface[NtWidth * NtHeight];

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
	/* the host shows each register this wide, in hex digits = bits/4 */
	const int kRegBits[] = { 8, 8, 8, 8, 16, 8 };
	constexpr int kRegCount = 6;

	const char *const kBusNames[] = { "PRG (CPU)", "PPU" };
	/* 6502 address space; the PPU's is 14-bit (0x0000-0x3FFF) */
	const int64_t kBusSizes[] = { 0x10000, 0x4000 };
	constexpr int kBusCount = 2;

	/* Trace ring buffer: the tracer runs inside the guest and appends NUL-
	 * terminated lines here; the host drains it once per frame. Crossing the
	 * sandbox boundary per instruction would be unusably slow. */
	constexpr int TraceBufBytes = 1 << 20;
	ECL_INVISIBLE char g_traceBuf[TraceBufBytes];
	int g_traceUsed = 0;
	int g_traceLines = 0;
	int g_traceEnabled = 0;
	int g_traceOverflow = 0;

	void traceCallback(unsigned int *regs)
	{
		if (!g_traceEnabled) return;
		// regs: A, X, Y, SP, PC, P (same order as get_regs)
		// regs[6] is the opcode (see cpuFlat.cpp / cpuPaged.cpp)
		// A tab splits the line into the host's two trace-logger columns:
		// "what executed" on the left, "machine state" on the right.
		char line[128];
		int n = snprintf(line, sizeof line,
			"PC:%04X  OP:%02X\tA:%02X X:%02X Y:%02X SP:%02X P:%02X",
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
ECL_EXPORT int GetRegisterBits(int i) { return (i >= 0 && i < kRegCount) ? kRegBits[i] : 0; }
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
/* the PPU bus is read-only here: peek_ppu has no poke_ppu counterpart */
ECL_EXPORT int GetBusWritable(int i) { return i == 0; }
ECL_EXPORT int64_t GetBusSize(int i) { return (i >= 0 && i < kBusCount) ? kBusSizes[i] : 0; }
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
ECL_EXPORT const char *TraceGetHeader(void) { return "6502: PC, opcode | A, X, Y, SP, P"; }
ECL_EXPORT int TraceGetLineCount(void) { return g_traceLines; }
ECL_EXPORT char *TraceGetBuffer(void) { return g_traceBuf; }
/* lets the host copy the whole frame's lines in one go instead of per line */
ECL_EXPORT int TraceGetUsedBytes(void) { return g_traceUsed; }
ECL_EXPORT int TraceGetOverflow(void) { return g_traceOverflow; }
ECL_EXPORT void TraceClear(void) { g_traceUsed = 0; g_traceLines = 0; g_traceOverflow = 0; }

} // extern "C"

int main() { return 0; }
