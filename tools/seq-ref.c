/* seq-ref: Phase 3 gate (b) REFERENCE harness + gate (c) PHX gap evidence.
   Drives the firmware sequencer directly through the wrapper pump, in 64-frame
   engine blocks with the same frame->systick rule the plugin's EngineHost uses
   (target = llround(frame * 4000/44140)), and prints the trigger log to stdout
   (see tools/seq-log.h). The plugin harness prints the identical format; the
   gate is a byte diff of the two stdouts.
   Gate (c) checks print to stderr as PASS/FAIL lines; exit code covers both. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "engine.h"
#include "seq-log.h"
#include "seq-script.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); failures++; } } while (0)
#define PASS(...) do { fprintf(stderr, "PASS: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)

/* Pump nSystick units in 64-frame engine blocks (the plugin's discipline). */
static void pump_units(uint32_t units)
{
	static uint64_t frame = 0;
	const uint32_t until = lxr_seq_systick() + units;
	while (lxr_seq_systick() < until) {
		frame += 64;
		lxr_seq_pump((uint32_t)llround((double)frame * 4000.0 / 44140.0));
	}
}

int main(void)
{
	lxr_init();
	seq_script_build();
	lxr_seq_set_bpm(SEQ_SCRIPT_BPM);

	/* ---------------- gate (b): the equivalence run ---------------- */
	seqlog_reset();
	lxr_seq_set_running(1);
	int switched = 0;
	{
		uint64_t frame = 0;
		while (lxr_seq_systick() < SEQ_SCRIPT_TOTAL_UNITS) {
			frame += 64;
			lxr_seq_pump((uint32_t)llround((double)frame * 4000.0 / 44140.0));
			if (!switched && lxr_seq_systick() >= SEQ_SCRIPT_SWITCH_AFTER_UNITS) {
				lxr_seq_request_pattern(1);
				switched = 1;
			}
		}
	}
	lxr_seq_set_running(0);
	fprintf(stderr, "gate(b) reference run: %d triggers, active pattern %d\n",
	        seqlog_count(), lxr_seq_active_pattern());
	/* stdout carries ONLY the gate (b) log (the gate diffs it byte-for-byte
	   against the plugin harness); gate (c) phases keep logging to /dev/null. */
	fflush(stdout);
	freopen("/dev/null", "w", stdout);
	CHECK(seqlog_count() > 40, "reference run produced only %d triggers", seqlog_count());
	CHECK(lxr_seq_active_pattern() == 1, "pattern switch did not happen (active %d)", lxr_seq_active_pattern());

	/* ---------------- gate (c): PHX-D01 gap checklist ----------------
	   PHX-D01's sequencer: 7 tracks x max 32 steps, [velocity, note] only,
	   8 pattern slots, host swing, no probability, no p-locks, no flam/shift,
	   no chaining, no song mode. Each item below exercises the firmware path
	   PHX lacks, through the wrapper (the same calls the editor makes). */

	/* 64 steps x 64 patterns (PHX: 32 steps, 8 slots) + full step detail */
	{
		LxrStep in, out;
		memset(&in, 0, sizeof in);
		in.volume = 77; in.prob = 37; in.note = 43;
		in.p1nr = 100; in.p1val = 111; in.p2nr = 200; in.p2val = 22;
		in.flam = 5; in.shift = -7;
		lxr_seq_set_step(63, 6, 63, &in);
		lxr_seq_set_main_step(63, 6, 63, 1);
		lxr_seq_get_step(63, 6, 63, &out);
		CHECK(out.volume == 77 && out.prob == 37 && out.note == 43, "step 63 of pattern 63 round-trip");
		CHECK(out.p1nr == 100 && out.p1val == 111 && out.p2nr == 200 && out.p2val == 22, "p-locks round-trip");
		CHECK(out.flam == 5 && out.shift == -7, "flam/shift round-trip (flam %d shift %d)", out.flam, out.shift);
		CHECK(lxr_seq_main_step(63, 6, 63) == 1, "main step 63 round-trip");
		PASS("64 steps x 64 patterns, probability, 2 p-locks, flam, shift — data paths round-trip");
	}

	/* probability is live, not just stored: prob=50 over 400 steps ~ 200 hits */
	{
		lxr_seq_clear_pattern(2);
		lxr_seq_set_track_length(2, 0, 1);
		LxrStep s; memset(&s, 0, sizeof s);
		s.volume = 100; s.prob = 50; s.note = 60; s.p1nr = s.p2nr = 0xff;
		lxr_seq_set_step(2, 0, 0, &s);
		lxr_seq_set_main_step(2, 0, 0, 1);
		lxr_seq_request_pattern(2);
		seqlog_reset();
		lxr_seq_set_running(1);
		pump_units(400 * 500);
		lxr_seq_set_running(0);
		int hits = 0;
		for (int i = 0; i < seqlog_time_count(); i++) if (seqlog_voice(i) == 0) hits++;
		CHECK(hits > 140 && hits < 260, "prob=50 gave %d/400 hits (want ~200)", hits);
		fprintf(stderr, "PASS: probability live — prob=50 step fired %d/400 times\n", hits);
	}

	/* shuffle is the firmware's own (pattern byte), not a host swing:
	   odd steps are delayed — the two inter-onset gaps must straddle 500. */
	{
		lxr_seq_clear_pattern(4);
		for (int trk = 0; trk < 7; trk++) lxr_seq_set_track_length(4, trk, 16);
		LxrStep s; memset(&s, 0, sizeof s);
		s.volume = 100; s.prob = 100; s.note = 60; s.p1nr = s.p2nr = 0xff;
		for (int st = 0; st < 4; st++) { lxr_seq_set_step(4, 0, st, &s); lxr_seq_set_main_step(4, 0, st, 1); }
		lxr_seq_set_shuffle(4, 96);
		lxr_seq_request_pattern(4);
		seqlog_reset();
		lxr_seq_set_running(1);
		pump_units(3 * 8000);   /* bar 1 may be the switch bar; measure the last bar */
		lxr_seq_set_running(0);
		uint32_t t[4]; int n = 0;
		for (int i = 0; i < seqlog_time_count() && n < 64; i++)
			if (seqlog_voice(i) == 0) t[n < 4 ? n : 3] = seqlog_time(i), n++;
		/* collect the final 4 onsets */
		uint32_t last[4]; int cnt = 0;
		for (int i = 0; i < seqlog_time_count(); i++)
			if (seqlog_voice(i) == 0) { last[cnt % 4] = seqlog_time(i); cnt++; }
		CHECK(cnt >= 8, "shuffle run produced %d onsets", cnt);
		/* onsets in the final bar, in order: */
		uint32_t bar[4]; int bi = 0;
		for (int i = 0; i < seqlog_time_count(); i++)
			if (seqlog_voice(i) == 0 && seqlog_time(i) > 2 * 8000) { if (bi < 4) bar[bi++] = seqlog_time(i); }
		if (bi == 4) {
			const int g01 = (int)bar[1] - (int)bar[0], g12 = (int)bar[2] - (int)bar[1];
			fprintf(stderr, "shuffle gaps: step0->1 = %d units, step1->2 = %d units (unshuffled = 500)\n", g01, g12);
			CHECK((g01 - 500) * (g12 - 500) < 0 && abs(g01 - 500) > 40,
			      "shuffle did not straddle (gaps %d/%d)", g01, g12);
			PASS("firmware shuffle delays alternate steps (gaps %d / %d)", g01, g12);
		} else {
			CHECK(0, "shuffle bar had %d onsets", bi);
		}
		(void)t; (void)n;
	}

	/* song mode + chaining: song [0, 1, END] must play pattern 0, then 1, then stop */
	{
		for (int pos = 0; pos < 64; pos++) lxr_seq_set_song_entry(0, pos, LXR_SONG_END);
		lxr_seq_set_song_entry(0, 0, 0);
		lxr_seq_set_song_entry(0, 1, 1);
		lxr_seq_set_song_entry(0, 2, LXR_SONG_END);
		lxr_seq_set_song_slot(0);
		lxr_seq_set_song_mode(1);
		lxr_seq_request_pattern(0);
		seqlog_reset();
		lxr_seq_set_running(1);
		int sawPattern1 = 0, stopped = 0;
		for (int bar = 0; bar < 8 && !stopped; bar++) {
			pump_units(8000);
			if (lxr_seq_active_pattern() == 1) sawPattern1 = 1;
			if (!lxr_seq_is_running()) stopped = 1;
		}
		lxr_seq_set_song_mode(0);
		lxr_seq_set_running(0);
		CHECK(sawPattern1, "song never advanced to pattern 1");
		CHECK(stopped, "SONG_END did not stop the sequencer");
		PASS("song mode chains patterns and SONG_END stops playback");
	}

	/* rolls: arming a roll while stopped one-shot triggers the voice (firmware) */
	{
		seqlog_reset();
		lxr_seq_set_roll(0, 1);
		CHECK(seqlog_count() >= 1, "roll one-shot produced no trigger");
		lxr_seq_set_roll(0, 0);
		PASS("roll arm one-shot trigger (count %d)", seqlog_count());
	}

	fprintf(stderr, failures ? "seq-ref: %d FAILURES\n" : "seq-ref: ALL PASS\n", failures);
	return failures ? 1 : 0;
}
