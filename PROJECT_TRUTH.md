# LXR Plugin — Project Truth

## One line
A DAW plugin (VST3/AU + standalone) that runs Erica Synths' open-source LXR-02 firmware sound engine and sequencer natively — the real code, not a recreation — with full hardware kit/pattern compatibility.

## What it is
- A native port of the official firmware (erica-synths/lxr-desktop-app, GPL-3.0, commit a43cd4f): 6 voices (3× drum synth, snare/clap, cymbal, hihat), 247 kit parameters, FX bus (distortion ×3 flavours, ring modulator, compressor, delay — reverb exists in the DSP tree but is compiled OUT upstream, `USE_REVERB 0`), per-voice LFO + velocity modulation, kit morphing.
- The firmware's own sequencer, not a substitute: 64 patterns per project, 7 tracks, per-step volume/note/probability, **two parameter locks per step**, flam + micro-shift timing, per-track length, shuffle, rolls, pattern chaining and song mode, Euclidean generator. Internal clock 96 ppq, sequencer resolution 32 ppq.
- File-compatible with the hardware both ways: kits are `.SND` (255 bytes = 8-byte name + 247 parameter bytes), patterns are `.PAT` pattern-set files in `PROJnn` project folders, exactly as the firmware's preset manager reads/writes them.
- A JUCE plugin first (VST3/AU/Standalone, macOS first, Windows in Phase 4) with host sync, host automation of the kit parameters, and the hardware's two stereo output pairs as plugin buses.

## What it is not
- **Not an emulation or a remodel.** No DSP is rewritten or "inspired by" — the firmware C is compiled and run, with only documented host patches. (The one exception class: entropy. Noise voices use the hardware RNG on the STM32; any port substitutes its own random source, so noise-based voices are statistically, not bit-, identical. Same is true of every other port, including PHX-D01.)
- **Not a standalone kit-auditioning app.** PHX-D01 (audited Oct 28, 2026 — real, verified by building and rendering with it on the Muse VM) already does that well on macOS. This project's ground is the DAW: plugin formats, host transport, automation, packaged releases.
- **Not the Eurorack LXR-Drum module.** That firmware is closed; PHX-D01's module panel is a manual-based recreation. Out of scope unless Erica open-sources it too.
- **Not affiliated with or endorsed by Erica Synths or Sonic Potions.** It ships as **ElfDrumLXR** (Garth's name, Oct 28, 2026) under our own branding; "LXR" appears descriptively (the product runs the LXR-02 firmware engine). GPL covers code, not trademarks — all packaging will carry a clear not-affiliated notice.
- **Not a hardware firmware mod.** Nothing here flashes a device; the recovery-safe SD update path is a separate world.

## Non-negotiables
1. **Real code, minimal patches.** The DSP/engine/sequencer source is vendored unmodified; every host adaptation is a numbered patch in `patches/` with a comment saying why (the model PHX-D01 proved: exactly one patch was needed for the whole engine — the Oscillator ARM overflow-flag read).
2. **Sound is verified, not asserted.** Every phase gates on golden renders: our harness vs reference renders (PHX-D01's phx-render, same upstream code) compared by peak/RMS/spectral profile per voice, and deterministic voices null-tested where the RNG isn't involved.
3. **GPL-3.0, own name.** The plugin ships under GPL-3.0 with full source, under a name that is ours. Reuse of PHX-D01's GPL shim/patch work is allowed and will be credited in NOTICE.
4. **Hardware files round-trip.** A kit or pattern made in the plugin must load on a real LXR-02 and vice versa; byte-level round-trip tests are part of the gates, not a hope.
5. **Honest status language.** BUILT NOT TESTED is not RUNS. Mac/DAW claims require a build on Garth's Mac and a load in Ableton Live, not a green Linux compile.

## Confirmed by Garth
- Oct 28, 2026: Asked whether a software version could be made from the Erica source; after the feasibility check, chose "plan the full plugin (JUCE VST3/AU + UI) first" over going straight to an M0 render proof.
- Oct 28, 2026: Chose "audit PHX-D01 first" before committing, then, after the audit verdict, chose **GO**: "finish the plugin plan — DAW-first, sequencer parity, reuse PHX's GPL shim/patch intel."

## Scope boundaries
- In scope: desktop (Erica) engine + sequencer, VST3/AU/Standalone, macOS then Windows, .SND/.PAT/.SNG-era project compatibility as far as the firmware formats go.
- After parity, a Phase 5 "Bonus" programme of host-side differentiators (kit ecosystem, creation tools, sequencing intelligence, performance/DAW features) extends the plugin beyond the hardware — see ROADMAP.md Phase 5.
- Out of scope for v1: the LXR-1 (Sonic Potions 0.37) engine — its licence is non-commercial and conflicts with a clean GPL release; user-sample loading (a feature of that other engine / LXR02Open, not of the Erica desktop engine); iOS/AUv3; CLAP (evaluated in Phase 4 via clap-juce-extensions, not promised).
- The disabled reverb (`USE_REVERB 0`) ships disabled in v1 for fidelity. Turning it on is a Phase 4 experiment, not a launch feature — it is off upstream for unknown reasons and the code path is unproven.
