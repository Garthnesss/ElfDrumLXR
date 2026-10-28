# LXR Plugin — Status

**Last updated:** 2026-10-28
**Phase:** Phase 0 COMPLETE on the Muse VM (Linux) — gate passed, evidence below. Phase 1 (JUCE shell) not started. Honest scope: what runs today is the *engine harness* on Linux; no plugin exists yet, and nothing has been built or loaded on macOS/Ableton.

## Where things stand
- Upstream firmware — **EXAMINED** (cloned to `~/workspace/lxr-desktop-app`, commit a43cd4f; 46,255 LOC of C in Core; DSP ~15.8k LOC; kit = 247 params; sequencer/FX/MIDI specs extracted from source, Oct 28, 2026).
- DSP host-compilability — **VERIFIED** (Muse compile test on the VM: 31/34 DSP files compile natively with a 2-header stub + `-D__fp16=float` + portable intrinsics; the 3 failures are the known oscillator/mixer/modulation-node stubs).
- Rival implementation (PHX-D01) — **AUDITED, RUNS** (cloned to `~/workspace/PHX-D01`; its phx-render was built on this VM and rendered the INIT kit: all 6 voices, spectral profile verified — kick 54 Hz peak, snare centroid 6.3 kHz, cymbal 8.4 kHz, hihat 12.4 kHz. Evidence: `~/workspace/your_files/phx-d01-audit-init-render.wav` + `phx-d01-audit-render.png`). Its architecture validates this plan's engine approach; its gaps (standalone-only, reduced sequencer, no releases) define our differentiation.
- Plugin code — **NOT STARTED**. No JUCE project yet (Phase 1).
- Engine harness (Phase 0) — **RUNS on the Muse VM** (gcc 13, Linux x86-64): `scripts/fetch-vendor.sh` pins the firmware at a43cd4f into `vendor/lxr-02`; the Makefile copies it to `build/src`, applies `patches/01` (PHX-D01's oscillator overflow patch, verbatim), compiles all 44 engine sources against `shim/` + `engine/` (PHX-D01's wrapper adapted, module mod-matrix dropped) into `tools/lxr-render`.
- Plan docs — **WRITTEN** (this set, Oct 28, 2026).

## Phase 0 gate evidence (Oct 28, 2026, Muse VM)
- Intrinsics: **28/28 checks pass** (`make test`) — `__QADD16/__QSUB16/__SADD16/__SMLALD/__SMLALDX/__SSAT/__PKHBT` against hand-computed ARM-semantics vectors incl. saturation edges. (Writing the tests caught two errors in the *test vectors*, none in the shim; a transcription error in `__QSUB` was caught by diffing the shim against PHX-D01's original before building.)
- INIT render vs PHX-D01 reference (`phx-d01-audit-init-render.wav`, same kit, same 6×0.6 s layout): **drums 1–3 are 100.00% sample-exact** — peak/centroid/peak-frequency identical (13866/14272/14564; 150/377/879 Hz; 48/95/230 Hz). That beats the roadmap gate (±1 LSB class) — the deterministic path is bit-identical to PHX-D01's build.
- Noise voices, 15-run distributions vs PHX-D01's own binary on the same kit: snare centroid −0.0%, RMS −0.6%, peak mean −0.7%; cymbal centroid +0.0%, RMS +1.0%, peak mean −3.9% (peak is a noisy max-statistic, sd ≈ 1250 on both sides); hihat centroid −0.1%, RMS −0.0%, peak +0.1%. All well inside the statistical tolerances (centroid ~10%, peak ~20%).
- Sanity numbers from the gate: kick (drum1) spectral peak 48 Hz (< 100 Hz ✓), hihat centroid 8.4 kHz, cymbal 8.3 kHz, snare 6.6 kHz.
- `__fp16` decision: **ship `_Float16`** (exact IEEE half, same as ARM `__fp16` and PHX-D01's gcc build). The `float` mapping also passes every gate (drums bit-identical again; noise within the same tolerances), so the INIT kit can't discriminate them — `_Float16` wins on principle (it doesn't widen the FX RAM buffer's precision beyond the hardware's). Revisit if an FX-heavy kit ever shows a difference.
- CPU (rough): ≈0.09 s user time to render 3.6 s of audio including process start + kit load ≈ **40× realtime**, ≈36 µs per 64-frame block on this VM.
- Surprise worth knowing: the pinned firmware never actually *calls* `__SMLALD`/`__SADD16` — the only `__SADD16` occurrences are inside comments in mixer.c. The live intrinsic set is `__QADD16` (65), `__SSAT` (77), `__PKHBT` (16), `__QSUB16` (3), `__CLZ` (2). The shim still provides the full documented set, tested.
- Remaining Phase 0 roadmap item NOT done: hand-built test kits exercising every FX type/filter type. The INIT kit leaves the FX bus effectively untested against PHX. Carried into Phase 1 as a harness task (the comparison machinery now exists and is cheap to re-run).

## Decisions made
- **Product name: ElfDrumLXR** (Oct 28, 2026, Garth). Note: the name carries the LXR mark descriptively — Garth's call, made with the trademark caveat in front of him.
- **Repo home: Garth creates the GitHub repo himself** (Oct 28, 2026) — scaffold proceeds once he hands over the repo name/URL.
- Full JUCE plugin planned before building (Oct 28, Garth) — supersedes the earlier "M0 render first" ordering; Phase 0 keeps the render harness as its gate anyway.
- DAW-first, sequencer at parity, PHX-D01 GPL intel reusable with credit (Oct 28, Garth — the GO decision).
- Engine runs at native 44,140 Hz + resampling, rather than retuning to host rate (Oct 28, Muse — fidelity call, recorded in ARCHITECTURE.md; PHX's 44,100 shortcut is ~1.6 cents off).
- Two stereo buses faithful to the hardware DAC pairs for v1; per-voice stems deferred to a Phase 4 experiment (Oct 28, Muse).
- Reverb ships off, as upstream has it (Oct 28, Muse — fidelity; Phase 4 experiment may revisit).

## Open decisions (need Garth's call)
1. **Release model** — free download (GPL source public) vs paid binaries with source available (GPL allows both). Blocks Phase 4 packaging choices only.
2. **Validation DAWs** — Ableton Live assumed primary; name any second host (Reaper/Logic) for the Phase 1/4 gates.

## Next actions
- [ ] Muse: Phase 1 scaffold (JUCE 8 shell: VST3/AU/Standalone) on Garth's go; first Mac build + Live load test is the Phase 1 gate.
- [ ] Muse: FX/filter test-kit sweep through the harness (carried over from Phase 0) before the panel work depends on FX behaviour.
- [ ] Muse: keep the upstream pin under watch — Erica's repo is days old and moving; re-check for new commits before Phase 1 starts.
