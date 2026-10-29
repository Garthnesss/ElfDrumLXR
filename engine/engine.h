/* ElfDrumLXR engine wrapper: the native LXR-02 firmware engine behind a small C API.
   Adapted from mechanical-dance/PHX-D01 host/engine.h (GPL-3.0; see NOTICE), minus PHX-D01's
   LXR-1 engine hooks and its Eurorack-module mod matrix (out of scope here — see PROJECT_TRUTH.md).
   Not thread-safe: call from one thread. */
#pragma once
#include <stdint.h>
#define LXR_BLOCK 64   /* frames per render call (OUTPUT_DMA_SIZE) */
#define LXR_JACKS 4    /* output jacks: DAC1 L/R (main), DAC2 L/R */
/* The rate the harness renders at. The firmware's own REAL_FS is 44140 Hz; PHX-D01 labelled its
   renders 44100 and this harness does the same so renders compare 1:1 with the PHX-D01 reference.
   The plugin's rate contract (engine at native 44140 + resampling) is ARCHITECTURE.md, Phase 1. */
#define LXR_RATE 44100

/* A kit is the 247 LXR-2 (.SND) parameter bytes after the 8-byte name. */
#define LXR_KIT_PARAMS 247

const char *lxr_engine_name(void);
void lxr_init(void);
void lxr_load_params(const uint8_t params[LXR_KIT_PARAMS]);
/* The same kit after an edit; sends only the parameters that changed. Returns how many it sent. */
int lxr_edit_params(const uint8_t params[LXR_KIT_PARAMS]);
void lxr_hit(int voice, uint8_t velocity);                   /* trigger 0-6: voices drum1-3, snare, cymbal, hihat; 5 and 6 both play the hihat (closed/open decay), closed chokes open */
void lxr_note(int voice, uint8_t note, uint8_t velocity);    /* as lxr_hit, pitched: note 60 = the kit's own pitch, +-1 = a semitone */
void lxr_set_fader(int voice, float gain);                    /* voice 0-5 fader gain 0-1, after the fader curve; 1 = no fader */
void lxr_render(int16_t jack[LXR_JACKS][LXR_BLOCK]);          /* one mono buffer per output jack */
/* Phase 4 stems (patch 04): voice 0-5's stereo contribution to the last rendered
   block, interleaved L/R, LXR_BLOCK frames. Valid until the next lxr_render. */
const int16_t* lxr_stem_data(int voice);

/* --- Phase 1 additions: firmware globals + the external-MIDI parser entry --- */
/* Feed raw external-MIDI bytes into the firmware's own parser (midiParser_parseUartData):
   CC/CC2/NRPN, pitch bend, program change are handled by the firmware, exactly as on hardware.
   Note on/off is NOT routed through here by the plugin (see the accessors below). */
void lxr_midi_bytes(const uint8_t *data, int len);
/* Read a global (non-kit) parameter from the engine's activeParameters (PAR_* index, e.g.
   PAR_MIDI_CHAN_1, PAR_MIDI_NOTE1, PAR_MORPH). Returns 0 for out-of-range indices. */
uint8_t lxr_global_param(uint16_t idx);
/* The firmware's per-voice MIDI configuration (globals, set at init to the firmware's own
   factory defaults from preset_loadDefaultGlobals): note = activeParameters[PAR_MIDI_NOTE1+voice]
   (an inert placeholder upstream — its parameterArray hooks are commented out in the firmware),
   channel = activeParameters[PAR_MIDI_CHAN_1+voice] stored 1-based, returned 0-based. */
uint8_t lxr_midi_note(int voice);
uint8_t lxr_midi_channel(int voice);
/* PAR_MIDI_CHAN_GLOBAL as stored: 0 means "all channels" on the hardware. */
uint8_t lxr_global_channel(void);
/* The front panel's active track (what a global-channel note plays on hardware). */
void lxr_set_active_track(int voice);
int lxr_active_track(void);

/* --- Phase 3: sequencer bridge (engine/seqbridge.c) ---
   The firmware sequencer (Sequencer/sequencer.c) driven through its own public API.
   Time base: the firmware's systick counter runs at 4000 units/s (TIM14, ~0.25 ms/unit);
   seq_tick() advances one 96-ppq prescale tick per call when due (24 ticks per 16th step).
   All functions are audio-thread only, like the rest of the wrapper. */
#define LXR_SEQ_TRACKS 7
#define LXR_SEQ_PATTERNS 64
#define LXR_SEQ_STEPS 64
#define LXR_SEQ_SONG_LEN 64
#define LXR_SEQ_NO_AUTO 0xff   /* Step paramNr value for "no parameter lock" (NO_AUTOMATION) */
#define LXR_SONG_END 64        /* song entry: stop the sequencer */
#define LXR_SONG_LOOP 65       /* song entry: jump back to the song start */

typedef struct {
	uint8_t volume;   /* bits 0..6 = velocity (bit 7 is the firmware's vestigial active flag) */
	uint8_t prob;     /* 0..100 */
	uint8_t note;     /* 0..127 */
	uint8_t p1nr, p1val, p2nr, p2val;   /* parameter locks: nr = parameter index (CC space), 0xff = none */
	uint8_t flam;     /* 0..5 */
	int8_t shift;     /* -7..+7 (StepTiming raw 0..14 minus 7) */
} LxrStep;

uint32_t lxr_seq_systick(void);                  /* current systick_ticks */
void lxr_seq_pump(uint32_t target);              /* advance systick one unit at a time, seq_tick() per unit; never backwards */
void lxr_seq_set_running(int running);           /* seq_setRunning: start resets step indices to pattern start */
int lxr_seq_is_running(void);
void lxr_seq_set_bpm(int bpm);                   /* firmware BPM is integer (seq_setBpm takes uint16) */
int lxr_seq_get_bpm(void);
/* Position the running sequencer so the next step played is globalStep within the pattern
   (per-track modulo its length), with the prescale phase set to prescale (0..23 of 24).
   Implemented as a muted seq_nextStep() walk (seq_stepIndex is file-static in the firmware),
   so a pending pattern switch or song advance crossed by the walk still applies. */
void lxr_seq_seek(int globalStep, int prescale);
void lxr_seq_set_ext_sync(int mode);             /* 0 = internal, 1 = MIDI clock (firmware seq_setExtSync) */
int lxr_seq_get_ext_sync(void);

int lxr_seq_active_pattern(void);
int lxr_seq_pending_pattern(void);
/* Request a pattern change through the firmware's pending mechanism. Live edits to the
   pattern being left are committed first (seq_copyPatternToBackupBuffer), because the
   firmware's switch restores the backup buffer over the pattern being left. */
void lxr_seq_request_pattern(int pattern);
void lxr_seq_commit_pattern(int pattern);        /* make live edits to a pattern survive switches */
void lxr_seq_copy_pattern(int src, int dst);     /* firmware seq_copyPattern */
void lxr_seq_clear_pattern(int pattern);         /* firmware seq_clearPattern */
void lxr_seq_clear_track(int pattern, int track);

void lxr_seq_get_step(int pattern, int track, int step, LxrStep *out);
void lxr_seq_set_step(int pattern, int track, int step, const LxrStep *in);
int lxr_seq_main_step(int pattern, int track, int step);        /* seq_isMainStepActive */
void lxr_seq_set_main_step(int pattern, int track, int step, int on);   /* seq_setMainStep */
int lxr_seq_track_length(int pattern, int track);               /* normalised: stored 0 -> 16 */
void lxr_seq_set_track_length(int pattern, int track, int len); /* writes length[] directly:
        the firmware's seq_setTrackLength operates on the front panel's shown pattern, not
        the pattern being edited, so it cannot be used for arbitrary patterns */
int lxr_seq_mute(int track);                                    /* seq_isTrackMuted */
void lxr_seq_set_mute(int track, int muted);                    /* seq_setMute */
int lxr_seq_shuffle(int pattern);                               /* stored byte 0..127 */
void lxr_seq_set_shuffle(int pattern, int value);               /* byte + live-apply when active */
int lxr_seq_kit_nr(int pattern);
void lxr_seq_set_kit_nr(int pattern, int kitNr);
int lxr_seq_roll(int track);
void lxr_seq_set_roll(int track, int on);                       /* seq_setRoll */
void lxr_seq_set_roll_rate(int menuIdx);                        /* seq_setRollRate (menu index) */
int lxr_seq_roll_rate(void);                                    /* current divider (seq_rollRate) */
void lxr_seq_euklid(int track, int pattern, int length, int steps, int rotation);   /* firmware EuklidGenerator */

void lxr_seq_set_song_mode(int on);                             /* seq_setSongModeEnabled */
int lxr_seq_song_mode(void);
int lxr_seq_song_position(void);                                /* seq_songPositionPointer */
int lxr_seq_song_slot(void);                                    /* which seq_songData slot is active */
void lxr_seq_set_song_slot(int slot);                           /* seq_activeSong = seq_songData[slot] */
int lxr_seq_song_entry(int slot, int pos);
void lxr_seq_set_song_entry(int slot, int pos, int value);
int lxr_seq_step_index(int track);                              /* playhead: seq_getStepIndex */

/* Whole-set state (plugin state v4). Sizes are the firmware's own sizeof()s. */
size_t lxr_seq_patternset_size(void);            /* sizeof(PatternSet) = 233536 */
void lxr_seq_patternset_read(void *out);
void lxr_seq_patternset_write(const void *in);   /* + commits the active pattern's backup */
size_t lxr_seq_songdata_size(void);              /* 64*64 = 4096 */
void lxr_seq_songdata_read(void *out);
void lxr_seq_songdata_write(const void *in);

/* Decoded access into a snapshot blob produced by lxr_seq_patternset_read (+ song data
   appended by the caller): keeps the PatternSet layout knowledge in the wrapper. */
void lxr_seq_snap_get_step(const void *snap, int pattern, int track, int step, LxrStep *out);
int lxr_seq_snap_main_step(const void *snap, int pattern, int track, int step);
int lxr_seq_snap_track_length(const void *snap, int pattern, int track);
int lxr_seq_snap_shuffle(const void *snap, int pattern);
int lxr_seq_snap_kit_nr(const void *snap, int pattern);

/* .PAT / song file layer: presetManager.c's layouts ported from FatFs to stdio.
   .PAT (per pattern, 3663 bytes): 8-byte name + "PatV03" + per track Step[64] + timing[64]
   + 4 uint16 main-step masks per track + shuffle + 7 lengths + kitNr. The reader accepts
   the firmware's V1 (untagged) and V2 files with the firmware's own version gating.
   Song file: 8-byte name + 64 song bytes. Return 0 on success. */
int lxr_pat_save_pattern(const char *path, int pattern, const char name[8]);
int lxr_pat_load_pattern(const char *path, int pattern, char nameOut[8]);
int lxr_pat_save_song(const char *path, int slot, const char name[8]);
int lxr_pat_load_song(const char *path, int slot, char nameOut[8]);

/* Host side. A kit file: the 8-byte name, then the 247 params. A 309-byte module kit is accepted;
   its 54 mod-matrix bytes are ignored (desktop engine). */
int lxr_read_kit(const char *path, uint8_t params[LXR_KIT_PARAMS], char name[9]);   /* 0 on success */
/* The firmware's kit morph (preset_morph_tick): amount 0-255 from kit a to kit b. Routing and mod targets stay on a. */
void lxr_morph_kit(const uint8_t a[LXR_KIT_PARAMS], const uint8_t b[LXR_KIT_PARAMS], uint8_t amount, uint8_t out[LXR_KIT_PARAMS]);
