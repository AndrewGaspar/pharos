#!/usr/bin/env python3
"""Colour image of consecutive presents from a Frame Generation present trace (<dir>/bands.bin,
presents.csv; `touch <out>/fgtrace-start`), stacked top to bottom with their kind (R real,
G generated) printed, for looking at what generated frames do to small moving things.

usage: fgbands.py <fgtrace dir> <first shot> <count> <out.png> [x0 x1] [scale=1]
"""
import struct
import subprocess
import sys

import numpy as np

d, i0, n, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
x0 = int(sys.argv[5]) if len(sys.argv) > 5 else 0
x1 = int(sys.argv[6]) if len(sys.argv) > 6 else None
sc = int(sys.argv[7]) if len(sys.argv) > 7 else 1
with open(d + "/bands.bin", "rb") as f:
    magic, w, h, cnt, bgra = struct.unpack("<5I", f.read(20))
    f.seek(20 + i0 * w * h * 4)
    raw = np.frombuffer(f.read(n * w * h * 4), np.uint8).reshape(n, h, w, 4)
rgb = raw[..., [2, 1, 0]] if bgra else raw[..., :3]
kind = {}
for line in open(d + "/presents.csv").read().splitlines()[1:]:
    v = line.split(",")
    kind[int(v[7])] = v[0]
rows = []
for k in range(n):
    band = rgb[k, :, x0:x1]
    sep = np.full((3, band.shape[1], 3), 255 if kind.get(i0 + k) == "G" else 0, np.uint8)
    rows += [band, sep]
img = np.concatenate(rows, axis=0)
if sc > 1:
    img = img.repeat(sc, axis=0).repeat(sc, axis=1)
hh, ww = img.shape[:2]
subprocess.run(["magick", "-size", "%dx%d" % (ww, hh), "-depth", "8", "rgb:-", out], input=np.ascontiguousarray(img).tobytes())
print("".join(kind.get(i0 + k, "?") for k in range(n)), "(white separator below G, black below R)")
