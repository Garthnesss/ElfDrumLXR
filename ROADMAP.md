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

## Phase 2 — "The panel"
- Generated UI metadata from the firmware menu tables (labels, value lists, display types) committed via `tools/gen_pages`.
- Editor laid out on the hardware's own menu structure: 6 voice pages (Osc/EG/Mod/FM/Transient/Filter/LFO/Mix), FX page, global page — knobs and value fields, no photo-skeuomorphism.
- Kit browser: folders/cards of `.SND` as banks (slot order from filenames), prev/next, INIT, randomise-per-voice (a gap PHX and the hardware both leave open — small, high-value).
- Morph slider wired to the firmware morph; per-voice faders (the hardware's 7 faders → 6 voices + master).
- Gate: every one of the 247 parameters is visible/editable in the UI (automated UI↔parameter-map cross-check against generated metadata), automatable, and a Live project saved and reloaded restores the exact kit (Garth-verified in Live).

## Phase 3 — "Sequencer parity"
- Discovery first (one pass, written up in STATUS): exact main-step/substep editing semantics in the desktop firmware, song storage format, chaining rules — from `sequencer.c` + `presetManager.c`, not from the manual.
- Host-transport drive: DAW position/tempo → 96 ppq tick accumulator → `seq_tick()`; standalone falls back to internal clock; external MIDI clock input honoured via the firmware's sync path.
- Pattern editor on the firmware's data model: main steps + substep detail, per-step velocity/note/probability, **two parameter locks**, flam + shift timing, per-track length, track mute, rolls, shuffle, Euclidean generator, 64-pattern set, chaining + song mode.
- `.PAT` import/export through the ported preset-manager layouts.
- Gate: (1) a pattern authored in the plugin exports a `.PAT` that the ported firmware parser reads back into an identical pattern set (automated); (2) a fixed pattern + kit render an identical trigger-event log (voice, time, velocity, note, p-lock changes) in the plugin and in a reference harness driving `seq_tick()` directly; (3) checklist diff vs PHX-D01's sequencer (32 steps, no probability/p-locks/substeps) shows every gap closed; (4) Garth plays it in Live against host transport, by ear.

## Phase 4 — "Reach & polish"
- Windows x64 build (VST3 + Standalone); CLAP evaluated via clap-juce-extensions (ship only if it passes the same gates without a second codebase).
- Packaged releases: signed/notarised macOS installer + Windows zip, README + short manual, pluginval clean on both platforms.
- Experiments, each behind its own mini-gate and **off** until passed: `USE_REVERB 1` (prove the reverb path complete + stable + good-sounding, or document why upstream disabled it and drop it); per-voice stem outputs (mixer patch — prove routing semantics unchanged on the main buses).
- Gate: clean install on a fresh user account on Mac and Windows; pluginval strictness 5+ passes; release renders re-run through the Phase 0 golden suite unchanged.

## Explicitly later (not this roadmap)
- LXR-1 (0.37) engine — non-commercial licence; would need its own separate, non-commercial artifact and legal care. Un-park if Garth wants a personal-use-only build.
- Eurorack module panel/matrix — closed firmware; un-park if Erica open-sources the module.
- iOS/AUv3 — un-park after Phase 4 if the plugin has users asking.
- User-sample loading — not a feature of the Erica desktop engine; un-park only if a future firmware adds it.
- Push 3 Standalone port of the plugin — a different host/runtime problem; un-park on Garth's word (the engine core is portable C, so it stays possible).
