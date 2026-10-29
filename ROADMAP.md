# LXR Plugin — Roadmap

Phases are gated on verification, not dates. A phase is done when STATUS.md
can state its claim with evidence behind it.

## Phase 0 — "Engine harness & golden truth"
- Vendor-pin the firmware; build copy + patch pipeline (`patches/01` oscillator overflow, from PHX-D01).
- Shim layer + engine wrapper (kit load via CC replay, index-127 skip, NULL-pointer scratch fix).
- `lxr-render` CLI: load a `.SND`, hit all 6 voices (+ open hat), render WAV; render INIT and a set of hand-built test kits covering every FX type (distortion, ringmod, compressor, delay) and every filter type.
- Intrinsics unit tests: portable `__QADD16/__QSUB16/__SMLALD/__SSAT/__PKHBT` against ARM-semantics reference vectors.
- Decide `__fp16` mapping (real `_Float16` vs float) by render comparison; record CPU per block for one engine instance.
- Gate: all voices + FX render non-silent, spectrally sane output (kick fundamental < 100 Hz, hat centroid > 10 kHz on INIT); our renders match PHX-D01 phx-render output on the same kits within tolerance on deterministic voices (peak ±1 LSB class agreement on pitched voices; statistical match on noise voices); intrinsics vectors pass 100%.

## Phase 1 — "A plugin that sounds"
- JUCE shell: VST3 + AU + Standalone, CMake, macOS universal (arm64 + x86_64).
- MIDI in through the firmware's own parser path (notes per voice map, CC/CC2/NRPN parameter control, morph CC), APVTS mirroring the 247 kit parameters for DAW automation.
- Fixed-rate engine (44,140 Hz, 64-frame blocks) + FIFO + r8brain resampling to host rate; two stereo buses (DAC1/DAC2) with hardware routing semantics.
- `.SND` import/export (drag & drop + file dialog); DAW state save/restore of kit bytes.
- Gate: builds on Garth's Mac; loads in Ableton Live and passes a play test (MIDI clip triggers all voices, by ear — Garth); a kit exported from the plugin re-imports byte-identical (automated round-trip test), and the INIT kit rendered inside the plugin null-matches the Phase 0 CLI render at 44.1 kHz host rate (resampler bypassed identity check at matched rates within resampler tolerance).
- **Status: COMPLETE** (Oct 28–29, 2026) — the gate's byte-identical kit round-trip clause was the last open item; closed Oct 29 by the automated save/reload harness (`plugin_morph_test save` mode: parameters 8..255 identical, name preserved). Garth's Live load + trigger test passed; the morph re-test after the Oct 29 fix is his remaining by-ear check (see STATUS.md).

## Phase 2 — "The panel"
- Generated UI metadata from the firmware menu tables (labels, value lists, display types) committed via `tools/gen_pages`. ✅
- Editor laid out on the hardware's own menu structure: 6 voice pages (Osc/EG/Mod/FM/Transient/Filter/LFO/Mix), FX page, global page — knobs and value fields, no photo-skeuomorphism. ✅
- Kit browser: folders/cards of `.SND` as banks (slot order from filenames), prev/next, INIT, randomise-per-voice (a gap PHX and the hardware both leave open — small, high-value). ✅ (randomise shipped guarded: routing/mod-target keep-set + 30% level floor)
- Morph slider wired to the firmware morph — **done early**: shipped in Phase 1; the Oct 29, 2026 fix added explicit morph-target loading (target name shown, "Load target .SND", drag-drop). Per-voice faders (the hardware's 7 faders → 6 voices + master). ✅ (Phase 2)
- Pulled forward from Phase 5: **per-voice presets** (Copy/Paste/Save/Load Voice, `.lxv` slice format) — the Kit Blender groundwork. ✅
- Timeboxed per-instance engine investigation: state surface fully enumerated (157 symbols / 317,047 B, no heap); swap deferred by design (see ARCHITECTURE) — singleton kept.
- Gate: every one of the 247 parameters is visible/editable in the UI (automated UI↔parameter-map cross-check against generated metadata ✅ — `tools/check_metadata.py` + `plugin_panel_test`), automatable ✅, and a Live project saved and reloaded restores the exact kit (Garth-verified in Live — **the one remaining clause**).
- **Status: IMPLEMENTED + VM-gated Oct 29, 2026; Mac rebuilt + reinstalled.** Remaining: Garth's Live save/reload + by-eye gate (see STATUS.md).

## Phase 3 — "Sequencer parity"
- Discovery first (one pass, written up in STATUS): exact main-step/substep editing semantics in the desktop firmware, song storage format, chaining rules — from `sequencer.c` + `presetManager.c`, not from the manual.
- Host-transport drive: DAW position/tempo → 96 ppq tick accumulator → `seq_tick()`; standalone falls back to internal clock; external MIDI clock input honoured via the firmware's sync path.
- Pattern editor on the firmware's data model: main steps + substep detail, per-step velocity/note/probability, **two parameter locks**, flam + shift timing, per-track length, track mute, rolls, shuffle, Euclidean generator, 64-pattern set, chaining + song mode.
- `.PAT` import/export through the ported preset-manager layouts.
- Gate: (1) a pattern authored in the plugin exports a `.PAT` that the ported firmware parser reads back into an identical pattern set (automated); (2) a fixed pattern + kit render an identical trigger-event log (voice, time, velocity, note, p-lock changes) in the plugin and in a reference harness driving `seq_tick()` directly; (3) checklist diff vs PHX-D01's sequencer (32 steps, no probability/p-locks/substeps) shows every gap closed; (4) Garth plays it in Live against host transport, by ear.
- **Status: IMPLEMENTED + VM-gated Oct 29, 2026; Mac rebuilt + reinstalled.** Gates (1)–(3) PASS with evidence in STATUS (trigger logs byte-identical, 59 events; PHX checklist all-pass; .PAT round-trip byte-exact incl. V1/V2 gating) via `tools/seq-gate.sh`. Remaining: gate (4), Garth's by-ear Live check.

## Phase 4 — "Reach & polish" — DONE Oct 29, 2026 (with noted exceptions)
- [x] **Reverb experiment — SHIPPED.** `USE_REVERB 1` (patch 03) as a sixth FX type; the DSP was complete but disabled at every layer upstream (likely STM32 CPU headroom — inference, labelled as such in STATUS). Gate green: decaying tail, 60 s stability at nominal + max feedback, every other render bit-exact; provably inert for existing kits (the value never occurs in them). Bonus fix: the panel's enum name tables were stride-corrupt since Phase 2 (generator bug) — fixed, FX type is now 0..5 incl. "Rev".
- [x] **Per-voice stem outputs — SHIPPED.** Mixer tap (patch 04) → 6 extra stereo buses (default layout unchanged: 2 mains). Gate green: patch proven inert at engine level (bit-exact A/B), isolation exact (one voice → one stem), stem == mains where only one voice sounds.
- [x] **CLAP — SHIPPED as a build target.** clap-juce-extensions (pinned), same processor, ID `com.garth.elfdrumlxr`, features instrument+drum-machine. Gate green beyond the minimum: builds, exports `clap_entry`, and an independent JUCE-free host renders the INIT kit matching the VST3 path within 1 LSB on pitched voices. In-DAW (Bitwig/Reaper) check is Garth's.
- [x] **Per-instance engines — SHIPPED (Linux/ELF).** Runtime ELF symtab census swap (161 symbols / 319,251 B), fail-safe to the singleton everywhere else (incl. stripped binaries). Gate green: partner-invariance, bit-identical renders incl. noise. Side-fix: the RNG shim is now a deterministic xorshift over a census global (was `arc4random()`). Caveat: pluginval L10 thread-safety crashes only in swap mode (see STATUS) — undiagnosed, beyond the release gate.
- [~] **Windows route — workflow LIVE, runs FAILING, logs unreachable.** `.github/workflows/build.yml` pushed (the token accepted workflow files); run 40854309569 failed in all three jobs (Linux at the engine gates — not reproducible from a clean local clone; macOS/Windows at the plugin build — the Windows one is at least partly the then-unconditional `clap_smoke_test`, since guarded to Linux). Job logs are not retrievable through the available API route, so per-job causes beyond that are unproven. Nothing about Windows is proven yet; a green run + a Windows-machine test remain outstanding.
- [~] **Packaging prep — DONE locally, release decision parked.** pluginval strictness 5: **SUCCESS** (Linux VST3, under xvfb). `dist/` holds the Linux zip + SHA256 (local only); `tools/make-macos-pkg.sh` builds the unsigned macOS .pkg on the Mac. Signed/notarised macOS + Windows zip await Garth's Developer ID, the release-model decision, and a working Mac build of the Phase 4 tree.
- [ ] Phase 4 gate items still open: clean install on a fresh user account (Mac + Windows); pluginval on macOS/Windows; Mac build of the Phase 4 state (Tailscale tunnel down all session — item carried over from Phase 3).

## Phase 5 — "Bonus" (post-parity differentiators, pick per release)
Sequenced by leverage for this engine, not by research order. [H] = host-side, no DSP change. [P] = engine patch, flagged, needs its own gate.

**PHASE 5 LANDED Oct 29, 2026** — every [H] headline shipped with a headless gate (`plugin_phase5_test`, 11 modes, all PASS) and the full prior suite re-run green (see STATUS "Phase 5"). Per item:

### Kit ecosystem
- **Kit Blender + per-voice preset library [H]** — ✅ SHIPPED (Blender tab: browse `.lxv` by voice type, audition/apply into the current kit, save current voice, extract a voice from any `.SND`). Gate: applied slice == manual byte splice; blended snare centroid 1417 Hz vs source 1403 Hz; untouched drum 1 peak bit-exact. Save/load single voices; assemble kits from voices across kits. The #1 verbatim LXR-02 owner request (Elektronauts); trivial here because a voice is a fixed slice of the 247 kit bytes. Groundwork shipped with Phase 2 (per-voice Copy/Paste/Save/Load + the `.lxv` format); Phase 5 added the library/blender UI layer on top of it.
- **SD-card project librarian + shareable kit packs [H]** — ✅ SHIPPED (Library tab: scan a card-shaped tree — PROJnn folders + loose files — import kits/patterns/songs through the plugin's own load paths, export/import zip packs with a `manifest.json` carrying format + author + file list). Gate: synthetic 3-project card round-trips; imported kit/pattern/song bytes identical; every packed file byte-identical after import. (Card ⇄ plugin "sync" is import/export by folder pick — no background watcher.)
- **In-context audition browser [H]** — ✅ SHIPPED IN REDUCED FORM: the Library tab *is* the audition path (click a kit on the card → it loads and plays in context at host tempo, pattern untouched; patterns/songs import the same way). Favourites/playlists/history: not built (no evidence of need yet).
- **Sonic similarity space [H]** — ⏸ DEFERRED with reason: needs an offline render + feature-extraction farm over the whole library and a 2-D space UI; disproportionate to the rest of the phase and unproven demand. The one-shot analysis input it would need ("match this hit") is likewise deferred. Groove extraction (below) covers the "audio as analysis" slot for this phase.

### Creation
- **Guarded kit randomizer [H]** — ✅ SHIPPED IN PHASE 2 (per-voice + whole-kit, keep-set + written-value level floor ≥30%); Phase 5's mutate/breed reuse the exact same guard definitions (metadata flags), cross-checked by the Phase 5 gate.
- **Mutate / Breed [H]** — ✅ SHIPPED (Create tab: Patternarium-style mutate with amount + seed; breed per-parameter or per-voice against a second kit; lineage strip = the processor's kit history with Undo). Guards: keep-set + scope + written-level floor hold over 50 seeds × 3 amounts; breed children are byte-pure recombinations (every mutable byte is one parent's); both seed-reproducible; processor results == the pure functions.
- **Morph XY / 4-corner surface [H]** — ✅ SHIPPED as the 2-target form (Perform tab): X → target A (the classic morph target), Y → target B (new), composition = firmware morph toward A by x, then toward B by y (documented in ARCHITECTURE). Gate: pure corners byte-exact vs single-target morphs; rendered corners sample-identical to the legacy path for drum 1; XY + target B persist in state v5. The 4-corner generalisation is a straight extension of the same composition and was not needed to prove the semantics.

### Sequencing intelligence (host layer over the firmware sequencer)
- **Pattern generator + Remix [H]** — ✅ SHIPPED (Create tab: seedable generator, per-track density, Euclidean or free placement, one bar tiled ×4; Remix = rotate / probability-thin / lock-jitter, all through the seq snapshot + SeqCmd write path — no parallel pattern model). Gate: generated PatternData round-trips through the engine byte-exact, the neighbouring pattern is untouched, transforms are seed-reproducible, and a generated pattern plays through the internal-clock sequencer (peak 1.000).
- **Multi-track Euclidean composer [H]** — ✅ SHIPPED as the generator's Euclidean mode: coordinated per-track densities across all 7 tracks in one operation, with pattern-level Undo (the two things the hardware's own euklid lacks). The firmware's per-track euklid command (Phase 3 Sequencer tab) remains available.
- **Groove extraction + humanize [H]** — ✅ SHIPPED (Create tab: drop a `.mid`, extract a 16-step velocity/shift template — shift in firmware units — apply at variable strength to any pattern). Gate: crafted MIDI (velocity pattern + a +6-tick late step) extracts to the expected scales and shift +2; strength 0 is identity.
- **Infinity / evolving mode + auto-fill [H]** — ✅ SHIPPED (infinity half): a toggle that counts track-1 loop wraps and applies one small guarded mutation (amount 0.12, seeded) every 4 bars, on the undoable lineage. Gate: kit evolves within 4 bars at 240 BPM with the keep-set intact. Auto-fill scheduling: not built (fills exist as the Phase 3 roll arm; scheduled fills were the weaker half of the item).
- **Ratio/logic conditional trigs [P]** — ⏸ DEFERRED with findings (see STATUS "Phase 5, item 9"): the live trigger test has one clean hook point, but two lookahead probability evaluations must agree with it, and conditions have nowhere to live in the fixed `.PAT` layout — a state-extension + firmware-patch slice of its own. Not attempted rather than rushed against a green tree.

### Performance & DAW
- **Performance page [H]** — ✅ SHIPPED (Perform tab: 4 macros × 4 slots with anchor-relative depths, CTRL-ALL with byte-identical restore, TEMP store/recall). Gate: macro deltas land on exactly the assigned params by the documented formula; CTRL-ALL restore returns the kit byte-for-byte; assignments + positions persist in state v5. Momentary FILL rolls: the Phase 3 roll arm already covers the musical case; host-triggered 1/32 rolls not added.
- **16 Pitches tonal mode [H]** — ✅ SHIPPED (Perform tab: any note-on plays one of drums 1–3 + snare, scale-quantised — chromatic/major/minor/major-pent/minor-pent, ties resolve down — and is retro-capturable). Gate: rendered peak-freq ratios match 2^(2/12) and 2^(4/12) within FFT-bin tolerance; quantiser unit-verified.
- **Retro-capture [H]** — ✅ SHIPPED (hits played while the sequencer runs are ring-buffered with their pattern/track/step; Commit writes them into a chosen pattern through the normal write path). Gate: a hit played while step 2 was live lands as a step at step 2 (127 velocity, note 60). Scope: notes only — "tweaks" (param moves) are not captured; the ring holds the last 512 hits rather than a bar count.
- **Drag-out export [H]** — ✅ SHIPPED (Export tab: pattern → `.mid` by drag or save — notes on per-track channels, p-locks as CC (nr < 128) or NRPN (nr ≥ 128), probability resolved by a documented deterministic policy; per-voice one-shot WAVs; 8-file stems bundle from the Phase 4 stem buses). Gate: the `.mid` parses back to the identical exported step set and re-export is byte-identical; one-shot Drum 1 peak == the harness render peak (14631); bundle segment 0 Main == Stem-Drum1 with Stem-Drum2 silent. Whole-song arrangement export: not built (songs export as `.SNG` via the Library).
- **Live voice display [H]** — ⏸ DEFERRED with reason: the per-voice audio already leaves the plugin on the Phase 4 stem buses (any host analyser sees it); an in-plugin per-voice analyser needs a render-thread tap + FIFO that doesn't exist yet, and faking it from editor-side renders would misrepresent the live engine. Revisit if a future editor slice builds the tap for other reasons.

### Engine-patch headline
- **Sequencer depth pack [P]** — ⏸ DEFERRED with findings: >2 lock lanes or finer micro-timing change `Step`/`StepTiming`, and `sizeof(PatternSet)` *is* the `.PAT` file format (3,663 B) and the state layout — the pack breaks hardware interchange by construction, violating the phase invariant that kits/patterns stay on the firmware data model. Random-LFO-fresh-value-per-trigger remains possible as a pure engine patch and is un-parked separately if wanted.
(Reverb unlock already lives in Phase 4 experiments.)

### Explicitly skipped (researched and rejected)
Sample import/layering/slicing (violates the synthesis-first premise — Erica refused it for the hardware too), field-recording generation (Life), stem separation, acoustic/round-robin libraries, built-in mastering chains, standalone groovebox environments. Audio input is welcome only as *analysis* (groove extraction, match-this-hit), never as a sound source.

## Explicitly later (not this roadmap)
- LXR-1 (0.37) engine — non-commercial licence; would need its own separate, non-commercial artifact and legal care. Un-park if Garth wants a personal-use-only build.
- Eurorack module panel/matrix — closed firmware; un-park if Erica open-sources the module.
- iOS/AUv3 — un-park after Phase 4 if the plugin has users asking.
- User-sample loading — not a feature of the Erica desktop engine; un-park only if a future firmware adds it.
- Push 3 Standalone port of the plugin — a different host/runtime problem; un-park on Garth's word (the engine core is portable C, so it stays possible).
