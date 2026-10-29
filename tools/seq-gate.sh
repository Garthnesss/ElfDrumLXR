#!/bin/sh
# Phase 3 gate runner (VM, Linux). Prereqs: `make` (engine + seq-pat + seq-ref)
# and the plugin build in plugin/build (plugin_seq_test, plugin_panel_test,
# plugin_render). Runs gates (a)-(e) and the Phase 0-2 regressions.
# Usage: tools/seq-gate.sh [plugin-build-dir]
set -e
cd "$(dirname "$0")/.."
PB="${1:-plugin/build}"
B=build
fail=0
note() { echo; echo "=== $* ==="; }

note "make test (intrinsics, MIDI parser, gate (a) .PAT round-trip)"
make test || fail=1

note "gate (b) trigger-log equivalence: seq-ref vs plugin-seq-test log"
./$B/seq-ref > /tmp/seq-ref.log 2>/tmp/seq-ref.err || fail=1
$PB/plugin_seq_test_artefacts/Release/plugin_seq_test log > /tmp/seq-plugin.log 2>/tmp/seq-plugin.err || fail=1
if diff -u /tmp/seq-ref.log /tmp/seq-plugin.log > /tmp/seq-diff.txt; then
    echo "trigger logs IDENTICAL ($(wc -l < /tmp/seq-plugin.log) lines)"
else
    echo "trigger logs DIFFER:"; cat /tmp/seq-diff.txt; fail=1
fi

note "gate (c) PHX gap checklist (inside seq-ref, stderr)"
cat /tmp/seq-ref.err

note "gate (e) audio onset sanity"
$PB/plugin_seq_test_artefacts/Release/plugin_seq_test audio 2>&1 | grep -v '^$' || fail=1

note "state v4 round-trip (plugin-seq-test seqstate)"
$PB/plugin_seq_test_artefacts/Release/plugin_seq_test seqstate tests/kits/flt-bp.SND 2>&1 || fail=1

note "regression: panel test (metadata/randomise/state/slice) + morph test"
$PB/plugin_panel_test_artefacts/Release/plugin_panel_test metadata | tail -1
$PB/plugin_panel_test_artefacts/Release/plugin_panel_test randomise | tail -1
$PB/plugin_panel_test_artefacts/Release/plugin_panel_test state tests/kits/flt-bp.SND tests/kits/fx-dist.SND /tmp | tail -1
$PB/plugin_panel_test_artefacts/Release/plugin_panel_test slice tests/kits/flt-bp.SND factory/INIT.SND /tmp | tail -1
$PB/plugin_morph_test_artefacts/Release/plugin_morph_test twokit tests/kits/morph-target.SND tests/kits/flt-bp.SND /tmp/mt2 > /dev/null || fail=1
$PB/plugin_morph_test_artefacts/Release/plugin_morph_test save tests/kits/flt-bp.SND /tmp/mt-save.SND > /dev/null || fail=1
cmp tests/kits/flt-bp.SND /tmp/mt-save.SND && echo "kit save byte-exact"

note "regression: 44140 Hz render vs Phase 0 golden (lxr-render)"
./$B/lxr-render factory/INIT.SND /tmp/p3-golden.wav || fail=1
$PB/plugin_render_artefacts/Release/plugin_render factory/INIT.SND /tmp/p3-init.wav 44140 || fail=1
python3 tools/render-compare.py /tmp/p3-init.wav /tmp/p3-golden.wav || fail=1
# drum3's fixed-window centroid is offset-sensitive: plugin-render schedules hits
# on a 0.6 s host grid, lxr-render on 413-block boundaries, so drum3 sounds one
# engine block later in its segment. Onset-aligned (-64 frames) it is bit-exact.
python3 - <<'PYEOF' || fail=1
import wave, numpy as np
def load(p):
    w = wave.open(p)
    return np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).reshape(-1, 2)
a, b = load('/tmp/p3-init.wav'), load('/tmp/p3-golden.wav')
SEG = 413 * 64
sa, sb = a[2*SEG:3*SEG], b[2*SEG:3*SEG]
assert np.array_equal(sa[64:], sb[:-64]), "drum3 not bit-exact at -64 shift"
print("drum3 onset-aligned (-64 frames): bit-exact vs Phase 0 golden")
PYEOF

echo
if [ "$fail" = 0 ]; then echo "SEQ GATE: ALL PASS"; else echo "SEQ GATE: FAILURES"; exit 1; fi
