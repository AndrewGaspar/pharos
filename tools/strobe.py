#!/usr/bin/env python3
"""Temporal flicker ("strobing") in a framedump (<out>/frames/NNNN.ppm, `echo N > <out>/framedump`).

usage: strobe.py <frames_dir> [threshold=24] [strip.png]

Frames from a moving camera (a Senu flight) are first aligned to each other by their global shift
(phase correlation on half-resolution luma); what is left after alignment is local change.

Luma per pixel (2x2 averaged). A pixel "flips" at frame t when it changes by more than the threshold
from t-1 to t and changes back by more than the threshold from t to t+1 (A -> B -> A): what a
shadow or highlight popping on and off looks like; steady motion (a walker, a camera pan) changes
monotonically and does not count. Prints flips per megapixel per frame and the share of pixels
that flipped at least 3 times, overall and in a 4x4 grid. With a third argument, writes a strip of
6 consecutive crops (the 1/8-width cell with the most flips) for a look.
"""
import glob
import os
import sys

import numpy as np


def load_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return np.frombuffer(parts[3], dtype=np.uint8, count=w * h * 3).reshape(h, w, 3)


d = sys.argv[1]
T = float(sys.argv[2]) if len(sys.argv) > 2 else 24
files = sorted(glob.glob(os.path.join(d, "*.ppm")))
if len(files) < 4:
    sys.exit("need at least 4 frames")
rgb = [load_ppm(f) for f in files]
lum = []
for im in rgb:
    y = im[..., 0] * 0.299 + im[..., 1] * 0.587 + im[..., 2] * 0.114
    h, w = y.shape
    y = y[: h // 2 * 2, : w // 2 * 2].reshape(h // 2, 2, w // 2, 2).mean(axis=(1, 3))
    lum.append(y.astype(np.float32))
L = np.stack(lum)


def shift_between(a, b):
    """(dy, dx) that moves b onto a (phase correlation, integer)."""
    h, w = a.shape
    ca, cb = a[h // 4: 3 * h // 4, w // 4: 3 * w // 4], b[h // 4: 3 * h // 4, w // 4: 3 * w // 4]
    fa, fb = np.fft.fft2(ca - ca.mean()), np.fft.fft2(cb - cb.mean())
    r = fa * np.conj(fb)
    r /= np.abs(r) + 1e-9
    c = np.abs(np.fft.ifft2(r))
    dy, dx = np.unravel_index(np.argmax(c), c.shape)
    hh, ww = c.shape
    return (dy - hh if dy > hh // 2 else dy), (dx - ww if dx > ww // 2 else dx)


pos = [(0, 0)]
for t in range(1, len(L)):
    dy, dx = shift_between(L[t - 1], L[t])
    pos.append((pos[-1][0] + dy, pos[-1][1] + dx))
pys, pxs = [p[0] for p in pos], [p[1] for p in pos]
H0, W0 = L.shape[1:]
y0, y1 = max(pys) - min(pys), H0
x0, x1 = max(pxs) - min(pxs), W0
moved = (y0, x0) != (0, 0)
if moved and (y1 - y0 < H0 // 3 or x1 - x0 < W0 // 3):
    print("camera moved too far over the sequence (%d, %d px); using the first %d frames" % (y0, x0, len(L)))
if moved:
    al = []
    for t in range(len(L)):
        oy, ox = max(pys) - pys[t], max(pxs) - pxs[t]
        al.append(L[t][oy: oy + H0 - y0, ox: ox + W0 - x0])
    L = np.stack(al)
    print("aligned: total shift %d, %d px (half resolution)" % (y0, x0))
D = np.diff(L, axis=0)
flip = (np.abs(D[:-1]) > T) & (np.abs(D[1:]) > T) & (np.sign(D[:-1]) != np.sign(D[1:]))
per_px = flip.sum(axis=0)
n_t = flip.shape[0]
mp = per_px.size / 1e6 * 4  # in full-resolution megapixels
print("%d frames, %dx%d, threshold %g" % (len(files), rgb[0].shape[1], rgb[0].shape[0], T))
print("flips per MP per frame: %.0f; pixels flipping >= 3 times: %.3f%%; mean change %.2f" %
      (flip.sum() / mp / n_t, 100.0 * (per_px >= 3).mean(), np.abs(D).mean()))
H, W = per_px.shape
g = per_px[: H // 4 * 4, : W // 4 * 4].reshape(4, H // 4, 4, W // 4).sum(axis=(1, 3)) / (H * W / 16 / 1e6 * 4) / n_t
print("grid (flips per MP per frame, top row first):")
for r in g:
    print("  " + " ".join("%7.0f" % v for v in r))
if len(sys.argv) > 3:
    cw, ch = W // 8, H // 8
    best, bi = -1, (0, 0)
    for y in range(0, H - ch + 1, ch // 2):
        for x in range(0, W - cw + 1, cw // 2):
            s = per_px[y:y + ch, x:x + cw].sum()
            if s > best:
                best, bi = s, (y, x)
    t0 = int(np.argmax(flip[:, bi[0]:bi[0] + ch, bi[1]:bi[1] + cw].sum(axis=(1, 2))))
    t0 = max(0, min(t0, len(rgb) - 6))
    crops = []
    for t in range(t0, t0 + 6):
        oy, ox = (max(pys) - pys[t], max(pxs) - pxs[t]) if moved else (0, 0)
        y, x = (bi[0] + oy) * 2, (bi[1] + ox) * 2
        crops.append(rgb[t][y:y + ch * 2, x:x + cw * 2])
    strip = np.concatenate(crops, axis=1)
    strip = strip.repeat(2, axis=0).repeat(2, axis=1)
    hh, ww = strip.shape[:2]
    with open(sys.argv[3] + ".ppm", "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (ww, hh))
        f.write(np.ascontiguousarray(strip).tobytes())
    os.system("magick '%s.ppm' '%s' && rm '%s.ppm'" % (sys.argv[3], sys.argv[3], sys.argv[3]))
    print("strip: frames %d-%d, crop x=%d y=%d %dx%d -> %s" % (t0, t0 + 5, x, y, cw * 2, ch * 2, sys.argv[3]))
