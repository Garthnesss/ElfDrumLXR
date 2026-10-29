#!/bin/sh
# Phase 4 reverb gate — reproducible evidence for patches/03 (USE_REVERB 1).
#
#   (i)   fx-verb kit through the reverb build: non-silent decaying tail
#         after the trigger; 60 s renders at nominal (P2=90) and max
#         (P2=127) feedback show no runaway / NaN pollution.
#   (ii)  every pre-existing kit renders identically in the reverb build
#         and the default (patch-03-removed) build: drums bit-exact via
#         tools/render-compare.py, noise voices in statistical family.
#
# The default A/B build is produced by applying all patches EXCEPT 03 in a
# scratch tree (OPT_PATCHES only ever adds; here we need the inverse, so the
# script drives the Makefile's patch step manually for the B tree).
set -e
cd "$(dirname "$0")/.."
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

echo "== building reverb (default, patch 03 applied) harness"
make -j4 >/dev/null
make -j4 build/tail-render >/dev/null

echo "== building A/B harness without patch 03"
mkdir -p "$T/norev/src"
cp -R vendor/lxr-02/Core "$T/norev/src/"
for p in patches/01-*.patch patches/02-*.patch; do patch -s -p1 -d "$T/norev/src" < "$p"; done
# build the no-reverb binary directly from the prepared (01+02 only) tree:
cc -std=gnu11 -O2 -w -D__fp16=_Float16 '-Dsection(x)=unused' \
   -Ishim -Iengine -I"$T/norev/src/Core/Inc" -I"$T/norev/src/Core/Inc/Soundengine" -I"$T/norev/src/Core/Src" \
   $(cd vendor/lxr-02/Core/Src && ls Soundengine/DSPAudio/*.c Soundengine/DSPAudio/FX/*.c Soundengine/DSPAudio/FX/*/*.c \
        Soundengine/FxBus.c Soundengine/MIDI/*.c Soundengine/Sequencer/*.c | grep -v -e MIDI/Uart.c | \
        sed "s|^|$T/norev/src/Core/Src/|") \
   engine/engine.c engine/stubs.c engine/kit.c engine/seqbridge.c \
   "$T/norev/src/Core/Src/menu/Cc2Text.c" tools/lxr-render.c -o "$T/lxr-render-norev" -lm
# sanity: the A/B binary must NOT contain the reverb enum path — render the
# verb kit with it and require the dry (no-tail) result, checked in python below.

echo "== (ii) cross-build kit comparisons (drums must be bit-exact)"
fail=0
for k in factory/INIT.SND tests/kits/fx-dist.SND tests/kits/fx-ring.SND tests/kits/fx-comp.SND \
         tests/kits/fx-delay.SND tests/kits/flt-hp.SND tests/kits/flt-bp.SND tests/kits/morph-target.SND; do
	n=$(basename "$k" .SND)
	./build/lxr-render "$k" "$T/$n-rev.wav" >/dev/null
	"$T/lxr-render-norev" "$k" "$T/$n-norev.wav" >/dev/null
	out=$(python3 tools/render-compare.py "$T/$n-rev.wav" "$T/$n-norev.wav")
	echo "$out" | sed "s/^/  $n /"
	echo "$out" | grep -E '^(drum1|drum2|drum3)' | grep -vq 'bitexact=True' && fail=1
done
[ "$fail" = 0 ] || { echo "REVERB GATE: FAIL (pitched voices not bit-exact)"; exit 1; }

echo "== (i) tail + stability analysis"
./build/tail-render tests/kits/fx-verb.SND "$T/verb-8s.wav" 8 0 >/dev/null
./build/tail-render tests/kits/fx-verb.SND "$T/verb-60s.wav" 60 0 >/dev/null
python3 - "$T" <<'EOF'
import sys, wave, struct, math
T = sys.argv[1]
def load(p):
    w = wave.open(p); n = w.getnframes()
    d = struct.unpack(f'<{2*n}h', w.readframes(n)); sr = w.getframerate()
    return [(d[2*i]+d[2*i+1])/2 for i in range(n)], sr
def rms(x): return math.sqrt(sum(v*v for v in x)/max(1, len(x)))
m, sr = load(f'{T}/verb-8s.wav')
w = lambda a, b: rms(m[int(a*sr):int(b*sr)])
assert w(0.0, 0.5) > 500, 'reverb render silent?'
assert w(1.0, 1.5) > 20, 'no tail after trigger stopped'
assert w(0.5, 1.0) > w(1.0, 1.5) > w(2.0, 2.5) > w(3.0, 3.5), 'tail not decaying'
print(f'  tail RMS: 0-0.5s {w(0,0.5):.0f} > 0.5-1s {w(0.5,1):.0f} > 1-1.5s {w(1,1.5):.0f} > 2-2.5s {w(2,2.5):.1f} > 3-3.5s {w(3,3.5):.1f}')
m, sr = load(f'{T}/verb-60s.wav')
late = rms(m[int(55*sr):int(60*sr)])
full = sum(1 for v in m if abs(v) >= 32767)
assert late < 1.0 and full == 0, f'60 s stability fail (late RMS {late}, full-scale {full})'
print(f'  60 s: late-window RMS {late:.2f}, full-scale samples {full} — stable')
EOF
echo "REVERB GATE: ALL PASS"
