/* seq-script: the shared deterministic sequencer script for Phase 3 gate (b).
   Built ONLY through the wrapper API (engine.h), identically by the reference
   harness and the plugin harness. All steps prob = 100 so the firmware's
   probability rolls (fed by the host RNG stub, arc4random) cannot change the
   trigger set; probability is exercised separately. Fixed 120 BPM: one 16th
   step = 500 systick units, one bar = 8000, pattern span = 16 steps. */
#pragma once
#include "engine.h"

static inline void seq_script_step(int pat, int trk, int step, int on,
                                   int vel, int note, int p1n, int p1v, int p2n, int p2v,
                                   int flam, int shift)
{
	LxrStep s;
	s.volume = (uint8_t)vel; s.prob = 100; s.note = (uint8_t)note;
	s.p1nr = (uint8_t)p1n; s.p1val = (uint8_t)p1v;
	s.p2nr = (uint8_t)p2n; s.p2val = (uint8_t)p2v;
	s.flam = (uint8_t)flam; s.shift = (int8_t)shift;
	lxr_seq_set_step(pat, trk, step, &s);
	lxr_seq_set_main_step(pat, trk, step, on);
}

static inline void seq_script_build(void)
{
	const int X = LXR_SEQ_NO_AUTO;
	for (int pat = 0; pat < 2; pat++)
		for (int trk = 0; trk < LXR_SEQ_TRACKS; trk++)
			lxr_seq_set_track_length(pat, trk, 16);

	/* pattern 0 */
	seq_script_step(0, 0, 0,  1, 100, 60, 16, 30, X, 0, 0, 0);    /* drum1, p-lock 1 */
	seq_script_step(0, 0, 4,  1, 90, 60, X, 0, X, 0, 0, 0);
	seq_script_step(0, 0, 8,  1, 100, 60, X, 0, 20, 99, 0, 0);   /* p-lock 2 */
	seq_script_step(0, 0, 12, 1, 80, 60, X, 0, X, 0, 0, 0);
	seq_script_step(0, 1, 2,  1, 110, 62, X, 0, X, 0, 0, 0);     /* drum2 */
	seq_script_step(0, 1, 6,  1, 110, 62, X, 0, X, 0, 2, 0);     /* flam x3 */
	seq_script_step(0, 1, 10, 1, 110, 62, X, 0, X, 0, 0, 0);
	seq_script_step(0, 1, 14, 1, 110, 62, X, 0, X, 0, 0, 0);
	seq_script_step(0, 3, 4,  1, 127, 58, X, 0, X, 0, 0, 0);     /* snare */
	seq_script_step(0, 3, 12, 1, 127, 58, X, 0, X, 0, 0, 3);     /* shift +3 */
	for (int st = 0; st < 16; st += 2)                            /* hihat eighths */
		seq_script_step(0, 5, st, 1, 70, 66, X, 0, X, 0, st == 4 ? 1 : 0, 0);   /* flam x2 on step 4 */
	seq_script_step(0, 6, 14, 1, 95, 68, X, 0, X, 0, 0, 0);      /* open hat */
	seq_script_step(0, 2, 15, 1, 85, 50, X, 0, X, 0, 0, -2);     /* drum3, shift -2 */

	/* pattern 1 (switch target) */
	seq_script_step(1, 0, 0,  1, 120, 55, X, 0, X, 0, 0, 0);
	seq_script_step(1, 0, 3,  1, 120, 55, X, 0, X, 0, 0, 0);
	seq_script_step(1, 0, 6,  1, 120, 55, 16, 77, X, 0, 0, 0);
	seq_script_step(1, 0, 10, 1, 120, 55, X, 0, X, 0, 0, 0);
	seq_script_step(1, 0, 12, 1, 120, 55, X, 0, X, 0, 0, 0);
	seq_script_step(1, 4, 8,  1, 100, 71, X, 0, X, 0, 0, 0);     /* cymbal */
}

/* Gate (b) run plan (both harnesses): BPM 120, start on pattern 0, request
   pattern 1 after exactly 1 bar of pumping (inside bar 2, so the firmware's
   pending mechanism switches at step 32), run 4 bars + 1 step of slack. */
#define SEQ_SCRIPT_BPM 120
#define SEQ_SCRIPT_SWITCH_AFTER_UNITS 8000u    /* 1 bar at 120 BPM */
#define SEQ_SCRIPT_TOTAL_UNITS (4u * 8000u + 500u)
