#!/usr/bin/env python3
"""Input-to-frame latency from an injection log + the mod's latency CSVs (all in QPC ns, which is
CLOCK_MONOTONIC_RAW under Wine).

usage: latency.py <out_dir> [threshold_deg]

For every injected mouse motion at time t:
  reaction frame = first TAA pass recorded after t whose camera rotation differs from the camera
                   in use just before t by more than threshold_deg
  record         = CPU time that frame's TAA pass was recorded            - t
  gpu_post       = GPU time that frame's TAA/upscale pass executed        - t
  frame_done     = GPU time all work up to that frame's Present finished  - t
Display scanout/compositor time is not included (headless).
"""
import csv
import math
import statistics
import sys

out = sys.argv[1]
thr = float(sys.argv[2]) if len(sys.argv) > 2 else 0.05


def rows(name):
    with open(f"{out}/{name}") as f:
        return list(csv.DictReader(f))


inj = [int(r["t_send_ns"]) for r in rows("inject.csv")]
all_taa = rows("latency_taa.csv")
taa = [r for r in all_taa if r["have_cam"] == "1"]
if len(taa) < len(all_taa):
    print(f"WARNING: {len(all_taa) - len(taa)} of {len(all_taa)} TAA rows have no camera (reactions may be detected late)")
taa = [(int(r["cpu_record_ns"]), int(r["gpu_ns"]),
        [float(r[k]) for k in ("r00", "r01", "r02", "r10", "r11", "r12", "r20", "r21", "r22")]) for r in taa]
taa.sort()
pres = sorted((int(r["gpu_ns"]), int(r["cpu_present_ns"]), int(r["frame"])) for r in rows("latency_present.csv"))


def angle(a, b):
    # rotation angle between two 3x3 rotations (row-major): acos((tr(A^T B) - 1) / 2)
    tr = sum(a[i] * b[i] for i in range(9))
    return math.degrees(math.acos(max(-1.0, min(1.0, (tr - 1) / 2))))


# Frame-to-frame camera noise when idle (for choosing the threshold).
deltas = sorted(angle(taa[i - 1][2], taa[i][2]) for i in range(1, len(taa)))
if deltas:
    print(f"camera frame-to-frame delta: median {deltas[len(deltas)//2]:.4f} deg, "
          f"p90 {deltas[int(len(deltas)*0.9)]:.4f} deg, max {deltas[-1]:.3f} deg; threshold {thr} deg")

res = {"record": [], "gpu_post": [], "frame_done": [], "present_call": []}
misses = 0
for t in inj:
    before = [r for r in taa if r[0] < t]
    if not before:
        misses += 1
        continue
    base = before[-1][2]
    react = next((r for r in taa if r[0] >= t and angle(base, r[2]) > thr), None)
    if react is None or react[0] - t > 500e6:
        misses += 1
        continue
    rec, gpu, _ = react
    done = next((p for p in pres if p[0] >= gpu), None)
    res["record"].append((rec - t) / 1e6)
    res["gpu_post"].append((gpu - t) / 1e6)
    if done:
        res["frame_done"].append((done[0] - t) / 1e6)
        res["present_call"].append((done[1] - t) / 1e6)

if len(pres) > 1:
    span = (pres[-1][1] - pres[0][1]) / 1e9
    print(f"frames: {len(pres)} presents over {span:.1f} s = {len(pres)/span:.1f} fps")
print(f"injections: {len(inj)}, reactions found: {len(res['record'])}, misses: {misses}")
print(f"{'stage (ms after input)':<26}{'median':>8}{'mean':>8}{'p10':>8}{'p90':>8}")
labels = {"record": "TAA recorded (CPU)", "present_call": "Present() called (CPU)",
          "gpu_post": "TAA/DLSS ran (GPU)", "frame_done": "frame done (GPU)"}
for k in ("record", "present_call", "gpu_post", "frame_done"):
    v = sorted(res[k])
    if not v:
        continue
    q = lambda p: v[min(len(v) - 1, int(p * len(v)))]
    print(f"{labels[k]:<26}{statistics.median(v):8.1f}{statistics.mean(v):8.1f}{q(0.1):8.1f}{q(0.9):8.1f}")
