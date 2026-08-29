/* Standalone driver for the waterboxed quickerNES core: runs core.wbx through the
 * miniBox host over a rom and reports per-frame video/audio/RAM digests, so the
 * waterboxed flavor can be compared against the native core on the same inputs.
 *
 * usage: run-wbx <core.wbx> <rom.nes> <frames> [--rerecord] [--turbo]
 *
 * --rerecord round-trips the WHOLE guest machine through the host's
 * save/load state around every frame; the digests must be identical either way.
 *
 * --turbo runs the first half of the frames with the core's drawing switched
 * off, then switches it back on for the second half. Everything except the
 * whole-run video hash must come out identical to a plain run - including the
 * pictures of that second half, which is the interesting part: it proves the
 * machine the undrawn frames left behind is the machine that would have been
 * there anyway, right down to what it draws.
 */
#include "minibox.h"
/* guest entry points are sysv64 even on a win64 host (MB_GUEST_ABI is a no-op
 * on Linux); without this the driver cannot call a guest on Windows at all */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FNV-1a: a digest is all we need to compare two implementations frame by frame */
static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
	const uint8_t *b = (const uint8_t *)p;
	if (!h) h = 1469598103934665603ULL;
	for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
	return h;
}

typedef struct { FILE *f; } freader;
static intptr_t file_read(uintptr_t ud, uint8_t *d, uintptr_t s) { return (intptr_t)fread(d, 1, s, ((freader *)ud)->f); }
typedef struct { const uint8_t *p; size_t n, pos; } memreader;
static intptr_t mem_reader(uintptr_t ud, uint8_t *d, uintptr_t s)
{
	memreader *m = (memreader *)ud;
	size_t take = s < (m->n - m->pos) ? s : (m->n - m->pos);
	memcpy(d, m->p + m->pos, take); m->pos += take; return (intptr_t)take;
}
typedef struct { uint8_t *b; size_t len, cap, pos; } membuf;
static int32_t mem_write(uintptr_t ud, const uint8_t *d, uintptr_t n)
{
	membuf *m = (membuf *)ud;
	if (m->len + n > m->cap) { m->cap = (m->len + n) * 2 + 64; m->b = realloc(m->b, m->cap); }
	memcpy(m->b + m->len, d, n); m->len += n; return 0;
}
static intptr_t mem_read(uintptr_t ud, uint8_t *d, uintptr_t n)
{
	membuf *m = (membuf *)ud;
	uintptr_t avail = m->len - m->pos; if (n > avail) n = avail;
	memcpy(d, m->b + m->pos, n); m->pos += n; return (intptr_t)n;
}

/* A deterministic per-frame button pattern, identical in both drivers, so the
 * gate compares the INPUT path too rather than 300 frames of nothing pressed.
 * Bit order is the NES joypad's own: A,B,Select,Start,Up,Down,Left,Right. */
static uint8_t padForFrame(long frame)
{
	uint64_t x = (uint64_t)frame * 6364136223846793005ULL + 1442695040888963407ULL;
	x ^= x >> 33;
	return (uint8_t)(x & 0xFF);
}

typedef int (MB_GUEST_ABI *intfn)(void);
typedef void (MB_GUEST_ABI *voidfn_i)(int);
typedef void (MB_GUEST_ABI *framefn)(uint64_t);
typedef uintptr_t (MB_GUEST_ABI *ptrfn)(void);
typedef uintptr_t (MB_GUEST_ABI *ptrfn_i)(int);
typedef int (MB_GUEST_ABI *intfn_i)(int);
typedef int64_t (MB_GUEST_ABI *i64fn_i)(int);

static uintptr_t proc(mb_host *h, const char *n)
{
	mb_return r; wbx_get_proc_addr(h, n, &r);
	if (r.error_message[0]) { fprintf(stderr, "proc %s: %s\n", n, r.error_message); exit(2); }
	return r.data;
}

/* An optional export: absence is address 0, not an error. */
static uintptr_t proc_opt(mb_host *h, const char *n)
{
	mb_return r; wbx_get_proc_addr(h, n, &r);
	return r.error_message[0] ? 0 : r.data;
}

static uint8_t *slurp(const char *p, long *n)
{
	FILE *f = fopen(p, "rb"); if (!f) return 0;
	fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
	uint8_t *b = malloc(*n ? *n : 1);
	if (fread(b, 1, *n, f) != (size_t)*n) { free(b); fclose(f); return 0; }
	fclose(f); return b;
}

int main(int argc, char **argv)
{
	const char *wbxPath = 0, *romPath = 0; long frames = 60; int rerecord = 0, turbo = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--rerecord")) rerecord = 1;
		else if (!strcmp(argv[i], "--turbo")) turbo = 1;
		else if (!wbxPath) wbxPath = argv[i];
		else if (!romPath) romPath = argv[i];
		else frames = strtol(argv[i], 0, 0);
	}
	if (!wbxPath || !romPath) { fprintf(stderr, "usage: run-wbx <core.wbx> <rom.nes> <frames> [--rerecord] [--turbo]\n"); return 2; }

	long romLen = 0;
	uint8_t *rom = slurp(romPath, &romLen);
	if (!rom) { fprintf(stderr, "cannot read %s\n", romPath); return 1; }
	FILE *wf = fopen(wbxPath, "rb");
	if (!wf) { fprintf(stderr, "cannot open %s\n", wbxPath); return 1; }

	/* Sized to the core's needs (see waterbox.config): a savestate costs work
	 * proportional to the declared layout, so an oversized one is pure per-frame cost. */
	mb_memory_layout_template layout = { 16u << 20, 16u << 20, 4u << 20, 16u << 20, 16u << 20 };
	freader fr = { wf };
	mb_return r;
	wbx_create_host(&layout, "core.wbx", file_read, (uintptr_t)&fr, &r);
	fclose(wf);
	if (r.error_message[0]) { fprintf(stderr, "create: %s\n", r.error_message); return 1; }
	mb_host *h = (mb_host *)r.data;

	memreader mr = { rom, (size_t)romLen, 0 };
	wbx_mount_file(h, "rom", mem_reader, (uintptr_t)&mr, false, &r);
	if (r.error_message[0]) { fprintf(stderr, "mount: %s\n", r.error_message); return 1; }

	wbx_activate_host(h, &r);
	intfn Init = (intfn)proc(h, "Init");
	if (Init() != 1) { fprintf(stderr, "Init failed (bad rom?)\n"); return 1; }

	framefn FrameAdvance = (framefn)proc(h, "FrameAdvance");
	ptrfn GetVideoBgra = (ptrfn)proc(h, "GetVideoBgra");
	ptrfn GetAudio = (ptrfn)proc(h, "GetAudio");
	intfn GetAudioSampleCount = (intfn)proc(h, "GetAudioSampleCount");
	intfn InputWasRead = (intfn)proc(h, "InputWasRead");
	intfn GetMemoryDomainCount = (intfn)proc(h, "GetMemoryDomainCount");
	ptrfn_i GetMemoryDomainName = (ptrfn_i)proc(h, "GetMemoryDomainName");
	ptrfn_i GetMemoryDomainPtr = (ptrfn_i)proc(h, "GetMemoryDomainPtr");
	i64fn_i GetMemoryDomainSize = (i64fn_i)proc(h, "GetMemoryDomainSize");
	intfn_i GetMemoryDomainWritable = (intfn_i)proc(h, "GetMemoryDomainWritable");

	/* the guest self-describes its domains only after Init - they depend on the cart */
	int nd = GetMemoryDomainCount();
	printf("domains=%d\n", nd);
	for (int i = 0; i < nd; i++) {
		printf("  [%d] %-20s size=%-8lld writable=%d\n", i,
			(const char *)GetMemoryDomainName(i), (long long)GetMemoryDomainSize(i),
			GetMemoryDomainWritable(i));
	}

	wbx_deactivate_host(h, &r);
	wbx_seal(h, &r);
	if (r.error_message[0]) { fprintf(stderr, "seal: %s\n", r.error_message); return 1; }
	wbx_activate_host(h, &r);

	voidfn_i SetRenderingEnabled = (voidfn_i)proc_opt(h, "SetRenderingEnabled");
	if (turbo && !SetRenderingEnabled) { fprintf(stderr, "core exports no SetRenderingEnabled\n"); return 3; }

	uint64_t vh = 0, ah = 0;
	/* the second half of the run, hashed separately: see the turbo hook */
	const long tail = frames / 2;
	uint64_t th = 0;
	long lag = 0;
	membuf st = {0};
	for (long f = 0; f < frames; f++) {
		/* turbo: draw nothing for the first half of the run, then draw the
		 * second half normally - those are the pictures the turbo leg
		 * compares */
		if (turbo) SetRenderingEnabled(f >= tail);
		if (rerecord) {
			/* No deactivate/activate bracket: the host activates itself for the
			 * duration and restores what it found. Bracketing it unmaps and remaps
			 * the whole guest arena four times per frame, which for a rerecord
			 * replay costs far more than the state itself. */
			st.len = 0;
			wbx_save_state(h, mem_write, (uintptr_t)&st, &r);
			st.pos = 0;
			wbx_load_state(h, mem_read, (uintptr_t)&st, &r);
			if (r.error_message[0]) { fprintf(stderr, "rerecord: %s\n", r.error_message); return 1; }
		}
		/* the guest does its own packing from this mask - see waterbox.cpp */
		FrameAdvance((uint64_t)padForFrame(f));
		vh = fnv(vh, (const void *)GetVideoBgra(), 256 * 240 * 4);
		if (f >= tail) th = fnv(th, (const void *)GetVideoBgra(), 256 * 240 * 4);
		ah = fnv(ah, (const void *)GetAudio(), (size_t)GetAudioSampleCount() * 2);
		/* lag is emulation-visible state too: a build with joypad-read detection
		 * compiled out reports every frame as lag, and RAM alone never notices */
		if (!InputWasRead()) lag++;
	}

	printf("frames=%ld\n", frames);
	printf("videoHash=%016llx\n", (unsigned long long)vh);
	printf("tailVideoHash=%016llx\n", (unsigned long long)th);
	printf("audioHash=%016llx\n", (unsigned long long)ah);
	printf("lagFrames=%ld\n", lag);
	for (int i = 0; i < nd; i++) {
		uint64_t dh = fnv(0, (const void *)GetMemoryDomainPtr(i), (size_t)GetMemoryDomainSize(i));
		printf("domain[%s]=%016llx\n", (const char *)GetMemoryDomainName(i), (unsigned long long)dh);
	}

	wbx_deactivate_host(h, &r); wbx_destroy_host(h, &r);
	free(rom); free(st.b);
	return 0;
}
