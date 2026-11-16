#!/usr/bin/env python3
"""Frame-to-frame change in image regions across a framedump (<out>/frames/NNNN.ppm).

usage: framediff.py <frames_dir> name=x,y,w,h [name=x,y,w,h ...]

Prints per-frame mean absolute difference (0-255) to the previous frame for each region, plus
the frame interval, and a summary of how often each region "moved" (diff above a threshold
relative to its median) to expose periodic updates (e.g. cloth stepping every Nth frame).
"""
import sys

import numpy as np


def load_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # header: P6\n<w> <h>\n255\n
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return np.frombuffer(parts[3], dtype=np.uint8, count=w * h * 3).reshape(h, w, 3).astype(np.int16)


d = sys.argv[1]
regions = []
for arg in sys.argv[2:]:
    name, geo = arg.split("=")
    x, y, w, h = map(int, geo.split(","))
    regions.append((name, x, y, w, h))
times = {}
for line in open(f"{d}/times.txt"):
    i, t = line.split()
    times[int(i)] = int(t)
n = len(times)
prev = None
series = {r[0]: [] for r in regions}
print("frame  dt_ms  " + "  ".join(f"{r[0]:>9}" for r in regions))
for i in range(n):
    img = load_ppm(f"{d}/{i:04d}.ppm")
    if prev is not None:
        vals = []
        for name, x, y, w, h in regions:
            v = float(np.abs(img[y:y + h, x:x + w] - prev[y:y + h, x:x + w]).mean())
            series[name].append(v)
            vals.append(v)
        print(f"{i:5d} {(times[i] - times[i - 1]) / 1e6:6.2f}  " + "  ".join(f"{v:9.3f}" for v in vals))
    prev = img
for name, s in series.items():
    s = np.array(s)
    med = np.median(s)
    moved = s > 2 * med
    idx = np.nonzero(moved)[0] + 1
    gaps = np.diff(idx)
    print(f"{name}: median {med:.3f}, frames above 2x median: {moved.sum()}/{len(s)}, "
          f"gap histogram {dict(zip(*np.unique(gaps, return_counts=True))) if len(gaps) else {}}")
