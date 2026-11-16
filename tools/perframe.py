#!/usr/bin/env python3
"""Per-frame counts of dispatches/draws by shader in a multi-frame capture, to find work that
runs only on some frames (e.g. a simulation stepping every Nth frame).

usage: perframe.py <cap-dir> [--all]
Prints compute shaders (and, with --all, pixel/vertex shaders) whose per-frame count varies.
"""
import json
import sys
from collections import Counter, defaultdict

cap = sys.argv[1]
show_all = "--all" in sys.argv
frames = [Counter()]
names = {}
with open(f"{cap}/events.jsonl") as f:
    for line in f:
        try:
            e = json.loads(line)
        except json.JSONDecodeError:
            continue
        op = e.get("op", "")
        if op == "Present":
            frames.append(Counter())
            continue
        if op.startswith("Dispatch"):
            key = ("cs", e.get("cs", "?"))
            frames[-1][key] += 1
            if "args" in e:
                names.setdefault(key, str(e["args"]))
        elif op.startswith("Draw") and show_all:
            frames[-1][("vs", e.get("vs", "?"))] += 1
frames = [fr for fr in frames if fr]
keys = sorted({k for fr in frames for k in fr})
print(f"{len(frames)} frames")
for k in keys:
    counts = [fr.get(k, 0) for fr in frames]
    if len(set(counts)) > 1:
        print(f"{k[0]} {k[1]}  " + " ".join(f"{c:3d}" for c in counts) + f"   e.g. args {names.get(k, '')}")
