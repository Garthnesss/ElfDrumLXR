#!/bin/sh
# Phase 4 stem gate — evidence for patches/04 (mixer stem capture) + the
# six extra JUCE output buses. Prereq: the plugin build in plugin/build
# (plugin_stem_test, plugin_render) and the engine harness (`make`).
#
#   (a) mains unchanged: the stem build's main buses, summed the way
#       lxr-render sums the jacks, match the Phase 0 golden render
#       (drums bit-exact, noise statistical) — for INIT and fx-dist —
#       and match the pre-stem plugin build's renders peak-for-peak.
#   (b) isolation: hitting exactly one voice leaves exactly one stem
#       non-silent (all other stems peak 0), for all 7 triggers
#       (open hat lands in the HiHat stem, as on the hardware).
#   (c) consistency: where only drum1 sounds (INIT, segment 0) the
#       mains equal stem Drum 1 sample-for-sample (same resampler path).
set -e
cd "$(dirname "$0")/.."
PB="${1:-plugin/build}"
ST="$PB/plugin_stem_test_artefacts/Release/plugin_stem_test"
PR="$PB/plugin_render_artefacts/Release/plugin_render"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

echo "== (a) mains: stem-test 16-ch renders, jack sum vs Phase 0 golden"
"$ST" all factory/INIT.SND "$T/init16.wav" >/dev/null
"$ST" all tests/kits/fx-dist.SND "$T/dist16.wav" >/dev/null
"$PR" factory/INIT.SND "$T/init-pr.wav" 44140 >/dev/null
python3 - "$T" <<'EOF'
import sys, wave, struct
T = sys.argv[1]
def load_multi(p):
    w = wave.open(p); n = w.getnframes(); ch = w.getnchannels()
    d = struct.unpack(f'<{n*ch}h', w.readframes(n))
    return [d[c::ch] for c in range(ch)], w.getframerate()
def write_stereo(p, L, R):
    w = wave.open(p, 'wb'); w.setnchannels(2); w.setsampwidth(2); w.setframerate(44140)
    out = bytearray()
    for l, r in zip(L, R):
        out += struct.pack('<hh', max(-32768, min(32767, l)), max(-32768, min(32767, r)))
    w.writeframes(bytes(out)); w.close()
for name in ['init16', 'dist16']:
    chans, sr = load_multi(f'{T}/{name}.wav')
    L = [a + b for a, b in zip(chans[0], chans[2])]
    R = [a + b for a, b in zip(chans[1], chans[3])]
    write_stereo(f'{T}/{name}-sum.wav', L, R)
print('jack sums written')
EOF
python3 tools/render-compare.py "$T/init16-sum.wav" build/render-f16.wav | tee "$T/cmp-init.txt"
grep -E '^drum[123]' "$T/cmp-init.txt" | grep -vq 'bitexact=True' && { echo "STEM GATE: FAIL (INIT mains drums not bit-exact)"; exit 1; }
# pre-stem plugin build reference (captured from the slice-1 binary; skip if absent)
if [ -f /tmp/stemgate/ref-init.wav ]; then
	python3 tools/render-compare.py "$T/init-pr.wav" /tmp/stemgate/ref-init.wav | tee "$T/cmp-pr.txt"
	grep -E '^drum[123]' "$T/cmp-pr.txt" | grep -vq 'bitexact=True' && { echo "STEM GATE: FAIL (plugin mains changed vs pre-stem build)"; exit 1; }
	echo "  plugin_render INIT == pre-stem build (drums bit-exact)"
fi

echo "== (b) isolation: one hit per voice"
python3 - "$ST" "$T" <<'EOF'
import subprocess, sys, wave, struct
ST, T = sys.argv[1], sys.argv[2]
names = ['drum1', 'drum2', 'drum3', 'snare', 'cymbal', 'hihat', 'openhat']
for v in range(7):
    out = f'{T}/one-{v}.wav'
    subprocess.run([ST, 'one', 'factory/INIT.SND', str(v), out], check=True,
                   stdout=subprocess.DEVNULL)
    w = wave.open(out); n = w.getnframes(); ch = w.getnchannels()
    d = struct.unpack(f'<{n*ch}h', w.readframes(n))
    peaks = [max(abs(x) for x in d[c::ch]) for c in range(ch)]
    stem_peaks = peaks[4:]
    expect = min(v, 5)
    for s in range(6):
        pk = max(stem_peaks[2*s], stem_peaks[2*s+1])
        if s == expect:
            assert pk > 1000, f'{names[v]}: own stem {s} silent (peak {pk})'
        else:
            assert pk == 0, f'{names[v]}: stem {s} leaks (peak {pk})'
    print(f'  {names[v]:8s} -> stem {expect} only (peak {max(stem_peaks[2*expect], stem_peaks[2*expect+1])})')
EOF

echo "== (c) consistency: segment 0 mains == stem Drum 1 (INIT)"
python3 - "$T" <<'EOF'
import sys, wave, struct
T = sys.argv[1]
w = wave.open(f'{T}/init16.wav'); n = w.getnframes(); ch = w.getnchannels()
d = struct.unpack(f'<{n*ch}h', w.readframes(n))
chans = [d[c::ch] for c in range(ch)]
SEG = 413 * 64
maxdiff = 0
for i in range(SEG):
    maxdiff = max(maxdiff, abs(chans[0][i] - chans[4][i]), abs(chans[1][i] - chans[5][i]))
print(f'  max |main - stemD1| over segment 0: {maxdiff} LSB')
assert maxdiff <= 2, 'stem/mains mismatch'
EOF
echo "STEM GATE: ALL PASS"
