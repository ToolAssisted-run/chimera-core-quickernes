/* Native-core reference for the waterbox equivalence gate.
 *
 * Drives the ORIGINAL quickerNES shared library (libquicknes, the qn_* C API)
 * over the same rom and frame count as waterbox/run-wbx.c, computing the same
 * digests the same way. If the two disagree, waterboxing changed emulation -
 * which is the one thing that must never happen.
 *
 * usage: run-native <libquicknes.so> <rom.nes> <frames>
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
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

typedef struct { uint8_t red, green, blue; } rgb_t;

int main(int argc, char **argv)
{
	if (argc < 4) { fprintf(stderr, "usage: run-native <libquicknes.so> <rom.nes> <frames>\n"); return 2; }
	long frames = strtol(argv[3], 0, 0);

	void *lib = dlopen(argv[1], RTLD_NOW);
	if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }

	void *(*qn_new)(void) = dlsym(lib, "qn_new");
	const char *(*qn_loadines)(void *, const uint8_t *, int) = dlsym(lib, "qn_loadines");
	const char *(*qn_set_sample_rate)(void *, int) = dlsym(lib, "qn_set_sample_rate");
	const char *(*qn_emulate_frame)(void *, uint32_t, uint32_t, uint8_t, uint8_t, int) = dlsym(lib, "qn_emulate_frame");
	void (*qn_blit)(void *, int32_t *, const int32_t *, int, int, int, int) = dlsym(lib, "qn_blit");
	int (*qn_read_audio)(void *, short *, int) = dlsym(lib, "qn_read_audio");
	const rgb_t *(*qn_get_default_colors)(void) = dlsym(lib, "qn_get_default_colors");
	int (*qn_get_memory_area)(void *, int, const void **, int *, int *, const char **) = dlsym(lib, "qn_get_memory_area");
	if (!qn_new || !qn_loadines || !qn_emulate_frame || !qn_blit || !qn_read_audio
		|| !qn_get_default_colors || !qn_get_memory_area) { fprintf(stderr, "missing qn_* symbols\n"); return 1; }

	FILE *f = fopen(argv[2], "rb");
	if (!f) { perror(argv[2]); return 1; }
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	uint8_t *rom = malloc(n);
	if (fread(rom, 1, n, f) != (size_t)n) { fprintf(stderr, "short read\n"); return 1; }
	fclose(f);

	void *e = qn_new();
	const char *err = qn_loadines(e, rom, (int)n);
	if (err) { fprintf(stderr, "load_ines: %s\n", err); return 1; }
	if (qn_set_sample_rate) qn_set_sample_rate(e, 44100);

	/* the same palette the guest builds, so the video digests are comparable */
	const rgb_t *colors = qn_get_default_colors();
	int32_t palette[512];
	for (int i = 0; i < 512; i++)
		palette[i] = (int32_t)(0xFF000000u | ((uint32_t)colors[i].red << 16)
			| ((uint32_t)colors[i].green << 8) | (uint32_t)colors[i].blue);

	static int32_t video[256 * 240];
	static short audio[4096];
	uint64_t vh = 0, ah = 0;
	for (long i = 0; i < frames; i++) {
		qn_emulate_frame(e, 0, 0, 0, 0, 0);
		qn_blit(e, video, palette, 0, 0, 0, 0);
		int samples = qn_read_audio(e, audio, 4096);
		vh = fnv(vh, video, sizeof video);
		ah = fnv(ah, audio, (size_t)samples * 2);
	}

	printf("frames=%ld\n", frames);
	printf("videoHash=%016llx\n", (unsigned long long)vh);
	printf("audioHash=%016llx\n", (unsigned long long)ah);
	for (int i = 0; i < 32; i++) {
		const void *data; int size, writable; const char *name;
		if (!qn_get_memory_area(e, i, &data, &size, &writable, &name)) break;
		printf("domain[%s]=%016llx\n", name, (unsigned long long)fnv(0, data, (size_t)size));
	}
	return 0;
}
