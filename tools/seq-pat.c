/* seq-pat: Phase 3 gate (a) — .PAT round-trip.
   1. Fill the whole pattern set + song data deterministically through the wrapper.
   2. Export all 64 patterns (+ 4 songs) through the ported writer.
   3. INDEPENDENT CHECK: re-parse every .PAT with a hand-rolled parser written from the
      documented layout (literal sizes: Step = 7 bytes, timing = 1 byte flam|shift<<4,
      masks = uint16 LE) and compare every field against the engine snapshot decoded
      through the wrapper's snapshot accessors.
   4. Wipe (re-init), re-import through the ported reader, memcmp the full sets.
   5. Hand-crafted V1 (untagged) and V2 files load with the firmware's version gating.
   Usage: seq-pat [tmpdir]   (default /tmp/seq-pat) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "engine.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static uint32_t lcg = 0x12345678;
static uint32_t rnd(void) { lcg = lcg * 1664525u + 1013904223u; return lcg >> 8; }

static char dir[512];
static void path_of(char *out, const char *name) { snprintf(out, 1024, "%s/%s", dir, name); }

/* --- independent parser, from the documented layout only --- */
struct PatFile {
	char name[8];
	uint8_t steps[7][64][7];    /* [trk][step][volume,prob,note,p1n,p1v,p2n,p2v] */
	uint8_t timing[7][64];      /* flam | shift<<4 */
	uint16_t masks[7][4];       /* [trk][bar] */
	uint8_t shuffle, lengths[7], kitNr;
};
static int parse_pat(const char *path, struct PatFile *out, long *sizeOut)
{
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	if (sizeOut) *sizeOut = sz;
	uint8_t buf[8192];
	if (sz > (long)sizeof buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return -1; }
	fclose(f);
	const uint8_t *p = buf;
	memcpy(out->name, p, 8); p += 8;
	if (memcmp(p, "PatV03", 6) != 0) return -2;
	p += 6;
	for (int t = 0; t < 7; t++) { memcpy(out->steps[t], p, 64 * 7); p += 64 * 7; memcpy(out->timing[t], p, 64); p += 64; }
	for (int t = 0; t < 7; t++)
		for (int b = 0; b < 4; b++) { out->masks[t][b] = (uint16_t)(p[0] | (p[1] << 8)); p += 2; }
	out->shuffle = *p++;
	memcpy(out->lengths, p, 7); p += 7;
	out->kitNr = *p++;
	return (int)(p - buf);
}

int main(int argc, char **argv)
{
	snprintf(dir, sizeof dir, "%s", argc > 1 ? argv[1] : "/tmp/seq-pat");
	mkdir(dir, 0777);
	char path[1024];

	lxr_init();

	/* 1. deterministic fill */
	char names[64][8];
	for (int pat = 0; pat < 64; pat++) {
		memset(names[pat], ' ', 8);
		names[pat][0] = 'P';
		names[pat][1] = (char)('0' + pat / 10);
		names[pat][2] = (char)('0' + pat % 10);
		for (int trk = 0; trk < 7; trk++) {
			lxr_seq_set_track_length(pat, trk, 1 + (int)(rnd() % 64));
			for (int st = 0; st < 64; st++) {
				LxrStep s;
				s.volume = (uint8_t)(rnd() % 128);
				s.prob = (uint8_t)(rnd() % 101);
				s.note = (uint8_t)(rnd() % 128);
				s.p1nr = (rnd() % 4 == 0) ? 0xff : (uint8_t)(rnd() % 247);
				s.p1val = (uint8_t)(rnd() % 128);
				s.p2nr = (rnd() % 4 == 0) ? 0xff : (uint8_t)(rnd() % 247);
				s.p2val = (uint8_t)(rnd() % 128);
				s.flam = (uint8_t)(rnd() % 6);
				s.shift = (int8_t)((int)(rnd() % 15) - 7);
				lxr_seq_set_step(pat, trk, st, &s);
				lxr_seq_set_main_step(pat, trk, st, (int)(rnd() & 1));
			}
		}
		lxr_seq_set_shuffle(pat, (int)(rnd() % 128));
		lxr_seq_set_kit_nr(pat, (int)(rnd() % 64));
	}
	for (int slot = 0; slot < 4; slot++)
		for (int pos = 0; pos < 64; pos++)
			lxr_seq_set_song_entry(slot, pos, (int)(rnd() % 66));

	const size_t psSize = lxr_seq_patternset_size(), sgSize = lxr_seq_songdata_size();
	uint8_t *snapA = malloc(psSize), *songA = malloc(sgSize);
	lxr_seq_patternset_read(snapA);
	lxr_seq_songdata_read(songA);
	CHECK(psSize == 233536, "patternset size %zu", psSize);
	CHECK(sgSize == 4096, "songdata size %zu", sgSize);

	/* 2. export */
	for (int pat = 0; pat < 64; pat++) {
		char fn[32]; snprintf(fn, sizeof fn, "pat%02d.pat", pat);
		path_of(path, fn);
		CHECK(lxr_pat_save_pattern(path, pat, names[pat]) == 0, "save pattern %d", pat);
	}
	char songNames[4][8];
	for (int slot = 0; slot < 4; slot++) {
		memset(songNames[slot], ' ', 8);
		songNames[slot][0] = 'S';
		songNames[slot][1] = (char)('0' + slot);
		char fn[32]; snprintf(fn, sizeof fn, "song%d.sng", slot);
		path_of(path, fn);
		CHECK(lxr_pat_save_song(path, slot, songNames[slot]) == 0, "save song %d", slot);
	}

	/* 3. independent parse + field compare */
	for (int pat = 0; pat < 64; pat++) {
		char fn[32]; snprintf(fn, sizeof fn, "pat%02d.pat", pat);
		path_of(path, fn);
		struct PatFile pf; long sz = 0;
		const int consumed = parse_pat(path, &pf, &sz);
		CHECK(sz == 3663, "pattern %d file size %ld != 3663", pat, sz);
		CHECK(consumed == 3663, "pattern %d consumed %d", pat, consumed);
		CHECK(memcmp(pf.name, names[pat], 8) == 0, "pattern %d name", pat);
		for (int trk = 0; trk < 7; trk++) {
			CHECK(lxr_seq_snap_track_length(snapA, pat, trk) == (pf.lengths[trk] == 0 ? 16 : pf.lengths[trk]),
			      "pat %d trk %d length raw %d", pat, trk, pf.lengths[trk]);
			for (int st = 0; st < 64; st++) {
				LxrStep s;
				lxr_seq_snap_get_step(snapA, pat, trk, st, &s);
				const uint8_t *raw = pf.steps[trk][st];
				CHECK((raw[0] & 0x7f) == s.volume && raw[1] == s.prob && raw[2] == s.note
				      && raw[3] == s.p1nr && raw[4] == s.p1val && raw[5] == s.p2nr && raw[6] == s.p2val,
				      "pat %d trk %d step %d fields", pat, trk, st);
				CHECK((pf.timing[trk][st] & 0x0f) == s.flam && (pf.timing[trk][st] >> 4) == (uint8_t)(s.shift + 7),
				      "pat %d trk %d step %d timing", pat, trk, st);
				CHECK(lxr_seq_snap_main_step(snapA, pat, trk, st) == (int)((pf.masks[trk][st / 16] >> (st % 16)) & 1),
				      "pat %d trk %d step %d mask", pat, trk, st);
			}
		}
		CHECK(lxr_seq_snap_shuffle(snapA, pat) == pf.shuffle, "pat %d shuffle", pat);
		CHECK(lxr_seq_snap_kit_nr(snapA, pat) == pf.kitNr, "pat %d kitNr", pat);
		if (failures > 20) { printf("too many failures, aborting\n"); return 1; }
	}

	/* 4. wipe + import + full memcmp */
	lxr_init();
	uint8_t *snapW = malloc(psSize);
	lxr_seq_patternset_read(snapW);
	CHECK(memcmp(snapA, snapW, psSize) != 0, "wipe did not change the pattern set");
	free(snapW);
	for (int pat = 0; pat < 64; pat++) {
		char fn[32]; snprintf(fn, sizeof fn, "pat%02d.pat", pat);
		path_of(path, fn);
		char nm[8];
		CHECK(lxr_pat_load_pattern(path, pat, nm) == 0, "load pattern %d", pat);
		CHECK(memcmp(nm, names[pat], 8) == 0, "pattern %d name round-trip", pat);
	}
	for (int slot = 0; slot < 4; slot++) {
		char fn[32]; snprintf(fn, sizeof fn, "song%d.sng", slot);
		path_of(path, fn);
		char nm[8];
		CHECK(lxr_pat_load_song(path, slot, nm) == 0, "load song %d", slot);
	}
	uint8_t *snapB = malloc(psSize), *songB = malloc(sgSize);
	lxr_seq_patternset_read(snapB);
	lxr_seq_songdata_read(songB);
	CHECK(memcmp(snapA, snapB, psSize) == 0, "pattern set round-trip memcmp");
	CHECK(memcmp(songA, songB, sgSize) == 0, "song data round-trip memcmp");

	/* 5. V1 / V2 files with the firmware's version gating.
	   Build them from pattern 5's parsed data: V2 = tag PatV02, no timing block, kitNr kept;
	   V1 = no tag, no timing block, no kitNr. Load into patterns 6 (V2) and 7 (V1) whose
	   timing/kitNr were pre-filled with sentinels the reader must NOT overwrite. */
	{
		struct PatFile pf; long sz;
		path_of(path, "pat05.pat");
		CHECK(parse_pat(path, &pf, &sz) == 3663, "re-parse pat05");
		LxrStep sentinel; memset(&sentinel, 0, sizeof sentinel);
		sentinel.flam = 3; sentinel.shift = -3; sentinel.note = 60; sentinel.prob = 100; sentinel.volume = 99;
		sentinel.p1nr = sentinel.p2nr = 0xff;
		for (int trk = 0; trk < 7; trk++)
			for (int st = 0; st < 64; st++)
				lxr_seq_set_step(6, trk, st, &sentinel), lxr_seq_set_step(7, trk, st, &sentinel);
		lxr_seq_set_kit_nr(6, 41); lxr_seq_set_kit_nr(7, 42);

		for (int ver = 1; ver <= 2; ver++) {
			char fn[32]; snprintf(fn, sizeof fn, "legacy%d.pat", ver);
			path_of(path, fn);
			FILE *f = fopen(path, "wb");
			fwrite(pf.name, 1, 8, f);
			if (ver == 2) fwrite("PatV02", 1, 6, f);
			for (int t = 0; t < 7; t++) fwrite(pf.steps[t], 1, 64 * 7, f);
			for (int t = 0; t < 7; t++)
				for (int b = 0; b < 4; b++) { uint16_t m = pf.masks[t][b]; fwrite(&m, 1, 2, f); }
			fwrite(&pf.shuffle, 1, 1, f);
			fwrite(pf.lengths, 1, 7, f);
			if (ver == 2) fwrite(&pf.kitNr, 1, 1, f);
			fclose(f);
			const int dst = ver == 2 ? 6 : 7;
			char nm[8];
			CHECK(lxr_pat_load_pattern(path, dst, nm) == 0, "load legacy V%d", ver);
			LxrStep s;
			lxr_seq_get_step(dst, 0, 0, &s);
			CHECK(s.note == pf.steps[0][0][2] && s.prob == pf.steps[0][0][1], "V%d step fields", ver);
			CHECK(s.flam == 3 && s.shift == -3, "V%d timing must be untouched (flam %d shift %d)", ver, s.flam, s.shift);
			CHECK(lxr_seq_kit_nr(dst) == (ver == 2 ? pf.kitNr : 42), "V%d kitNr gating (got %d)", ver, lxr_seq_kit_nr(dst));
			CHECK(lxr_seq_main_step(dst, 3, 17) == (int)((pf.masks[3][1] >> 1) & 1), "V%d mask", ver);
		}
	}

	printf(failures ? "seq-pat: %d FAILURES\n" : "seq-pat: ALL PASS (64 patterns + songs round-trip, V1/V2 gating)\n", failures);
	return failures ? 1 : 0;
}
