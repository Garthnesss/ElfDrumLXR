/* tail-render: load a kit, hit ONE voice once, render N seconds of tail.
   Phase 4 reverb gate workhorse: proves a decaying FX tail after the
   trigger stops and long-render stability (no runaway / NaN pollution).
   Output analysis lives in tools/reverb-gate.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"

static void wav_header(FILE *f, uint32_t frames)
{
	uint32_t rate = LXR_RATE, bytes = frames * 4, chunk = 36 + bytes, fmtlen = 16, byterate = rate * 4;
	uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
	fwrite("RIFF", 1, 4, f); fwrite(&chunk, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
	fwrite(&fmtlen, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
	fwrite(&byterate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
	fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
}

int main(int argc, char **argv)
{
	if (argc < 4) { fprintf(stderr, "usage: tail-render KIT.SND out.wav SECONDS [voice]\n"); return 2; }
	const double seconds = atof(argv[3]);
	const int voice = argc > 4 ? atoi(argv[4]) : 0;
	lxr_init();
	char name[9];
	uint8_t params[LXR_KIT_PARAMS];
	if (lxr_read_kit(argv[1], params, name)) { fprintf(stderr, "can't load %s\n", argv[1]); return 1; }
	lxr_load_params(params);
	FILE *out = fopen(argv[2], "wb");
	if (!out) { perror(argv[2]); return 1; }
	wav_header(out, 0);
	const long blocks = (long)(seconds * LXR_RATE / LXR_BLOCK);
	int16_t jack[LXR_JACKS][LXR_BLOCK], buf[LXR_BLOCK * 2];
	uint32_t frames = 0;
	lxr_hit(voice, 127);
	for (long b = 0; b < blocks; b++) {
		lxr_render(jack);
		for (int i = 0; i < LXR_BLOCK; i++)
			for (int c = 0; c < 2; c++) {
				int v = jack[c][i] + jack[2 + c][i];
				buf[2 * i + c] = v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
			}
		fwrite(buf, 2, LXR_BLOCK * 2, out);
		frames += LXR_BLOCK;
	}
	fseek(out, 0, SEEK_SET); wav_header(out, frames); fclose(out);
	printf("%s: hit voice %d, %.1f s -> %s\n", name, voice, frames / (double)LXR_RATE, argv[2]);
	return 0;
}
