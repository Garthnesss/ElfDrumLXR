#!/usr/bin/env python3
"""render-compare: per-segment peak / spectral-centroid comparison of two lxr-render-style
WAVs (6 hits, 413 blocks of 64 frames each = 26432 frames per segment).
Usage: render-compare.py OURS.wav REF.wav
Pitched voices should be bit-exact; noise voices are judged statistically
(centroid within ~2%, peak within the max-statistic noise band ~15%)."""
import sys, wave
import numpy as np

SEG = 413 * 64
NAMES = ['drum1', 'drum2', 'drum3', 'snare', 'cymbal', 'hihat']

def load(p):
    w = wave.open(p)
    return np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).reshape(-1, 2).astype(np.float64)

def stats(seg):
    mono = seg.mean(axis=1)
    mag = np.abs(np.fft.rfft(mono * np.hanning(len(mono))))
    freqs = np.fft.rfftfreq(len(mono), 1 / 44100)
    return np.abs(seg).max(), (freqs * mag).sum() / mag.sum()

a, b = load(sys.argv[1]), load(sys.argv[2])
for v in range(6):
    sa, sb = a[v*SEG:(v+1)*SEG], b[v*SEG:(v+1)*SEG]
    pa, ca = stats(sa); pb, cb = stats(sb)
    print(f'{NAMES[v]:7s} peak {pa:6.0f} vs {pb:6.0f} ({(pa-pb)/pb*100:+5.1f}%)  '
          f'cent {ca:7.0f} vs {cb:7.0f} ({(ca-cb)/cb*100:+5.1f}%)  bitexact={np.array_equal(sa, sb)}')
