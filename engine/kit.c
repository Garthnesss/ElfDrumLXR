/* Kit file IO + the firmware's kit morph. Adapted from mechanical-dance/PHX-D01 host/kit.c
   (GPL-3.0; see NOTICE); the module mod-matrix tail handling is dropped (desktop engine). */
#include <stdio.h>
#include <string.h>
#include "engine.h"
#include "Parameters.h"

int lxr_read_kit(const char *path, uint8_t params[LXR_KIT_PARAMS], char name[9])
{
	uint8_t k[8 + LXR_KIT_PARAMS + 54 + 1];
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	size_t n = fread(k, 1, sizeof k, f);
	fclose(f);
	if (n < 8 + LXR_KIT_PARAMS) return -2;
	memcpy(name, k, 8); name[8] = 0;
	for (int i = 7; i >= 0 && (name[i] == ' ' || name[i] == 0); i--) name[i] = 0;
	memcpy(params, k + 8, LXR_KIT_PARAMS);
	/* A 309-byte file is a module kit: its extra 54 mod-matrix bytes are ignored here. */
	return 0;
}

/* PresetManager.c interpolate(): fixed point, rounding half up. */
static uint8_t interpolate(uint8_t a, uint8_t b, uint8_t x)
{
	uint16_t v = (uint16_t)(a * 256 + (b - a) * x);
	return (uint8_t)((v & 0xff) < 0x7f ? v / 256 : v / 256 + 1);
}

void lxr_morph_kit(const uint8_t a[LXR_KIT_PARAMS], const uint8_t b[LXR_KIT_PARAMS], uint8_t amount, uint8_t out[LXR_KIT_PARAMS])
{
	for (int i = 0; i < LXR_KIT_PARAMS; i++) {
		int keep = (i >= PAR_AUDIO_OUT1 && i <= PAR_AUDIO_OUT6)          /* "Do not morph audio routing" */
		        || (i >= PAR_VEL_DEST_1 && i < PAR_VEL_DEST_1 + 6)       /* "Mod targets are not morphed!!!" */
		        || (i >= PAR_TARGET_LFO1 && i < PAR_TARGET_LFO1 + 6);
		out[i] = keep ? a[i] : interpolate(a[i], b[i], amount);
	}
}
