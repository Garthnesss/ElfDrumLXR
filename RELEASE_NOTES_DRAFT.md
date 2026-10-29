# ElfDrumLXR — DRAFT release notes (not published)

> Status: DRAFT for Garth's review. The release-model decision (whether /
> where to publish binaries) is parked with Garth — see STATUS.md. Nothing
> here has been released.

## What ElfDrumLXR is

A native DAW plugin (VST3 / AU / Standalone, plus an experimental CLAP
target) that runs the Erica Synths LXR-02 drum machine's open-source
firmware (GPL-3.0, pinned upstream commit `a43cd4f`) directly as compiled
C — no emulation layer — at the firmware's native 44,140 Hz, resampled to
the host rate with r8brain. All 247 kit parameters, the 6-voice mixer,
the FX bus, and the full pattern/song sequencer behave as the firmware
defines them; the Phase 0 harness proved the pitched voices sample-exact
against the reference desktop port.

## Highlights by phase

- **Phase 1** — JUCE plugin: 2 stereo hardware output pairs, kit load/save
  (.SND byte-exact), morph between kits, minimal editor.
- **Phase 2** — Full panel generated from the firmware's own menu tables
  (247 kit params + globals, no hand-maintained metadata), kit browser,
  per-voice presets (.lxv), guarded randomise.
- **Phase 3** — Sequencer parity on the firmware's own code: 64 patterns ×
  7 tracks with probability/p-locks/flam/shift, .PAT/.SNG file formats,
  song mode, host-transport sync + standalone internal clock, state v4.
- **Phase 4** —
  - **Reverb**: the firmware's compiled-out Spin-style reverb is enabled
    as a sixth FX type (existing kits unaffected — the value never occurs
    in them; all other FX renders bit-identical).
  - **Per-voice stem outputs**: six extra stereo buses carrying each
    voice's post-fader, post-pan contribution (pre-FX), for stem
    recording in the DAW. Default layout unchanged (mains only).
  - **Per-instance engines**: multiple plugin instances in one process
    now have fully independent kits, patterns and voices (previously the
    firmware's global state was shared). Proved by a partner-invariance
    gate: an instance's render is bit-identical regardless of which other
    instance it is interleaved with. Linux/ELF only for now; other
    platforms keep the previous singleton behaviour automatically.
  - **Deterministic noise**: the host RNG shim is now a seeded xorshift
    (was OS entropy), so noise voices render reproducibly.
  - **CLAP**: builds and passes a headless load/render smoke test against
    an independent minimal host; in-DAW testing outstanding.

## Known limitations

- AU validation (`auval`) and in-Ableton by-ear checks are the
  maintainer's manual gates; see STATUS.md for the current list.
- Per-instance separation is ELF/Linux-only in this release; macOS and
  Windows builds fall back to one shared engine state per process
  (fail-safe, automatic).
- macOS binaries are unsigned/ad-hoc: notarisation requires the
  maintainer's Apple Developer ID. Windows is built by CI but not yet
  tested on a Windows machine.
- The FX type list gained "Rev" as value 5; kits saved by the hardware
  firmware never contain it.

## Artefacts (prepared locally, unpublished)

- `dist/` layout: macOS .pkg (unsigned) + source/binaries zip with
  SHA256 checksums — see dist/README once generated.
