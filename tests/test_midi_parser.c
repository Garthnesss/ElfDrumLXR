/* test_midi_parser: the firmware's external-MIDI parser (midiParser_parseUartData, reached via
   lxr_midi_bytes) works in the host harness — the plugin feeds it every non-note MIDI byte.
   Firmware protocol facts this test pins down (from MidiParser.c / MidiMessages.h):
   - Plain CC n sets parameter n-1 (paramNr = data1-1): CC38 is PAR_FILTER_FREQ_1.
   - "NRPN" is CC99/CC98 selecting a CC2 number (= parameter index - 128) and CC6 entering the
     value. There is NO data-entry-fine handling: CC38 stays a live parameter CC.
   - Factory global defaults (preset_loadDefaultGlobals): voice notes all 48, voice channels
     all 1 (stored 1-based), global channel 0 ("all"). */
#include <stdio.h>
#include <string.h>
#include "engine.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL: %s\n", #cond); failures++; } } while (0)

static int16_t ref[200 * 64 * 2], buf[200 * 64 * 2];

static long render_drum1(int16_t *out)
{
	int16_t jack[LXR_JACKS][LXR_BLOCK];
	long peak = 0;
	int n = 0;
	lxr_hit(0, 127);
	for (int b = 0; b < 200; b++) {
		lxr_render(jack);
		for (int i = 0; i < LXR_BLOCK; i++) { out[n++] = jack[0][i]; out[n++] = jack[1][i]; }
	}
	for (int i = 0; i < n; i++) { int v = out[i]; if (v < 0) v = -v; if (v > peak) peak = v; }
	return peak;
}

int main(void)
{
	lxr_init();
	CHECK(lxr_midi_note(0) == 48 && lxr_midi_note(6) == 48);
	CHECK(lxr_midi_channel(0) == 0 && lxr_midi_channel(6) == 0);
	CHECK(lxr_global_channel() == 0);

	uint8_t params[LXR_KIT_PARAMS];
	char name[9];
	if (lxr_read_kit("factory/INIT.SND", params, name)) { printf("FAIL: kit load\n"); return 1; }
	lxr_load_params(params);
	CHECK(render_drum1(ref) > 10000);

	/* NRPN: select CC2 #0 (PAR_FILTER_DRIVE_1), enter 127 -> driven kick, louder, different */
	const uint8_t nrpn[] = {0xB0, 99, 0, 0xB0, 98, 0, 0xB0, 6, 127};
	lxr_midi_bytes(nrpn, sizeof nrpn);
	long peak = render_drum1(buf);
	CHECK(peak > 20000);
	CHECK(memcmp(ref, buf, sizeof ref) != 0);

	/* plain CC38 -> PAR_FILTER_FREQ_1 = 30 -> darker kick, still loud, different */
	lxr_load_params(params);
	render_drum1(ref);
	const uint8_t cc[] = {0xB0, 38, 30};
	lxr_midi_bytes(cc, sizeof cc);
	peak = render_drum1(buf);
	CHECK(peak > 5000);
	CHECK(memcmp(ref, buf, sizeof ref) != 0);

	if (!failures) printf("midi parser checks passed\n");
	return failures != 0;
}
