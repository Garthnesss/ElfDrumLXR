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

/* Host side. A kit file: the 8-byte name, then the 247 params. A 309-byte module kit is accepted;
   its 54 mod-matrix bytes are ignored (desktop engine). */
int lxr_read_kit(const char *path, uint8_t params[LXR_KIT_PARAMS], char name[9]);   /* 0 on success */
/* The firmware's kit morph (preset_morph_tick): amount 0-255 from kit a to kit b. Routing and mod targets stay on a. */
void lxr_morph_kit(const uint8_t a[LXR_KIT_PARAMS], const uint8_t b[LXR_KIT_PARAMS], uint8_t amount, uint8_t out[LXR_KIT_PARAMS]);
