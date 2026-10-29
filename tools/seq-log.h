/* seq-log: trigger-log capture for the Phase 3 gate (b), shared by the reference
   harness (tools/seq-ref.c) and the plugin harness (plugin/render/PluginSeqTest.cpp).
   Both binaries link the engine with
     -Wl,--wrap=voiceControl_noteOn,--wrap=autoNode_setDestination,--wrap=autoNode_updateValue
   so every sequencer trigger funnels through here. Attribution: seq_triggerVoice()
   calls seq_parseAutomationNodes() (setDestination x2, updateValue x2, in order)
   immediately before voiceControl_noteOn() for the same track, single-threaded —
   so the last four automation calls belong to the next noteOn. The real functions
   are always called through, so engine behaviour is unchanged. */
#pragma once
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

extern volatile uint32_t systick_ticks;   /* engine/stubs.c */

void __real_voiceControl_noteOn(uint8_t voice, uint8_t note, uint8_t vel);
void __real_autoNode_setDestination(void *node, uint16_t dest);
void __real_autoNode_updateValue(void *node, uint8_t val);

static int seqlog_pendingDest[2] = { 255, 255 };
static int seqlog_pendingVal[2] = { 0, 0 };
static int seqlog_destCount = 0, seqlog_valCount = 0;
static int seqlog_noteOns = 0;
static FILE *seqlog_out = NULL;
/* Recent noteOn times/voices for in-process assertions (shuffle, probability). */
static uint32_t seqlog_times[8192];
static uint8_t seqlog_voices[8192];
static int seqlog_timeCount = 0;

static inline void seqlog_reset(void)
{
	seqlog_pendingDest[0] = seqlog_pendingDest[1] = 255;
	seqlog_pendingVal[0] = seqlog_pendingVal[1] = 0;
	seqlog_destCount = seqlog_valCount = 0;
	seqlog_noteOns = 0;
	seqlog_timeCount = 0;
}
static inline int seqlog_count(void) { return seqlog_noteOns; }
static inline void seqlog_set_out(FILE *f) { seqlog_out = f; }
static inline int seqlog_time_count(void) { return seqlog_timeCount; }
static inline uint32_t seqlog_time(int i) { return seqlog_times[i]; }
static inline int seqlog_voice(int i) { return seqlog_voices[i]; }

void __wrap_autoNode_setDestination(void *node, uint16_t dest)
{
	if (seqlog_destCount < 2) seqlog_pendingDest[seqlog_destCount] = dest;
	seqlog_destCount++;
	__real_autoNode_setDestination(node, dest);
}

void __wrap_autoNode_updateValue(void *node, uint8_t val)
{
	if (seqlog_valCount < 2) seqlog_pendingVal[seqlog_valCount] = val;
	seqlog_valCount++;
	__real_autoNode_updateValue(node, val);
}

void __wrap_voiceControl_noteOn(uint8_t voice, uint8_t note, uint8_t vel)
{
	FILE *out = seqlog_out ? seqlog_out : stdout;
	fprintf(out, "T %u V %u N %u VEL %u P1 %d %d P2 %d %d\n",
	        (unsigned)systick_ticks, voice, note, vel,
	        seqlog_pendingDest[0], seqlog_pendingVal[0],
	        seqlog_pendingDest[1], seqlog_pendingVal[1]);
	seqlog_noteOns++;
	if (seqlog_timeCount < 8192) {
		seqlog_times[seqlog_timeCount] = systick_ticks;
		seqlog_voices[seqlog_timeCount] = voice;
		seqlog_timeCount++;
	}
	seqlog_pendingDest[0] = seqlog_pendingDest[1] = 255;
	seqlog_pendingVal[0] = seqlog_pendingVal[1] = 0;
	seqlog_destCount = seqlog_valCount = 0;
	__real_voiceControl_noteOn(voice, note, vel);
}

#ifdef __cplusplus
}
#endif
