#!/usr/bin/env python3
"""Flicker between presented frames in a Frame Generation present trace (`touch <out>/fgtrace-start`;
<dir>/presents.csv + bands.bin: a 1/16-height band at y = 9/32 of every one of the first
[fg] trace_shots presents, real and generated, in present order).

usage: fgstrobe.py <fgtrace dir> [threshold=24] [strip.png]

For a still camera (Senu hovering): a pixel "spikes" in a present when it is brighter (or darker)
than both the present before and the one after by more than the threshold: on-off-on at the
present rate, what a shadow or a thin detail strobing looks like. Counted separately for generated
(G) and real (R) presents, per megapixel. If generated frames mis-place small moving things (people
and their shadows), G spikes far exceed R spikes. With a third argument, writes 6 consecutive
presents (R/G labelled in the console) of the 256-px column window with the most G spikes.
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import importlib.util

spec = importlib.util.spec_from_file_location("fgp", os.path.join(os.path.dirname(os.path.abspath(__file__)), "fg-pacing.py"))
fgp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fgp)

d = sys.argv[1]
T = float(sys.argv[2]) if len(sys.argv) > 2 else 24
rows, bands = fgp.load(d)
if bands is None:
    sys.exit("no bands.bin")
n = bands.shape[0]
kinds = []
for r in rows:
    if r["shot"] >= 0 and r["shot"] < n:
        kinds.append((r["shot"], r["kind"]))
kinds.sort()
kind = ["?"] * n
for s, k in kinds:
    kind[s] = k
B = bands
up = (B[1:-1] - B[:-2] > T) & (B[1:-1] - B[2:] > T)
dn = (B[:-2] - B[1:-1] > T) & (B[2:] - B[1:-1] > T)
spike = up | dn
mp = B.shape[1] * B.shape[2] / 1e6
per = {"G": [], "R": [], "N": []}
for i in range(1, n - 1):
    per.setdefault(kind[i], []).append(spike[i - 1].sum() / mp)
print("%d presents (%s), band %dx%d, threshold %g" % (n, "".join(kind[:24]) + "...", B.shape[2], B.shape[1], T))
for k in ("R", "G", "N"):
    v = per.get(k, [])
    if v:
        print("  %s: %3d presents, spikes per MP: mean %.0f, median %.0f, max %.0f" % (k, len(v), np.mean(v), np.median(v), np.max(v)))
mean_change = np.abs(np.diff(B, axis=0)).mean()
print("  mean change between presents %.2f" % mean_change)
if len(sys.argv) > 3:
    W = B.shape[2]
    gmask = np.array([kind[i] == "G" for i in range(1, n - 1)])
    colsum = spike[gmask].sum(axis=(0, 1)) if gmask.any() else spike.sum(axis=(0, 1))
    win = 256
    c = np.convolve(colsum, np.ones(win), mode="valid")
    x0 = int(np.argmax(c))
    gi = [i for i in range(1, n - 1) if kind[i] == "G"]
    i0 = max(0, (gi[len(gi) // 2] if gi else n // 2) - 2)
    i0 = min(i0, n - 6)
    # raw colour strip from bands.bin is luma only here: write luma, upscaled 2x
    strip = np.concatenate([B[i, :, x0:x0 + win] for i in range(i0, i0 + 6)], axis=1)
    strip = np.clip(strip, 0, 255).astype(np.uint8).repeat(2, axis=0).repeat(2, axis=1)
    h, w = strip.shape
    with open(sys.argv[3] + ".pgm", "wb") as f:
        f.write(b"P5\n%d %d\n255\n" % (w, h))
        f.write(strip.tobytes())
    os.system("magick '%s.pgm' '%s' && rm '%s.pgm'" % (sys.argv[3], sys.argv[3], sys.argv[3]))
    print("strip: presents %d-%d (%s), columns %d-%d -> %s" % (i0, i0 + 5, "".join(kind[i0:i0 + 6]), x0, x0 + win, sys.argv[3]))
