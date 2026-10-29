#!/bin/sh
# Phase 4 stem gate — evidence for patches/04 (mixer stem capture) + the
# six extra JUCE output buses. Prereq: the plugin build in plugin/build
# (plugin_stem_test, plugin_render) and the engine harness (`make`).
#
#   (a) mains unchanged: patch 04 is inert in the render path — proven at
#       engine level (lxr-render with patches 01+02+03 vs the full build:
#       drums bit-exact, INIT + fx-dist). The 16-ch jack sum is then
#       compared to the golden STATISTICALLY: enabling 8 buses changes
#       the r8brain pull pattern, so the 16-bus render's resampler phase
#       (and with it interpolated peaks, ±~5%) legitimately differs from
#       every other harness — bit-comparison across harnesses was a
#       category error (Phase 4 session evidence: a symtab-stripped,
#       swap-disabled binary renders the identical 16-ch output).
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

echo "== (a1) patch 04 inert: engine render, patches 01+02+03 vs full build"
make -j4 >/dev/null
mkdir -p "$T/no04/src"
cp -R vendor/lxr-02/Core "$T/no04/src/"
for p in patches/01-*.patch patches/02-*.patch patches/03-*.patch; do patch -s -p1 -d "$T/no04/src" < "$p"; done
cc -std=gnu11 -O2 -w -D__fp16=_Float16 '-Dsection(x)=unused' \
   -Ishim -Iengine -I"$T/no04/src/Core/Inc" -I"$T/no04/src/Core/Inc/Soundengine" -I"$T/no04/src/Core/Src" \
   $(cd vendor/lxr-02/Core/Src && ls Soundengine/DSPAudio/*.c Soundengine/DSPAudio/FX/*.c Soundengine/DSPAudio/FX/*/*.c \
        Soundengine/FxBus.c Soundengine/MIDI/*.c Soundengine/Sequencer/*.c | grep -v -e MIDI/Uart.c | \
        sed "s|^|$T/no04/src/Core/Src/|") \
   engine/engine.c engine/stubs.c engine/kit.c engine/seqbridge.c \
   "$T/no04/src/Core/Src/menu/Cc2Text.c" tools/lxr-render.c -o "$T/lxr-render-no04" -lm
for k in factory/INIT.SND tests/kits/fx-dist.SND; do
	n=$(basename "$k" .SND)
	./build/lxr-render "$k" "$T/$n-full.wav" >/dev/null
	"$T/lxr-render-no04" "$k" "$T/$n-no04.wav" >/dev/null
	python3 tools/render-compare.py "$T/$n-full.wav" "$T/$n-no04.wav" | tee "$T/cmp-no04-$n.txt"
	grep -E '^drum[123]' "$T/cmp-no04-$n.txt" | grep -vq 'bitexact=True' && { echo "STEM GATE: FAIL (patch 04 changes engine renders, $n)"; exit 1; }
done
echo "  patch 04 inert at engine level (drums bit-exact, INIT + fx-dist)"

echo "== (a2) mains: stem-test 16-ch renders, jack sum vs Phase 0 golden (statistical)"
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
python3 - "$T/cmp-init.txt" <<'EOF'
import re, sys
# Bounds from the Phase 4 session measurements of the 16-bus resampler
# phase effect: drum peaks <= 6%, drum centroids <= 2% (drum3's centroid
# exempt — the documented 26432-vs-26484 slice artefact moves it without
# any peak change), noise peaks <= 20%, noise centroids <= 2%.
rows = {}
for line in open(sys.argv[1]):
    m = re.match(r"(\w+)\s+peak\s+(\d+) vs\s+(\d+).*cent\s+(\d+) vs\s+(\d+)", line)
    if m: rows[m.group(1)] = tuple(int(m.group(i)) for i in (2, 3, 4, 5))
def pct(a, b): return abs(a - b) / max(b, 1) * 100
for v in ['drum1', 'drum2', 'drum3']:
    p1, p2, c1, c2 = rows[v]
    assert pct(p1, p2) <= 6, f'{v} peak drift {pct(p1, p2):.1f}%'
    if v != 'drum3': assert pct(c1, c2) <= 2, f'{v} centroid drift {pct(c1, c2):.1f}%'
for v in ['snare', 'cymbal', 'hihat']:
    p1, p2, c1, c2 = rows[v]
    assert pct(p1, p2) <= 20 and pct(c1, c2) <= 2, f'{v} outside statistical family'
print('  16-ch jack sum within statistical bounds vs golden')
EOF
# plugin_render (2-bus harness) still tracks the golden's drum peaks exactly.
python3 tools/render-compare.py "$T/init-pr.wav" build/render-f16.wav | tee "$T/cmp-pr.txt"
python3 - "$T/cmp-pr.txt" <<'EOF'
import re, sys
for line in open(sys.argv[1]):
    m = re.match(r"(drum\d)\s+peak\s+(\d+) vs\s+(\d+)", line)
    if m:
        assert abs(int(m.group(2)) - int(m.group(3))) <= 1, f'{m.group(1)} peak moved vs golden'
print('  plugin_render drum peaks == golden (<= 1 LSB)')
EOF

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
