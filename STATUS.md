# LXR Plugin — Status

**Last updated:** 2026-10-28
**Phase:** Plan complete (Phases 0–4 in ROADMAP.md). Phase 0 not started — awaiting Garth's go on the scaffold.

## Where things stand
- Upstream firmware — **EXAMINED** (cloned to `~/workspace/lxr-desktop-app`, commit a43cd4f; 46,255 LOC of C in Core; DSP ~15.8k LOC; kit = 247 params; sequencer/FX/MIDI specs extracted from source, Oct 28, 2026).
- DSP host-compilability — **VERIFIED** (Muse compile test on the VM: 31/34 DSP files compile natively with a 2-header stub + `-D__fp16=float` + portable intrinsics; the 3 failures are the known oscillator/mixer/modulation-node stubs).
- Rival implementation (PHX-D01) — **AUDITED, RUNS** (cloned to `~/workspace/PHX-D01`; its phx-render was built on this VM and rendered the INIT kit: all 6 voices, spectral profile verified — kick 54 Hz peak, snare centroid 6.3 kHz, cymbal 8.4 kHz, hihat 12.4 kHz. Evidence: `~/workspace/your_files/phx-d01-audit-init-render.wav` + `phx-d01-audit-render.png`). Its architecture validates this plan's engine approach; its gaps (standalone-only, reduced sequencer, no releases) define our differentiation.
- Plugin code — **NOT STARTED**. No repo scaffold, no JUCE project yet.
- Plan docs — **WRITTEN** (this set, Oct 28, 2026).

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
- [ ] Garth: finish creating the GitHub repo and hand Muse the name/URL (+ add it to the fine-grained token's repository access if reads/writes via the API are wanted, per the standing GitHub rule).
- [ ] Muse: on repo-ready, scaffold Phase 0 (vendor pin, patches, shim, engine wrapper, lxr-render) and run the Phase 0 gate suite.
- [ ] Muse: keep the upstream pin under watch — Erica's repo is 8 days old and moving; re-check for new commits before Phase 0 starts.
