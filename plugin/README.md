# ElfDrumLXR plugin (Phase 1)

JUCE shell around the Phase 0 engine harness. Formats: VST3 + Standalone everywhere,
plus AU on macOS. Product "ElfDrumLXR", company "Elf", manufacturer code `ElfM`,
plugin code `Edlx`, bundle id `com.garth.elfdrumlxr`, AU type `aumu` (music device).

## Pinned dependencies (fetched, never committed)

| Dependency | Pin | Fetch |
|---|---|---|
| LXR-02 firmware (Erica Synths, GPL-3.0) | commit `a43cd4fcda23ec3d577dc425261c3eb28bd5abff` | `../scripts/fetch-vendor.sh` → `../vendor/lxr-02` |
| JUCE (GPLv3 option) | tag `8.0.9` | `./fetch-deps.sh` → `vendor/JUCE` |
| r8brain-free-src (MIT) | commit `cb2abb9977efe2471979b380ed95daa56ab4fdb9` | `./fetch-deps.sh` → `vendor/r8brain-free-src` |

## Build

```sh
../scripts/fetch-vendor.sh   # firmware into ../vendor/lxr-02
./fetch-deps.sh              # JUCE + r8brain into vendor/
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

macOS universal: add `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`. Outputs land in
`build/ElfDrumLXR_artefacts/Release/` (`VST3/`, `AU/`, `Standalone/`) and the offline
gate harness at `build/plugin_render_artefacts/Release/plugin-render`.

`-DELF_FP16=float` switches the firmware's `__fp16` mapping from `_Float16` (default,
the Phase 0 decision) to plain float.

## Design notes (Phase 1)

- **Rate contract** (ARCHITECTURE.md): the engine always runs at its native 44,140 Hz in
  64-frame blocks. `EngineHost` feeds the four jack channels through r8brain resamplers
  (150 dB stop-band, 2.5 % transition band, linear phase) into per-channel FIFOs at the
  host rate. Resampler latency is measured with an impulse at `prepareToPlay` and reported
  via `setLatencySamples`.
- **Buses**: bus 1 "Main (DAC1)" = engine jacks 1/2, bus 2 "Out 2 (DAC2)" = jacks 3/4 —
  the hardware's two stereo DAC pairs, with the firmware's own per-voice routing intact.
- **Parameters**: APVTS `kit000`…`kit246` are the 247 kit bytes (stepped 0–127; generic
  names until Phase 2 generates the real ones from the firmware menu tables — the IDs are
  stable). Plus `morph` (0–1). One parameter-entry path only: the bridge reads APVTS every
  block and pushes changes through the wrapper's CC-replay/edit-diff (`lxr_load_params` /
  `lxr_edit_params`), the same path kit loads use. Morph crossfades the current kit toward
  a morph-target kit with the firmware's own morph interpolation (`lxr_morph_kit`); loading
  a kit makes the *previous* kit the morph target.
- **State**: `getStateInformation` writes a version byte, the 247 base-kit bytes, the 247
  morph-target bytes, the morph amount and the 8 kit-name bytes — the raw firmware data,
  restorable byte-for-byte. `.SND` export writes the same 255-byte layout the hardware
  uses (8-byte name + 247 params); import accepts 255-byte desktop kits and 309-byte
  module kits (matrix tail ignored).
- **Threading**: the engine is a single global state machine (firmware design) and lives
  entirely on the audio thread. Message-thread requests arrive via APVTS atomics, an
  atomic kit-request pointer, and a lock-free trigger ring.

## MIDI

- **Notes**: the firmware's external-MIDI behaviour is *channel*-based (each voice listens
  on its own channel; the global channel plays the front panel's active track), and its
  per-voice note parameters (`PAR_MIDI_NOTE1..7`) are inert placeholders in this firmware —
  the `parameterArray` hooks for them are commented out upstream, and the factory defaults
  (`preset_loadDefaultGlobals`) set every voice note to 48 and every channel to 1.
  The plugin therefore maps notes like this, reading the values live from the engine's
  globals via the wrapper:
  - If the seven configured notes are **not** all equal, a note triggers every voice whose
    configured note matches (and whose channel matches, unless the global channel is 0 =
    "all"), pitched by the played note — the firmware's own configuration, honoured.
  - While they **are** all equal (the stock state), the GM drum map selects the voice
    (36/45/50/38/39/42/46 for Drum 1–3/Snare/Cymbal/HiHat/Open Hat — the same map PHX-D01
    ships), and the played note's offset from the map note pitches the voice around the
    kit reference note 60.
  - Note offs are ignored (one-shot drums), as on the hardware.
  - Note timing is rendered at the hit's position inside the host block (converted to the
    engine-frame grid), not quantised to the block start.
- **Everything else** (CC, the NRPN sequence, pitch bend, program change, pressure) is fed
  as a raw byte stream into the firmware's own parser, `midiParser_parseUartData` — the
  exact code the hardware runs. Firmware protocol facts (pinned by
  `tests/test_midi_parser.c`): plain CC *n* sets parameter *n−1*; NRPN is CC 99/98
  selecting a CC2 number (= parameter index − 128) with CC 6 entering the value; there is
  no data-entry-fine handling (CC 38 is a live parameter CC: drum 1 filter frequency).
  Caveat: sequencer-side effects of the parser (automation recording, program-change →
  pattern change) act on the firmware sequencer state, which the plugin doesn't drive
  until Phase 3.

## Known Phase 1 limitations

- The engine is a process-wide singleton: multiple plugin instances in one host share one
  engine (the firmware's globals-everywhere design; PHX-D01 has the same constraint).
- Incoming parser CCs change voice state directly in the firmware structs; the APVTS
  bridge does not reflect them back into the host's parameter view (host automation wins
  the next time it moves — the bridge never reverts a CC change on its own).
