/* ElfDrumLXR Phase 3 sequencer bridge: the firmware sequencer behind the wrapper's C API.
   Everything here goes through the firmware's own public functions and data (sequencer.c,
   EuklidGenerator.c) — no sequencer semantics are reimplemented. The two deliberate
   deviations, both forced by the firmware's host build, are marked inline:
   (1) track lengths are written to seq_patternSet.length[] directly because the firmware's
       seq_setTrackLength() operates on the front panel's shown pattern, and
   (2) lxr_init seeds PAR_PATTERN_MODE = 0 (see engine.c) so a pattern switch does not
       switch the FX bus off chasing a kit autoload whose target is a host stub. */
#include <stdio.h>
#include <string.h>
#include "engine.h"
#include "Soundengine/Sequencer/sequencer.h"
#include "Soundengine/Sequencer/EuklidGenerator.h"
#include "Preset/SeqStep.h"
#include "Parameters.h"

extern volatile uint32_t systick_ticks;   /* engine/stubs.c — the firmware's time base */
extern uint8_t seq_rollState;             /* sequencer.c globals not in the header */
extern uint8_t seq_rollRate;
extern uint8_t seq_songModeEnabled;

#define STEPS_PER_PATTERN 64   /* presetManager.c's define (NUM_STEPS) */

/* ------------------------------------------------------------------ time */
uint32_t lxr_seq_systick(void) { return systick_ticks; }

void lxr_seq_pump(uint32_t target)
{
	/* seq_tick() advances at most one prescale tick per call and folds overshoot into
	   the next interval, so the pump advances the counter one unit at a time — the same
	   discipline the reference harness uses, which is what makes their trigger logs
	   comparable sample-for-sample. */
	while (systick_ticks < target) {
		systick_ticks++;
		seq_tick();
	}
}

/* ------------------------------------------------------------- transport */
void lxr_seq_set_running(int running) { seq_setRunning(running ? 1 : 0); }
int lxr_seq_is_running(void) { return seq_isRunning(); }
void lxr_seq_set_bpm(int bpm) { if (bpm > 0) seq_setBpm((uint16_t)bpm); }
int lxr_seq_get_bpm(void) { return seq_getBpm(); }

void lxr_seq_seek(int globalStep, int prescale)
{
	if (!seq_isRunning() || globalStep < 0)
		return;
	/* seq_stepIndex[] is file-static; the only public way to move it is seq_nextStep().
	   Muting every track first makes the walk silent (mute gates seq_triggerVoice,
	   including its parameter-lock application); indices, master counter, probability
	   rolls and any pending-pattern/song advance crossed on the way still happen,
	   exactly as if those steps had played. */
	uint8_t saved[LXR_SEQ_TRACKS];
	for (int t = 0; t < LXR_SEQ_TRACKS; t++) {
		saved[t] = seq_isTrackMuted((uint8_t)t);
		seq_setMute((uint8_t)t, 1);
	}
	for (int i = 0; i < globalStep; i++)
		seq_nextStep();
	for (int t = 0; t < LXR_SEQ_TRACKS; t++)
		seq_setMute((uint8_t)t, saved[t]);
	if (prescale < 0) prescale = 0;
	if (prescale > 23) prescale = 23;
	/* Sets the prescale phase and re-arms the tick phase at the current systick
	   (the sub-prescale-tick fraction is dropped — the firmware's own external-sync
	   seek has the same granularity). */
	seq_MidiSyncQueue((uint8_t)prescale);
}

void lxr_seq_set_ext_sync(int mode) { seq_setExtSync(mode ? 1 : 0); }
int lxr_seq_get_ext_sync(void) { return seq_getExtSync(); }

/* -------------------------------------------------------------- patterns */
int lxr_seq_active_pattern(void) { return seq_activePattern; }
int lxr_seq_pending_pattern(void) { return seq_pendingPattern; }

void lxr_seq_commit_pattern(int pattern)
{
	/* preset_savePatternRAM() semantics: copying the live pattern into the backup
	   buffer is what "save" means for the currently playing pattern, because the
	   switch path restores the backup over the pattern being left. */
	if (pattern == seq_activePattern)
		seq_copyPatternToBackupBuffer((uint8_t)pattern);
}

void lxr_seq_request_pattern(int pattern)
{
	if (pattern < 0 || pattern >= LXR_SEQ_PATTERNS)
		return;
	lxr_seq_commit_pattern(seq_activePattern);
	seq_setNextPattern((uint8_t)pattern);
}

void lxr_seq_copy_pattern(int src, int dst)
{
	if (src < 0 || src >= LXR_SEQ_PATTERNS || dst < 0 || dst >= LXR_SEQ_PATTERNS)
		return;
	seq_copyPattern((uint8_t)src, (uint8_t)dst);
	if (dst == seq_activePattern)
		lxr_seq_commit_pattern(dst);
}

void lxr_seq_clear_pattern(int pattern)
{
	if (pattern < 0 || pattern >= LXR_SEQ_PATTERNS)
		return;
	seq_clearPattern((uint8_t)pattern);
	if (pattern == seq_activePattern)
		lxr_seq_commit_pattern(pattern);
}

void lxr_seq_clear_track(int pattern, int track)
{
	if (pattern < 0 || pattern >= LXR_SEQ_PATTERNS || track < 0 || track >= LXR_SEQ_TRACKS)
		return;
	seq_clearTrack((uint8_t)track, (uint8_t)pattern);
	if (pattern == seq_activePattern)
		lxr_seq_commit_pattern(pattern);
}

/* ----------------------------------------------------------------- steps */
static int step_ok(int pattern, int track, int step)
{
	return pattern >= 0 && pattern < LXR_SEQ_PATTERNS && track >= 0 && track < LXR_SEQ_TRACKS
	    && step >= 0 && step < LXR_SEQ_STEPS;
}

static void step_decode(const Step *s, const StepTiming *tm, LxrStep *out)
{
	out->volume = (uint8_t)(s->volume & 0x7f);
	out->prob = s->prob;
	out->note = s->note;
	out->p1nr = s->param1Nr; out->p1val = s->param1Val;
	out->p2nr = s->param2Nr; out->p2val = s->param2Val;
	out->flam = tm->flam;
	out->shift = (int8_t)tm->shift - 7;
}

static void step_encode(const LxrStep *in, Step *s, StepTiming *tm)
{
	/* Keep the firmware's vestigial active bit in step with the main-step state is
	   playback-irrelevant; seq_resetNote sets volume 127 (bit set). We preserve the
	   stored bit so state round-trips are byte-stable. */
	s->volume = (uint8_t)((s->volume & 0x80) | (in->volume & 0x7f));
	s->prob = in->prob > 100 ? 100 : in->prob;
	s->note = (uint8_t)(in->note & 0x7f);
	s->param1Nr = in->p1nr; s->param1Val = in->p1val;
	s->param2Nr = in->p2nr; s->param2Val = in->p2val;
	tm->flam = in->flam > 5 ? 5 : in->flam;
	int sh = in->shift + 7;
	tm->shift = (uint8_t)(sh < 0 ? 0 : sh > 14 ? 14 : sh);
}

void lxr_seq_get_step(int pattern, int track, int step, LxrStep *out)
{
	if (!step_ok(pattern, track, step)) { memset(out, 0, sizeof *out); return; }
	step_decode(&seq_patternSet.seq_subStepPattern[pattern][track][step],
	            &seq_patternSet.seq_stepTiming[pattern][track][step], out);
}

void lxr_seq_set_step(int pattern, int track, int step, const LxrStep *in)
{
	if (!step_ok(pattern, track, step))
		return;
	step_encode(in, &seq_patternSet.seq_subStepPattern[pattern][track][step],
	            &seq_patternSet.seq_stepTiming[pattern][track][step]);
}

int lxr_seq_main_step(int pattern, int track, int step)
{
	if (!step_ok(pattern, track, step)) return 0;
	return seq_isMainStepActive((uint8_t)track, (uint8_t)step, (uint8_t)pattern);
}

void lxr_seq_set_main_step(int pattern, int track, int step, int on)
{
	if (!step_ok(pattern, track, step)) return;
	seq_setMainStep((uint8_t)pattern, (uint8_t)track, (uint8_t)step, on ? 1 : 0);
}

/* ------------------------------------------------------- track / pattern */
int lxr_seq_track_length(int pattern, int track)
{
	if (pattern < 0 || pattern >= LXR_SEQ_PATTERNS || track < 0 || track >= LXR_SEQ_TRACKS)
		return 16;
	const uint8_t len = seq_patternSet.length[pattern][track];
	return len == 0 ? 16 : len;
}

void lxr_seq_set_track_length(int pattern, int track, int len)
{
	if (pattern < 0 || pattern >= LXR_SEQ_PATTERNS || track < 0 || track >= LXR_SEQ_TRACKS)
		return;
	if (len < 1) len = 1;
	if (len > 64) len = 64;
	seq_patternSet.length[pattern][track] = (uint8_t)len;
}

int lxr_seq_mute(int track)
{
	if (track < 0 || track >= LXR_SEQ_TRACKS) return 0;
	return seq_isTrackMuted((uint8_t)track);
}

void lxr_seq_set_mute(int track, int muted)
{
	if (track < 0 || track >= LXR_SEQ_TRACKS) return;
	seq_setMute((uint8_t)track, muted ? 1 : 0);
}

int lxr_seq_shuffle(int pattern)
{
	if (pattern < 0 || pattern >= LXR_SEQ_PATTERNS) return 0;
	return seq_patternSet.shuffle[pattern];
}

void lxr_seq_set_shuffle(int pattern, int value)
{
	if (pattern < 0 || pattern >= LXR_SEQ_PATTERNS) return;
	if (value < 0) value = 0;
	if (value > 127) value = 127;
	seq_patternSet.shuffle[pattern] = (uint8_t)value;
	if (pattern == seq_activePattern) {
		/* The same live-apply seq_setPendingPatternActive() performs on a switch. */
		extern uint8_t activeParameters[];
		activeParameters[PAR_SHUFFLE] = (uint8_t)value;
		seq_setShuffle(value / 127.f);
	}
}

int lxr_seq_kit_nr(int pattern)
{
	if (pattern < 0 || pattern >= LXR_SEQ_PATTERNS) return 0;
	return seq_patternSet.kitNr[pattern];
}

void lxr_seq_set_kit_nr(int pattern, int kitNr)
{
	if (pattern < 0 || pattern >= LXR_SEQ_PATTERNS) return;
	seq_patternSet.kitNr[pattern] = (uint8_t)kitNr;
}

/* ------------------------------------------------------------------ rolls */
int lxr_seq_roll(int track)
{
	if (track < 0 || track >= LXR_SEQ_TRACKS) return 0;
	return (seq_rollState >> track) & 1;
}

void lxr_seq_set_roll(int track, int on)
{
	if (track < 0 || track >= LXR_SEQ_TRACKS) return;
	seq_setRoll((uint8_t)track, on ? 1 : 0);
}

void lxr_seq_set_roll_rate(int menuIdx) { seq_setRollRate((uint8_t)menuIdx); }
int lxr_seq_roll_rate(void) { return seq_rollRate; }

/* ----------------------------------------------------------------- euklid */
void lxr_seq_euklid(int track, int pattern, int length, int steps, int rotation)
{
	if (track < 0 || track >= LXR_SEQ_TRACKS || pattern < 0 || pattern >= LXR_SEQ_PATTERNS)
		return;
	if (length < 1) length = 1;
	if (length > 64) length = 64;
	/* euklid_setSteps/setRotation generate and transfer into the pattern set
	   (USE_PATTERN_EDIT_BUFFER is 0 in this firmware). Lengths above 16 exceed the
	   generator's 16-step buffer design; the firmware front panel caps at 16 too,
	   so clamp like it does. */
	if (length > 16) length = 16;
	euklid_setLength((uint8_t)track, (uint8_t)length);
	euklid_setSteps((uint8_t)track, (uint8_t)steps, (uint8_t)pattern);
	if (rotation)
		euklid_setRotation((uint8_t)track, (uint8_t)rotation, (uint8_t)pattern);
	if (pattern == seq_activePattern)
		lxr_seq_commit_pattern(pattern);
}

/* ------------------------------------------------------------------- song */
void lxr_seq_set_song_mode(int on) { seq_setSongModeEnabled(on ? 1 : 0); }
int lxr_seq_song_mode(void) { return seq_songModeEnabled; }
int lxr_seq_song_position(void) { return seq_songPositionPointer; }

int lxr_seq_song_slot(void)
{
	for (int i = 0; i < NUM_KIT_SLOTS; i++)
		if (seq_activeSong == seq_songData[i])
			return i;
	return 0;
}

void lxr_seq_set_song_slot(int slot)
{
	if (slot < 0 || slot >= NUM_KIT_SLOTS) return;
	seq_activeSong = seq_songData[slot];
}

int lxr_seq_song_entry(int slot, int pos)
{
	if (slot < 0 || slot >= NUM_KIT_SLOTS || pos < 0 || pos >= MAX_SONG_LENGTH) return LXR_SONG_END;
	return seq_songData[slot][pos];
}

void lxr_seq_set_song_entry(int slot, int pos, int value)
{
	if (slot < 0 || slot >= NUM_KIT_SLOTS || pos < 0 || pos >= MAX_SONG_LENGTH) return;
	seq_songData[slot][pos] = (uint8_t)value;
}

int lxr_seq_step_index(int track)
{
	if (track < 0 || track >= LXR_SEQ_TRACKS) return 0;
	return seq_getStepIndex((uint8_t)track);
}

/* ------------------------------------------------------------ whole state */
size_t lxr_seq_patternset_size(void) { return sizeof(PatternSet); }

void lxr_seq_patternset_read(void *out) { memcpy(out, &seq_patternSet, sizeof(PatternSet)); }

void lxr_seq_patternset_write(const void *in)
{
	memcpy(&seq_patternSet, in, sizeof(PatternSet));
	lxr_seq_commit_pattern(seq_activePattern);
	/* the restored shuffle of the active pattern applies live, as on a switch */
	lxr_seq_set_shuffle(seq_activePattern, seq_patternSet.shuffle[seq_activePattern]);
}

size_t lxr_seq_songdata_size(void) { return sizeof(seq_songData); }
void lxr_seq_songdata_read(void *out) { memcpy(out, seq_songData, sizeof(seq_songData)); }
void lxr_seq_songdata_write(const void *in) { memcpy(seq_songData, in, sizeof(seq_songData)); }

/* ------------------------------------------------------ snapshot decoding */
static const PatternSet *snap_set(const void *snap) { return (const PatternSet *)snap; }

void lxr_seq_snap_get_step(const void *snap, int pattern, int track, int step, LxrStep *out)
{
	if (!step_ok(pattern, track, step)) { memset(out, 0, sizeof *out); return; }
	step_decode(&snap_set(snap)->seq_subStepPattern[pattern][track][step],
	            &snap_set(snap)->seq_stepTiming[pattern][track][step], out);
}

int lxr_seq_snap_main_step(const void *snap, int pattern, int track, int step)
{
	if (!step_ok(pattern, track, step)) return 0;
	return (snap_set(snap)->seq_mainSteps[step / 16][pattern][track] >> (step % 16)) & 1;
}

int lxr_seq_snap_track_length(const void *snap, int pattern, int track)
{
	if (pattern < 0 || pattern >= LXR_SEQ_PATTERNS || track < 0 || track >= LXR_SEQ_TRACKS)
		return 16;
	const uint8_t len = snap_set(snap)->length[pattern][track];
	return len == 0 ? 16 : len;
}

int lxr_seq_snap_shuffle(const void *snap, int pattern)
{
	if (pattern < 0 || pattern >= LXR_SEQ_PATTERNS) return 0;
	return snap_set(snap)->shuffle[pattern];
}

int lxr_seq_snap_kit_nr(const void *snap, int pattern)
{
	if (pattern < 0 || pattern >= LXR_SEQ_PATTERNS) return 0;
	return snap_set(snap)->kitNr[pattern];
}

/* ------------------------------------------------------------- .PAT files */
/* Ported from presetManager.c (preset_writePatternData / preset_readPatternData /
   preset_loadPatternSD / preset_savePatternSD / preset_loadSongSD / preset_saveSongSD),
   FatFs calls replaced by stdio, one pattern per file. The uint16 main-step masks are
   written as native uint16 values exactly like the firmware (little-endian on the
   hardware and on every supported host). */
static int wr(FILE *f, const void *p, size_t n) { return fwrite(p, 1, n, f) == n ? 0 : -1; }
static int rd(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n ? 0 : -1; }

int lxr_pat_save_pattern(const char *path, int pattern, const char name[8])
{
	if (pattern < 0 || pattern >= LXR_SEQ_PATTERNS) return -1;
	FILE *f = fopen(path, "wb");
	if (!f) return -1;
	int rc = 0;
	rc |= wr(f, name, 8);
	rc |= wr(f, "PatV03", 6);
	for (int trk = 0; trk < NUM_TRACKS && !rc; trk++) {
		rc |= wr(f, &seq_patternSet.seq_subStepPattern[pattern][trk][0], sizeof(Step) * STEPS_PER_PATTERN);
		rc |= wr(f, &seq_patternSet.seq_stepTiming[pattern][trk][0], sizeof(uint8_t) * STEPS_PER_PATTERN);
	}
	for (int trk = 0; trk < NUM_TRACKS && !rc; trk++)
		for (int bar = 0; bar < NUM_BARS && !rc; bar++) {
			const uint16_t mainStepData = seq_patternSet.seq_mainSteps[bar][pattern][trk];
			rc |= wr(f, &mainStepData, sizeof(uint16_t));
		}
	rc |= wr(f, &seq_patternSet.shuffle[pattern], 1);
	for (int trk = 0; trk < NUM_TRACKS && !rc; trk++)
		rc |= wr(f, &seq_patternSet.length[pattern][trk], 1);
	rc |= wr(f, &seq_patternSet.kitNr[pattern], 1);
	if (fclose(f) != 0) rc = -1;
	return rc;
}

int lxr_pat_load_pattern(const char *path, int pattern, char nameOut[8])
{
	if (pattern < 0 || pattern >= LXR_SEQ_PATTERNS) return -1;
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	char name[8];
	if (rd(f, name, 8)) { fclose(f); return -1; }
	/* version tag, exactly as preset_loadPatternSD parses it */
	char buf[7];
	uint8_t fileVersion = 1;
	if (rd(f, buf, 6)) { fclose(f); return -1; }
	buf[6] = 0;
	{
		char num[3] = { buf[4], buf[5], 0 };
		buf[4] = 0;
		if (strcmp(buf, "PatV") == 0)
			fileVersion = (uint8_t)atoi(num);
		else {
			fileVersion = 1;               /* V1: no tag — rewind the 6 bytes */
			if (fseek(f, -6, SEEK_CUR)) { fclose(f); return -1; }
		}
	}
	int success = 1;
	for (int trk = 0; trk < NUM_TRACKS && success; trk++) {
		if (rd(f, &seq_patternSet.seq_subStepPattern[pattern][trk][0], sizeof(Step) * STEPS_PER_PATTERN))
			success = 0;
		else if (fileVersion > 2
		         && rd(f, &seq_patternSet.seq_stepTiming[pattern][trk][0], sizeof(uint8_t) * STEPS_PER_PATTERN))
			success = 0;
	}
	for (int trk = 0; trk < NUM_TRACKS && success; trk++)
		for (int bar = 0; bar < NUM_BARS && success; bar++) {
			uint16_t mainStepData;
			if (rd(f, &mainStepData, sizeof(uint16_t)))
				success = 0;
			else
				seq_patternSet.seq_mainSteps[bar][pattern][trk] = mainStepData;
		}
	if (success && rd(f, &seq_patternSet.shuffle[pattern], 1))
		success = 0;
	if (success) {
		/* The firmware tolerates a short/missing length block (it keeps reading
		   into `next` without checking); the port leaves the remaining lengths
		   untouched on EOF instead of replicating the stale-byte behaviour.
		   Well-formed files are unaffected. */
		for (int trk = 0; trk < NUM_TRACKS; trk++) {
			uint8_t next;
			if (rd(f, &next, 1)) break;
			seq_patternSet.length[pattern][trk] = next;
		}
	}
	if (success && fileVersion > 1) {
		if (rd(f, &seq_patternSet.kitNr[pattern], 1))
			success = 0;
	}
	fclose(f);
	if (!success) return -1;
	if (nameOut) memcpy(nameOut, name, 8);
	if (pattern == seq_activePattern)
		lxr_seq_commit_pattern(pattern);
	return 0;
}

int lxr_pat_save_song(const char *path, int slot, const char name[8])
{
	if (slot < 0 || slot >= NUM_KIT_SLOTS) return -1;
	FILE *f = fopen(path, "wb");
	if (!f) return -1;
	int rc = 0;
	rc |= wr(f, name, 8);
	rc |= wr(f, seq_songData[slot], MAX_SONG_LENGTH);
	if (fclose(f) != 0) rc = -1;
	return rc;
}

int lxr_pat_load_song(const char *path, int slot, char nameOut[8])
{
	if (slot < 0 || slot >= NUM_KIT_SLOTS) return -1;
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	char name[8];
	int rc = 0;
	rc |= rd(f, name, 8);
	rc |= rd(f, seq_songData[slot], MAX_SONG_LENGTH);
	fclose(f);
	if (rc) return -1;
	if (nameOut) memcpy(nameOut, name, 8);
	return 0;
}
