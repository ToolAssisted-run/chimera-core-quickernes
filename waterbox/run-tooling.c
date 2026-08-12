/* Exercises the OPTIONAL half of the waterbox guest ABI - core-managed tooling -
 * the way miniHawk's adapter does: probe for each group's exports, and use only
 * the groups that are there.
 *
 * Every export here is optional by design, so this driver reports what it finds
 * rather than failing on absences: that is exactly the contract the frontend
 * relies on to decide which tool windows a core can back.
 *
 * usage: run-tooling <core.wbx> <rom.nes> [frames]
 */
#include "minibox.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

typedef int (*intfn)(void);
typedef void (*framefn)(uint64_t);
typedef uintptr_t (*ptrfn)(void);
typedef uintptr_t (*ptrfn_i)(int);
typedef int (*intfn_i)(int);
typedef int64_t (*i64fn_i)(int);
typedef int (*peekfn)(int, int);
typedef void (*voidfn_i)(int);

/* the frontend's rule: a missing symbol is address 0, not an error */
static uintptr_t tryproc(mb_host *h, const char *n)
{
	mb_return r; wbx_get_proc_addr(h, n, &r);
	if (r.error_message[0]) { fprintf(stderr, "proc %s: %s\n", n, r.error_message); exit(2); }
	return r.data;
}

static uintptr_t proc(mb_host *h, const char *n)
{
	uintptr_t p = tryproc(h, n);
	if (!p) { fprintf(stderr, "missing required export %s\n", n); exit(2); }
	return p;
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
	if (argc < 3) { fprintf(stderr, "usage: run-tooling <core.wbx> <rom.nes> [frames]\n"); return 2; }
	long frames = argc > 3 ? strtol(argv[3], 0, 0) : 120;

	long romLen = 0;
	uint8_t *rom = slurp(argv[2], &romLen);
	if (!rom) { fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
	FILE *wf = fopen(argv[1], "rb");
	if (!wf) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }

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
	wbx_deactivate_host(h, &r); wbx_seal(h, &r); wbx_activate_host(h, &r);
	if (r.error_message[0]) { fprintf(stderr, "seal: %s\n", r.error_message); return 1; }

	/* get somewhere interesting - a title screen has tiles, sprites and palettes */
	for (long f = 0; f < frames; f++) FrameAdvance(0);
	printf("after %ld frames\n\n", frames);

	/* ---- surfaces ---- */
	intfn SurfCount = (intfn)tryproc(h, "GetSurfaceCount");
	ptrfn_i SurfName = (ptrfn_i)tryproc(h, "GetSurfaceName");
	intfn_i SurfW = (intfn_i)tryproc(h, "GetSurfaceWidth");
	intfn_i SurfH = (intfn_i)tryproc(h, "GetSurfaceHeight");
	ptrfn_i RenderSurface = (ptrfn_i)tryproc(h, "RenderSurface");
	if (SurfCount && SurfName && SurfW && SurfH && RenderSurface) {
		int n = SurfCount();
		printf("surfaces=%d\n", n);
		for (int i = 0; i < n; i++) {
			int w = SurfW(i), hgt = SurfH(i);
			const uint32_t *px = (const uint32_t *)RenderSurface(i);
			if (!px) { printf("  [%d] %-16s RENDER FAILED\n", i, (const char *)SurfName(i)); continue; }
			long lit = 0;
			for (long p = 0; p < (long)w * hgt; p++) if ((px[p] & 0xFFFFFF) != 0) lit++;
			printf("  [%d] %-16s %4dx%-4d hash=%016llx nonblack=%ld%%\n", i,
				(const char *)SurfName(i), w, hgt,
				(unsigned long long)fnv(0, px, (size_t)w * hgt * 4), lit * 100 / ((long)w * hgt));
		}
	} else {
		printf("surfaces: not supported by this core\n");
	}

	/* ---- registers ---- */
	intfn RegCount = (intfn)tryproc(h, "GetRegisterCount");
	ptrfn_i RegName = (ptrfn_i)tryproc(h, "GetRegisterName");
	i64fn_i RegValue = (i64fn_i)tryproc(h, "GetRegisterValue");
	intfn_i RegBits = (intfn_i)tryproc(h, "GetRegisterBits");
	printf("\n");
	if (RegCount && RegName && RegValue) {
		printf("registers=%d (bit widths %s, writable %s, cycle count %s)\n", RegCount(),
			RegBits ? "reported" : "defaulted to 32",
			tryproc(h, "SetRegisterValue") ? "yes" : "no",
			tryproc(h, "GetExecutedCycles") ? "yes" : "no");
		printf("  ");
		for (int i = 0; i < RegCount(); i++) {
			printf("%s=%llX/%db ", (const char *)RegName(i),
				(unsigned long long)RegValue(i), RegBits ? RegBits(i) : 32);
		}
		printf("\n");
	} else {
		printf("registers: not supported by this core\n");
	}

	/* ---- buses ---- */
	intfn BusCount = (intfn)tryproc(h, "GetBusCount");
	ptrfn_i BusName = (ptrfn_i)tryproc(h, "GetBusName");
	peekfn PeekBus = (peekfn)tryproc(h, "PeekBus");
	i64fn_i BusSize = (i64fn_i)tryproc(h, "GetBusSize");
	printf("\n");
	if (BusCount && BusName && PeekBus) {
		int n = BusCount();
		printf("buses=%d (poke %s)\n", n, tryproc(h, "PokeBus") ? "yes" : "no");
		for (int i = 0; i < n; i++) {
			long long size = BusSize ? BusSize(i) : 0x10000;
			/* a digest of the whole space: catches a bus that reads back all zeros */
			uint64_t bh = 1469598103934665603ULL; long nz = 0;
			for (long long a = 0; a < size; a++) {
				uint8_t b = (uint8_t)PeekBus(i, (int)a);
				if (b) nz++;
				bh ^= b; bh *= 1099511628211ULL;
			}
			printf("  [%d] %-12s size=%-8lld hash=%016llx nonzero=%lld%%\n", i,
				(const char *)BusName(i), size, (unsigned long long)bh, nz * 100 / size);
		}
	} else {
		printf("buses: not supported by this core\n");
	}

	/* ---- trace ---- */
	voidfn_i TraceSetEnabled = (voidfn_i)tryproc(h, "TraceSetEnabled");
	intfn TraceLines = (intfn)tryproc(h, "TraceGetLineCount");
	ptrfn TraceBuf = (ptrfn)tryproc(h, "TraceGetBuffer");
	intfn TraceUsed = (intfn)tryproc(h, "TraceGetUsedBytes");
	intfn TraceOverflow = (intfn)tryproc(h, "TraceGetOverflow");
	ptrfn TraceHeader = (ptrfn)tryproc(h, "TraceGetHeader");
	printf("\n");
	if (TraceSetEnabled && TraceLines && TraceBuf) {
		printf("trace: header=\"%s\"\n", TraceHeader ? (const char *)TraceHeader() : "(none)");
		TraceSetEnabled(1);
		FrameAdvance(0);
		int lines = TraceLines();
		printf("  one frame produced %d lines, %d bytes, overflow=%d\n",
			lines, TraceUsed ? TraceUsed() : -1, TraceOverflow ? TraceOverflow() : -1);
		/* the host splits each line on a tab into the logger's two columns */
		const char *p = (const char *)TraceBuf();
		for (int i = 0; i < 3 && i < lines; i++) {
			const char *tab = strchr(p, '\t');
			if (tab) printf("  | %.*s | %s\n", (int)(tab - p), p, tab + 1);
			else printf("  | %s\n", p);
			p += strlen(p) + 1;
		}
		TraceSetEnabled(0);
		printf("  after disabling: %d lines\n", TraceLines());
	} else {
		printf("trace: not supported by this core\n");
	}

	wbx_deactivate_host(h, &r); wbx_destroy_host(h, &r);
	free(rom);
	return 0;
}
