# LXR Plugin — Architecture

## Big picture
```
DAW (Live/…) ── VST3/AU ──┐
Standalone ──────────────┤
                         ▼
              JUCE PluginProcessor
        ┌───────────────┼──────────────────┐
        ▼               ▼                  ▼
  Param bridge    Seq bridge         File layer
  (APVTS ⇄ CC     (host transport    (.SND/.PAT/
   replay into     → 96 ppq ticks     project IO,
   firmware        → seq_tick())      no FatFs)
   parser)              │                  │
        └───────────────┼──────────────────┘
                        ▼
              Engine wrapper (C API, lxr_*)
                        ▼
     Vendored firmware Core (patched copy):
     DSPAudio + FX + FxBus + MIDI + Sequencer
                        ▼
              Shim layer (HAL/GPIO/RNG/
              Cortex intrinsics/DWT/FatFs stubs)
                        ▼
        64-frame blocks @ 44,140 Hz ── resampler ──► host rate
        4 jack channels (DAC1 L/R, DAC2 L/R) ──► 2 stereo buses
```

## Components
- **vendor/** — Pinned clone of erica-synths/lxr-desktop-app (commit a43cd4f, Oct 20, 2026). Never edited directly; the build copies `Core/` into `build/src` and applies `patches/` (the PHX-D01 build pattern, which works).
- **patches/** — Numbered, commented patches against the vendor tree. Known so far: `01-oscillator-overflow-flag` (compute the ARM V flag in C — from PHX-D01, GPL, credited). Our own additions land here as discovery requires (expect: none for the DSP; the shim absorbs the rest).
- **shim/** — Host stand-ins for the STM32 world: `stm32f7xx_hal.h` (types, `__QADD16/__QSUB16/__SSAT/__PKHBT/__CLZ`, RNG → host random, GPIO array whose IDR bits are the output-jack detect switches, DWT stub), FatFs/USB header stubs. Starting point: PHX-D01's 70-line shim (GPL-3, credited), extended with the intrinsics it didn't need on Apple clang (`__SMLALD` family) and a portable `__fp16` mapping (`_Float16` or float, decided by golden-render comparison in Phase 0).
- **engine/** — A small C wrapper (`lxr_init`, `lxr_load_params`, `lxr_edit_params`, `lxr_hit`, `lxr_note`, `lxr_set_fader`, `lxr_render`, `lxr_set_morph`, kit morph via the firmware's own `preset_morph_tick` logic). Follows PHX-D01's proven shape: kits are loaded by replaying the 247 parameter bytes as front-panel CCs into the firmware's own `frontParser_parseUartData` path (exactly how the hardware loads them), edits send only changed parameters. Applies PHX's two documented firmware-bug workarounds: parameter index 127 is skipped on load (stock code writes out of bounds — `midiParser_originalCcValues[65536]`), and NULL parameter pointers in the parameter array are pointed at scratch memory (readable flash on STM32, segfault natively).
- **plugin/** — JUCE 8 (CMake) `AudioProcessor` + editor. Formats: VST3, AU, Standalone. Owns threading: the engine is **not thread-safe** (single instance state, globals everywhere — same constraint PHX documents), so all engine calls happen on the audio thread; UI/MIDI-thread requests go through lock-free queues; kit loads are prepared off-thread and swapped on a block boundary.
- **param bridge** — APVTS parameters mirroring the 247 kit parameters (raw 0–127, named from generated metadata) plus a small global set (tempo when internal, morph amount, faders). APVTS changes are translated into the same CC replay the kit loader uses, so there is exactly one parameter-entry path into the engine — the firmware's.
- **seq bridge** — Drives the firmware sequencer from the host: DAW playhead/tempo → tick accumulator at 96 ppq → `seq_tick()` (the same call the firmware's timer ISR makes inside `soundengine_process()`), falling back to an internal clock in Standalone. Pattern/song state lives in the firmware's own `seq_patternSet`; the editor reads/writes it through the wrapper, never through a parallel model.
- **file layer** — `.SND` read/write (255 B desktop kits; reads 309 B module kits by ignoring the matrix tail), `.PAT` pattern-set read/write, `PROJnn` project-folder scan — ported from `presetManager.c`'s layouts with FatFs calls replaced by stdio. INIT kit ships from the firmware's own `initKit.c` (GPL, in-tree). Erica's factory kits are **not** bundled (they're a separate download on Erica's site); the browser imports user folders/cards.
- **UI metadata generation** — `tools/gen_pages`: the editor's page structure (voice pages, FX page, labels, display types, value-name lists like filter types and waveforms) is generated from the firmware's own menu tables (`menu.c`/`menuPages.h`), PHX-D01-style, so UI text can't drift from the hardware. Committed as generated source, regenerated when the vendor pin moves.
- **tools/** — `lxr-render` (offline CLI: load kit → hit voices → WAV; our golden-test workhorse, equivalent to PHX's phx-render), render-diff script (peak/RMS/spectral comparison), intrinsics vector tests.

## Data flow / protocols
- **Parameters in:** UI knob / DAW automation / MIDI CC-NRPN → APVTS or the firmware MidiParser → parameter array → voices read parameters per block, as on hardware.
- **Notes in:** DAW MIDI → firmware voice mapping (`voiceControl_noteOn`: 7 voices incl. open hihat as voice 6; per-voice MIDI channel and note-override parameters from the global parameter set) → voice triggers.
- **Audio out:** `mixer_calcNextSampleBlock()` fills the two DMA buffers = DAC1 (main stereo) + DAC2 (second stereo pair); per-voice routing parameters (St1/St2/L1/R1/L2/R2/FX) behave as on hardware across the two buses.
- **Rate contract:** the engine always runs at its native `REAL_FS` 44,140 Hz in 64-frame blocks. The plugin keeps a small FIFO per bus and resamples once at the boundary to the host rate (r8brain-free-src). Rationale: every EG/LFO/pitch constant in the firmware is tuned against 44,140; running the engine "at host rate" by redefining REAL_FS silently retunes the instrument (PHX-D01 runs it at a declared 44,100 and sits ~1.6 cents off). Fidelity first; the resampler costs little on drums.
- **State:** DAW project state = the 247 kit bytes + pattern set + globals, stored via APVTS/`getStateInformation` as the raw firmware structures (versioned), so state files stay diff-able against hardware files.

## Dependencies
- **JUCE 8** — plugin framework. Used under its GPLv3 option (this project is GPL-3.0); no JUCE licence fee while the project stays open-source.
- **erica-synths/lxr-desktop-app** — GPL-3.0-only, pinned commit a43cd4f. The engine itself.
- **mechanical-dance/PHX-D01** — GPL-3.0. Shim, oscillator patch, engine-wrapper shape and bug findings reused with attribution in NOTICE. It is a dependency of our *knowledge*, not of our build (we vendor our own copies).
- **r8brain-free-src** — MIT, sample-rate conversion.
- **CMake + ninja** — build; macOS via Xcode CLT on Garth's Mac, Windows via MSVC (Phase 4).
- **pluginval** — validation in Phase 4 gates.

## What's missing
- ~~A product name~~ — resolved Oct 28, 2026: **ElfDrumLXR** (Garth). In-tree codename stays `lxr-plugin`; JUCE project/plugin name = ElfDrumLXR from Phase 1.
- **Factory kits.** Not in the firmware repo and not bundled; INIT only, plus import. (Writing our own demo kits with the finished plugin is a Phase 2 nicety.)
- **Reverb.** Present in the DSP tree, compiled out upstream (`USE_REVERB 0`), completeness unknown. Phase 4 experiment, off by default.
- **Per-voice stem outputs.** The mixer routes to 2 stereo pairs only; true per-voice breakouts would require mixer surgery (a patch, not a config) — Phase 4 evaluation, not v1.
- **Signing/notarisation.** Ad-hoc signing first (as with previous Mac builds); a Developer ID is Garth's call if releases go public.
- **Song (.SNG-era) file details.** Pattern chaining and song mode exist in the sequencer API; the exact on-disk song storage in the desktop firmware still needs mapping during Phase 3 discovery (honest unknown, one focused read of presetManager.c settles it).
