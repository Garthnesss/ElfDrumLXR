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

### Kit ecosystem
- **Kit Blender + per-voice preset library [H]** — save/load single voices; assemble kits from voices across kits. The #1 verbatim LXR-02 owner request (Elektronauts); trivial here because a voice is a fixed slice of the 247 kit bytes. **Groundwork SHIPPED with Phase 2** (per-voice Copy/Paste/Save/Load + the `.lxv` format); what remains here is the library/blender UI layer on top of it.
- **SD-card project librarian + shareable kit packs [H]** — browse PROJnn folders by real names/tags, sync hardware card ⇄ plugin library both ways, export/import kits+patterns as packs with author/artwork metadata. Owners cite kit/pattern management as the hardware's biggest pain.
- **In-context audition browser [H]** — swap kit-keep-pattern / pattern-keep-kit at host tempo; favourites, playlists, history.
- **Sonic similarity space [H]** — offline-render the kit library through the engine, map kits by rendered features (XO/Atlas-style space, but the pool is synth presets); "match this hit": analyse a dropped one-shot, suggest the closest kit/voice settings.

### Creation
- **Guarded kit randomizer [H]** — amount, per-section locks, never-silence floor (≥30% on level-critical params), never touch routings; per-voice or whole-kit. (Extends the Phase 2 per-voice randomise; guards proven on this engine by the LXR02XXL community mod.)
- **Mutate / Breed [H]** — Patternarium-style: mutate current kit, breed two kits with per-voice inheritance, keep a lineage strip with undo.
- **Morph XY / 4-corner surface [H]** — generalises the firmware morph: 2 or 4 kit corners, XY pad performance, freeze any blend as a new kit. (Depends on the Phase 1 morph fix.)

### Sequencing intelligence (host layer over the firmware sequencer)
- **Pattern generator + Remix [H]** — genre/density-seeded generation writing firmware pattern data, per-track locks, remix variations.
- **Multi-track Euclidean composer [H]** — coordinated Euclidean across tracks with preview and undo (the hardware's Euclidean overwrites without undo).
- **Groove extraction + humanize [H]** — timing/velocity templates from dropped MIDI/clips applied to patterns; per-track slop.
- **Infinity / evolving mode + auto-fill [H]** — bar-counted mutations and scheduled fills at pattern boundaries.
- **Ratio/logic conditional trigs [P]** — 1:2, 3:4, PRE/NEI/FILL beyond the firmware's probability — owner-requested verbatim; runtime evaluation makes it an engine patch.

### Performance & DAW
- **Performance page [H]** — assignable macros, CTRL-ALL, hold-to-mangle / release-to-restore snapshots (TEMP), momentary FILL with fast rolls (incl. 1/32 host-triggered).
- **16 Pitches tonal mode [H]** — scale-locked pitch playing of voices 1–3 + snare from a MIDI keyboard/pads, recordable as step pitch (Move-style).
- **Retro-capture [H]** — rolling buffer of the last N bars of incoming MIDI + tweaks, commit to a pattern on demand.
- **Drag-out export [H]** — pattern → MIDI file/clip with parameter locks translated to CC/NRPN (probability resolved by a documented policy), per-voice one-shot WAVs, per-voice stems, whole-song arrangement.
- **Live voice display [H]** — rendered waveform/spectrum per triggered voice with pitch-curve readout (Kick-3-style analyser, but the input is our own synthesis).

### Engine-patch headline
- **Sequencer depth pack [P]** — >2 lock lanes per step, finer micro-timing resolution, random-LFO-fresh-value-per-trigger (on Erica's own firmware-ideas list). Bundle with ratio trigs above.
(Reverb unlock already lives in Phase 4 experiments.)

### Explicitly skipped (researched and rejected)
Sample import/layering/slicing (violates the synthesis-first premise — Erica refused it for the hardware too), field-recording generation (Life), stem separation, acoustic/round-robin libraries, built-in mastering chains, standalone groovebox environments. Audio input is welcome only as *analysis* (groove extraction, match-this-hit), never as a sound source.

## Explicitly later (not this roadmap)
- LXR-1 (0.37) engine — non-commercial licence; would need its own separate, non-commercial artifact and legal care. Un-park if Garth wants a personal-use-only build.
- Eurorack module panel/matrix — closed firmware; un-park if Erica open-sources the module.
- iOS/AUv3 — un-park after Phase 4 if the plugin has users asking.
- User-sample loading — not a feature of the Erica desktop engine; un-park only if a future firmware adds it.
- Push 3 Standalone port of the plugin — a different host/runtime problem; un-park on Garth's word (the engine core is portable C, so it stays possible).
