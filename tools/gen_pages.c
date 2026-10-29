/* gen_pages — generates plugin/src/ParamMetadata.generated.h from the LXR-02
 * firmware's own menu tables. Driven by tools/gen_pages.sh, which cuts
 * parameter_dtypes[] / valueNames[] out of menu.c and modTargets[] out of
 * Cc2Text.c into .inc files and extracts the ParamEnums symbol table from
 * Parameters.h. Everything else (menuPages, MenuText string tables) is
 * compiled in directly from the pinned vendor tree.
 *
 * Semantics pinned from the firmware sources (see ARCHITECTURE.md):
 *  - A parameter's page/section/slot order is its FIRST occurrence in
 *    menuPages, walking voice pages 1-6, the open-hat page, the FX page,
 *    then the performance page. Kit params on no page get the Global page's
 *    "System" section so all 247 are covered exactly once.
 *  - Control type and value range come from parameter_dtypes[] (menu.c):
 *    low byte = DTYPE_*, high byte = MENU_* id for DTYPE_MENU.
 *  - Enum value names come from the MenuText.h tables. The LCD lookup
 *    (menu.c getMenuItemNameForValue) indexes table[value+1]; for every
 *    menu except MENU_FILTER the stored value is 0-based into the entry
 *    list, matching how the DSP consumes it. MENU_FILTER is the exception:
 *    kit bytes are the DSP filterTypeEnum (FILTER_LP=1 .. FILTER_OFF=8,
 *    ResoFilter.h) and byte 0 is the INIT/bypass value, so the generated
 *    list has a leading "-" entry and values run 0..8.
 *  - PM63 display offsets follow menu.c's display switch: -60 for the
 *    COARSE / FM_FREQ / MOD_OSC_F params, -63 for the rest.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "menu.h"
#include "MenuText.h"
#include "menuPages.h"
#include "Parameters.h"
#include "dtypes.inc"
#include "names.inc"
#include "modtargets.inc"
#include "modoffsets.inc"
#include "param_symbols.inc"

#define KIT_COUNT END_OF_SOUND_PARAMETERS   /* 247 */
#define NO_VOICE 0xFF

/* Panel pages (editor tabs), in display order. */
enum { PG_DRUM1, PG_DRUM2, PG_DRUM3, PG_SNARE, PG_CYMBAL, PG_HIHAT, PG_FX, PG_GLOBAL };
static const char *pageNames[] = { "Drum 1", "Drum 2", "Drum 3", "Snare", "Cymbal", "HiHat", "FX", "Global" };
static const char *pageShort[] = { "D1", "D2", "D3", "Sn", "Cy", "HH", "FX", "G" };
/* Voice subpage titles in firmware order (menuPages voice pages). */
static const char *voiceSections[8] = { "Osc", "Amp EG", "Mod", "FM", "Transient", "Filter", "LFO", "Mix" };

/* LCD long names are 8-char truncations with typos; fix the known ones for
 * display use. The raw firmware strings stay in the vendor tree; this map is
 * presentation-only and deliberately small. */
static const char *fixName(const char *s)
{
	static const struct { const char *from, *to; } fixes[] = {
		{ "Frequncy", "Frequency" }, { "Resnance", "Resonance" },
		{ "Retriggr", "Retrigger" }, { "ClockSnc", "Clock Sync" },
		{ "PulsWdth", "Pulse Width" }, { "Overdriv", "Overdrive" },
		{ "SampleRt", "Sample Rate" }, { "DstParam", "Dest Param" },
		{ "DstVoice", "Dest Voice" }, { "Threshld", "Threshold" },
		{ "Prbablty", "Probability" }, { "Dcy Clsd", "Decay Closed" },
		{ "Dcy Open", "Decay Open" },
		{ "Vol mod", "Vol Mod" }, { "NextPatt", "Next Pattern" },
		{ "AudioOut", "Audio Out" }, { "RepeatCt", "Repeat Count" },
		{ "AutTrack", "Autom Track" }, { "Aut Dest", "Autom Dest" },
		{ "AutValue", "Autom Value" }, { "Screensv", "Screensaver" },
		{ "X Positn", "X Position" }, { "Y Positn", "Y Position" },
		{ "KitChMde", "Kit Change Mode" }, { "Freqcy 1", "Freq 1" },
		{ "Freqcy 2", "Freq 2" }, { "Oscilltr", "Oscillator" },
		{ "Transnt", "Transient" }, { "Veloc EG", "Velocity EG" },
		{ "PitchMod", "Pitch Mod" },
	};
	for (unsigned i = 0; i < sizeof fixes / sizeof fixes[0]; i++)
		if (strcmp(s, fixes[i].from) == 0) return fixes[i].to;
	return s;
}

/* Escape a string for a C string literal (names are printable ASCII in
 * practice, but the ratio table contains ':' and tables could change). */
static void esc(const char *s)
{
	for (; *s; s++) {
		if (*s == '"' || *s == '\\') putchar('\\');
		putchar(*s);
	}
}
static void qstr(const char *s) { putchar('"'); esc(s); putchar('"'); }

/* Copy a fixed-width firmware string (not NUL-terminated within width). */
static void clean(char *dst, const char *src, int width)
{
	int n = 0;
	while (n < width && src[n]) { dst[n] = src[n]; n++; }
	while (n > 0 && dst[n - 1] == ' ') n--;
	dst[n] = 0;
}

/* ---- assignment pass ---------------------------------------------------- */
typedef struct {
	int found;
	int page, voice, subpage, slot, order;
	uint8_t textId;
} Assign;
static Assign assign[NUM_PARAMS];
static int orderCounter = 0;

static void scanPage(int fwPage, int panelPage, int voice)
{
	for (int s = 0; s < NUM_SUB_PAGES; s++) {
		const Page *g = &menuPages[fwPage][s];
		const uint8_t tops[8] = { g->top1, g->top2, g->top3, g->top4, g->top5, g->top6, g->top7, g->top8 };
		const uint16_t bots[8] = { g->bot1, g->bot2, g->bot3, g->bot4, g->bot5, g->bot6, g->bot7, g->bot8 };
		for (int i = 0; i < 8; i++) {
			uint16_t par = bots[i];
			if (par == PAR_NONE || par >= NUM_PARAMS) continue;
			if (assign[par].found) continue;
			assign[par].found = 1;
			assign[par].page = panelPage;
			assign[par].voice = voice;
			assign[par].subpage = s;
			assign[par].slot = i;
			assign[par].order = orderCounter++;
			assign[par].textId = tops[i];
		}
	}
}

/* ---- enum name tables ---------------------------------------------------- */
/* A firmware name table: entry [0] holds the count as a char, entries follow. */
static void emitNameList(const char *arrName, const char (*table)[8], int width, int leadDash)
{
	int count = (uint8_t) table[0][0];
	printf("static const char *const %s[] = { ", arrName);
	if (leadDash) printf("\"-\", ");
	for (int i = 1; i <= count; i++) {
		char buf[16]; clean(buf, table[i], width);
		qstr(fixName(buf)); printf(", ");
	}
	printf("};\n");
}

/* ---- per-param derivation ------------------------------------------------ */
enum { CT_CONT, CT_ENUM, CT_TOGGLE, CT_MODTARGET };
typedef struct {
	int control, minV, maxV, dispOff, numValues;
	const char *valueList;   /* generated array name, or NULL */
} Derived;

static const char *menuListName(int menuId)
{
	switch (menuId) {
	case MENU_FILTER: return "kNamesFilter";
	case MENU_WAVEFORM: return "kNamesWaveform";
	case MENU_AUDIO_OUT: return "kNamesAudioOut";
	case MENU_SYNC_RATES: return "kNamesSyncRates";
	case MENU_LFO_WAVES: return "kNamesLfoWaves";
	case MENU_RETRIGGER: return "kNamesRetrigger";
	case MENU_TRANS: return "kNamesTransient";
	case MENU_MIX_FM: return "kNamesMixFm";
	case MENU_FX: return "kNamesFxType";
	case MENU_DIST: return "kNamesDistType";
	case MENU_RATIO: return "kNamesRatio";
	case MENU_DELAY: return "kNamesDelayType";
	case MENU_REVERB: return "kNamesReverbType";
	case MENU_AUDIO_OUT_FX: return "kNamesAudioOutFx";
	default: return NULL;
	}
}
static int menuCount(int menuId)
{
	switch (menuId) {
	case MENU_FILTER: return 9;               /* "-" + 8 DSP types */
	case MENU_WAVEFORM: return waveformNames[0][0];
	case MENU_AUDIO_OUT: return outputNames[0][0];
	case MENU_SYNC_RATES: return syncRateNames[0][0];
	case MENU_LFO_WAVES: return lfoWaveNames[0][0];
	case MENU_RETRIGGER: return retriggerNames[0][0];
	case MENU_TRANS: return transientNames[0][0];
	case MENU_MIX_FM: return mixFmNames[0][0];
	case MENU_FX: return fxNames[0][0];
	case MENU_DIST: return distNames[0][0];
	case MENU_RATIO: return ratioNames[0][0];
	case MENU_DELAY: return delayNames[0][0];
	case MENU_REVERB: return reverbNames[0][0];
	case MENU_AUDIO_OUT_FX: return outputNamesFx[0][0];
	default: return 0;
	}
}

static int isMinus60(int par)
{
	/* menu.c display switch: these PM63 params display value-60. */
	return (par >= PAR_COARSE1 && par <= PAR_COARSE6)
		|| (par >= PAR_FM_FREQ1 && par <= PAR_FM_FREQ3)
		|| par == PAR_MOD_OSC_F1_CYM || par == PAR_MOD_OSC_F2_CYM
		|| par == PAR_MOD_OSC_F1 || par == PAR_MOD_OSC_F2;
}

static Derived derive(int par)
{
	Derived d = { CT_CONT, 0, 127, 0, 0, NULL };
	uint16_t dt = parameter_dtypes[par];
	int dtype = dt & 0xFF, menuId = dt >> 8;
	switch (dtype) {
	case DTYPE_0B255: d.maxV = 255; break;
	case DTYPE_0B127: d.maxV = 127; break;
	case DTYPE_PM100: d.maxV = 100; break;
	case DTYPE_PM63: d.maxV = 127; d.dispOff = isMinus60(par) ? -60 : -63; break;
	case DTYPE_1B16: d.minV = 1; d.maxV = 16; break;
	case DTYPE_ON_OFF: d.control = CT_TOGGLE; d.maxV = 1; break;
	case DTYPE_0b1: d.control = CT_TOGGLE; d.maxV = 1; break;
	case DTYPE_1B64: d.minV = 1; d.maxV = 64; break;
	case DTYPE_0B15: d.maxV = 15; break;
	case DTYPE_0B100: d.maxV = 100; break;
	case DTYPE_NOTE_NAME: d.maxV = 127; break;
	case DTYPE_VOICE_LFO:
		d.control = CT_ENUM; d.minV = 1; d.maxV = 6;
		d.numValues = 6; d.valueList = "kNamesVoiceLfo"; break;
	case DTYPE_TARGET_SELECTION_LFO:
	case DTYPE_TARGET_SELECTION_VELO:
		d.control = CT_MODTARGET; d.maxV = (int)(sizeof modTargets / sizeof modTargets[0]) - 1;
		d.numValues = (int)(sizeof modTargets / sizeof modTargets[0]);
		d.valueList = "kModTargetNames"; break;
	case DTYPE_AUTOM_TARGET:
		d.control = CT_MODTARGET; d.maxV = (int)(sizeof modTargets / sizeof modTargets[0]) - 1;
		d.numValues = (int)(sizeof modTargets / sizeof modTargets[0]);
		d.valueList = "kModTargetNames"; break;
	case DTYPE_MENU:
		d.control = CT_ENUM;
		d.valueList = menuListName(menuId);
		d.numValues = menuCount(menuId);
		if (menuId == MENU_FILTER) { d.minV = 0; d.maxV = 8; }
		else { d.minV = 0; d.maxV = d.numValues - 1; }
		break;
	default: break;
	}
	return d;
}

static int flagRouting(int par)
{
	const char *s = paramSymbols[par];
	return strstr(s, "AUDIO_OUT") != NULL || strcmp(s, "PAR_FX_ROUTING") == 0;
}
static int flagLevel(int par)
{
	const char *s = paramSymbols[par];
	if (strncmp(s, "PAR_VOL", 7) == 0 && strlen(s) == 8 && s[7] >= '1' && s[7] <= '6') return 1;
	return strstr(s, "_VOL") != NULL && strncmp(s, "PAR_TRANS", 9) == 0;
}

/* Per-symbol display-name overrides where the firmware's shared long name
 * would collide inside one page/section (four FX params are all just
 * "Type"), or where a same-named pair needs its ordinal. */
static const char *symbolNameOverride(const char *sym)
{
	if (strcmp(sym, "PAR_FX_DIST_TYPE") == 0) return "Dist Type";
	if (strcmp(sym, "PAR_FX_DELAY_TYPE") == 0) return "Delay Type";
	if (strcmp(sym, "PAR_FX_REVERB_TYPE") == 0) return "Reverb Type";
	if (strcmp(sym, "PAR_FX_WAVE") == 0) return "Ring Wave";
	if (strncmp(sym, "PAR_VELO_MOD_AMT", 16) == 0) return "Velo Amount";
	if (strcmp(sym, "PAR_WAVE2_CYM") == 0 || strcmp(sym, "PAR_WAVE2_HH") == 0) return "Wave 2";
	if (strcmp(sym, "PAR_WAVE3_CYM") == 0 || strcmp(sym, "PAR_WAVE3_HH") == 0) return "Wave 3";
	if (strcmp(sym, "PAR_NONE") == 0) return "None";
	return NULL;
}

/* Name for a param that never appears on a menu page: derive from symbol. */
static void symbolName(char *dst, const char *sym)
{
	/* PAR_MORPH_KIT_DEST -> "Morph Kit Dest" */
	const char *p = sym + 4;   /* skip PAR_ */
	int n = 0, wordStart = 1;
	for (; *p && n < 30; p++) {
		if (*p == '_') { if (n && dst[n - 1] != ' ') dst[n++] = ' '; wordStart = 1; continue; }
		char c = *p;
		if (c >= 'A' && c <= 'Z') c = wordStart ? c : (char)(c - 'A' + 'a');
		dst[n++] = c; wordStart = 0;
	}
	dst[n] = 0;
}

int main(void)
{
	/* Assignment: voice pages in order, open-hat page (assigns only the
	 * params unique to it, e.g. PAR_VELOD6_OPEN, to the HiHat voice), FX
	 * page, performance page, then the global/MIDI page for completeness. */
	for (int v = 0; v < 6; v++) scanPage(v, v, v);
	scanPage(VOICE7_PAGE, PG_HIHAT, 5);
	scanPage(FX_MODE_PAGE, PG_FX, NO_VOICE);
	scanPage(PERF_PAGE, PG_GLOBAL, NO_VOICE);
	scanPage(MENU_MIDI_PAGE, PG_GLOBAL, NO_VOICE);
	/* Remaining pages hold only non-kit params; record them for globals. */
	for (int p = 0; p < NUM_PAGES; p++)
		if (p != FX_MODE_PAGE && p != PERF_PAGE && p != MENU_MIDI_PAGE && p > VOICE7_PAGE)
			scanPage(p, PG_GLOBAL, NO_VOICE);

	/* Kit params still unassigned -> Global / System. */
	for (int par = 0; par < KIT_COUNT; par++)
		if (!assign[par].found) {
			assign[par].found = 1;
			assign[par].page = PG_GLOBAL;
			assign[par].voice = NO_VOICE;
			assign[par].subpage = 7;   /* System section sorts last on the page */
			assign[par].slot = par % 8;
			assign[par].order = orderCounter++;
			assign[par].textId = 0xFF; /* sentinel: name from symbol */
		}

	/* ---------------- output ---------------- */
	printf("/* GENERATED by tools/gen_pages.sh from the LXR-02 firmware menu tables\n");
	printf(" * (vendor/lxr-02, pinned commit — see scripts/fetch-vendor.sh). Do not edit.\n");
	printf(" * Coverage: all %d kit parameters exactly once, plus the non-kit globals\n", KIT_COUNT);
	printf(" * the menu tables expose. Semantics: see tools/gen_pages.c. */\n");
	printf("#pragma once\n#include <cstdint>\n\nnamespace elfmeta {\n\n");
	printf("inline constexpr int kNumKitParams = %d;\n", KIT_COUNT);
	printf("inline constexpr int kNumModTargets = %d;\n\n", (int)(sizeof modTargets / sizeof modTargets[0]));
	printf("enum class Control : uint8_t { Continuous = 0, Enum = 1, Toggle = 2, ModTarget = 3 };\n");
	printf("enum Page : uint8_t { PageDrum1 = 0, PageDrum2, PageDrum3, PageSnare, PageCymbal, PageHiHat, PageFX, PageGlobal };\n");
	printf("inline constexpr uint8_t kNoVoice = 0xFF;\n");
	printf("inline constexpr uint8_t kFlagRouting = 1;   /* randomise keep-set */\n");
	printf("inline constexpr uint8_t kFlagModTarget = 2; /* mod-target selector or morph-destination selector: randomise keep-set */\n");
	printf("inline constexpr uint8_t kFlagLevel = 4;     /* randomise floor */\n\n");
	printf("struct KitParam {\n"
	       "    uint8_t index; const char* symbol; const char* name; const char* displayName;\n"
	       "    const char* section; uint8_t page; uint8_t voice;\n"
	       "    Control control; int16_t minValue; int16_t maxValue; int16_t displayOffset;\n"
	       "    const char* const* valueNames; uint8_t numValues; uint8_t flags;\n};\n\n");

	/* enum name lists */
	emitNameList("kNamesFilter", filterTypes, 3, 1);
	emitNameList("kNamesWaveform", waveformNames, 3, 0);
	emitNameList("kNamesAudioOut", outputNames, 6, 0);
	emitNameList("kNamesSyncRates", syncRateNames, 3, 0);
	emitNameList("kNamesLfoWaves", lfoWaveNames, 3, 0);
	emitNameList("kNamesRetrigger", retriggerNames, 3, 0);
	emitNameList("kNamesTransient", transientNames, 3, 0);
	emitNameList("kNamesMixFm", mixFmNames, 5, 0);
	emitNameList("kNamesFxType", fxNames, 3, 0);
	emitNameList("kNamesDistType", distNames, 3, 0);
	emitNameList("kNamesRatio", ratioNames, 3, 0);
	emitNameList("kNamesDelayType", delayNames, 3, 0);
	emitNameList("kNamesReverbType", reverbNames, 3, 0);
	emitNameList("kNamesAudioOutFx", outputNamesFx, 6, 0);
	printf("static const char *const kNamesVoiceLfo[] = { \"1\", \"2\", \"3\", \"4\", \"5\", \"6\" };\n\n");

	/* mod target names: "<owner prefix> <long name>", index 0 = Off */
	printf("static const char *const kModTargetNames[] = {\n");
	int numTargets = (int)(sizeof modTargets / sizeof modTargets[0]);
	for (int i = 0; i < numTargets; i++) {
		if (i == 0) { printf("\t\"Off\",\n"); continue; }
		uint16_t tp = modTargets[i].param;
		char ln[16]; clean(ln, longNames[valueNames[modTargets[i].nameIdx].longName], 16);
		const char *owner = "All";
		if (tp < NUM_PARAMS && assign[tp].found && tp < KIT_COUNT && assign[tp].voice != NO_VOICE)
			owner = pageShort[assign[tp].voice];
		else if (tp == PAR_VOICE_DECIMATION_ALL)
			owner = "All";
		printf("\t\"%s ", owner); esc(fixName(ln)); printf("\",\n");
	}
	printf("};\n\n");

	/* the kit table, sorted by index */
	printf("inline const KitParam kKitParams[%d] = {\n", KIT_COUNT);
	for (int par = 0; par < KIT_COUNT; par++) {
		Assign *a = &assign[par];
		Derived d = derive(par);
		char name[40], disp[64], symName[40];
		const char *section;
		const char *ovr = symbolNameOverride(paramSymbols[par]);
		if (ovr != NULL) {
			snprintf(name, sizeof name, "%s", ovr);
		} else if (a->textId != 0xFF && a->textId != TEXT_EMPTY) {
			char ln[16]; clean(ln, longNames[valueNames[a->textId].longName], 16);
			snprintf(name, sizeof name, "%s", fixName(ln));
		} else {
			symbolName(symName, paramSymbols[par]);
			snprintf(name, sizeof name, "%s", symName);
		}
		if (a->page <= PG_HIHAT) section = voiceSections[a->subpage];
		else if (a->page == PG_FX) section = "FX Bus";
		else section = (a->subpage == 7) ? "System" : "Performance";
		if (a->page <= PG_HIHAT)
			snprintf(disp, sizeof disp, "%s %s %s", pageNames[a->page], section, name);
		else if (a->page == PG_FX)
			snprintf(disp, sizeof disp, "FX %s", name);
		else
			snprintf(disp, sizeof disp, "Global %s", name);
		int flags = (flagRouting(par) ? 1 : 0)
			| ((d.control == CT_MODTARGET || par == PAR_MORPH_KIT_DEST) ? 2 : 0)
			| (flagLevel(par) ? 4 : 0);
		printf("\t{ %d, ", par); qstr(paramSymbols[par]); printf(", ");
		qstr(name); printf(", "); qstr(disp); printf(", "); qstr(section);
		printf(", %d, %d, ", a->page, a->voice);
		printf("Control::%s, %d, %d, %d, ",
			d.control == CT_ENUM ? "Enum" : d.control == CT_TOGGLE ? "Toggle" : d.control == CT_MODTARGET ? "ModTarget" : "Continuous",
			d.minV, d.maxV, d.dispOff);
		if (d.valueList) printf("%s, %d, ", d.valueList, d.numValues);
		else printf("nullptr, 0, ");
		printf("%d },\n", flags);
	}
	printf("};\n\n");

	/* panel order: indices sorted by (page, firmware order) */
	printf("inline const uint8_t kPanelOrder[%d] = {", KIT_COUNT);
	{
		int idx[KIT_COUNT]; for (int i = 0; i < KIT_COUNT; i++) idx[i] = i;
		for (int i = 0; i < KIT_COUNT; i++)
			for (int j = i + 1; j < KIT_COUNT; j++) {
				int A = idx[i], B = idx[j];
				if (assign[B].page < assign[A].page
					|| (assign[B].page == assign[A].page && assign[B].order < assign[A].order)) {
					idx[i] = B; idx[j] = A;
				}
			}
		for (int i = 0; i < KIT_COUNT; i++) printf("%s%d", i ? ", " : "", idx[i]);
	}
	printf(" };\n\n");

	/* non-kit globals the menu tables expose (sequencer/MIDI/pattern params;
	 * informational in Phase 2 — they are not kit bytes and the panel does
	 * not bind them yet). */
	printf("struct GlobalParam { uint16_t index; const char* symbol; uint8_t page; uint16_t dtype; };\n");
	printf("inline const GlobalParam kGlobalParams[] = {\n");
	for (int par = KIT_COUNT; par < NUM_PARAMS; par++)
		if (assign[par].found)
			printf("\t{ %d, \"%s\", %d, %d },\n", par, paramSymbols[par], assign[par].page, parameter_dtypes[par]);
	printf("};\n");
	printf("inline constexpr int kNumGlobalParams = (int)(sizeof (kGlobalParams) / sizeof (kGlobalParams[0]));\n");
	printf("\n} // namespace elfmeta\n");
	return 0;
}
