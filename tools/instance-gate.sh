#!/bin/sh
# ElfDrumLXR Phase 4 (5) gate: per-instance engines.
#   - the committed census matches the built engine library
#   - swap engages; instance A's render with an interleaved partner is
#     bit-identical regardless of the partner's kit/pattern (isolation),
#     pattern sets byte-stable and distinct — see PluginInstanceTest.cpp
#     for why partner-invariance (not solo-vs-interleaved) is the sound
#     criterion for this engine.
# Expects the plugin build tree to exist (plugin/build).
set -e
cd "$(dirname "$0")/.."
B=plugin/build
BIN=$B/plugin_instance_test_artefacts/plugin_instance_test
[ -x "$BIN" ] || BIN=$B/plugin_instance_test_artefacts/Release/plugin_instance_test

python3 tools/gen_census.py check
mkdir -p /tmp/instgate
"$BIN" /tmp/instgate "$PWD/tests/kits"
python3 tools/render-compare.py /tmp/instgate/inst-run2A.wav /tmp/instgate/inst-run1A.wav
echo "INSTANCE GATE PASS"
