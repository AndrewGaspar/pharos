#!/usr/bin/env python3
"""Pass-level view of a capture: groups consecutive draws/dispatches that write the same targets.

usage: passes.py <cap-dir> [--frame N]
"""
import argparse
import json
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import capsum  # noqa: E402


def load_events(cap):
    out, bad = [], 0
    for l in open(Path(cap) / "events.jsonl"):
        try:
            out.append(json.loads(l))
        except json.JSONDecodeError:
            bad += 1
    if bad:
        print(f"warning: {bad} unparseable events skipped", file=sys.stderr)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cap")
    ap.add_argument("--frame", type=int, default=1)
    args = ap.parse_args()
    events = load_events(args.cap)
    frames, cur = [], []
    for e in events:
        if e["op"] == "Present":
            frames.append(cur)
            cur = []
        else:
            cur.append(e)
    fr = frames[1:][args.frame]
    passes = [e for e in fr if e["op"].startswith(("Draw", "Dispatch"))]
    cnt = Counter(v["r"] for e in passes for v in (e.get("srv") or {}).values())
    capsum.GLOBAL.update(r for r, c in cnt.items() if c > 0.5 * len(passes))

    def outputs(e):
        if e["op"].startswith("Draw"):
            o = [v for _, v in sorted((e.get("rt") or {}).items())]
            if e.get("ds"):
                o.append(e["ds"])
            return o
        return [v for _, v in sorted((e.get("uav") or {}).items())]

    groups = []
    for i, e in enumerate(fr):
        if not e["op"].startswith(("Draw", "Dispatch")):
            if e["op"].startswith(("Copy", "Clear")):
                groups.append({"i": i, "kind": e["op"], "e": e})
            continue
        key = tuple(v["r"] for v in outputs(e))
        g = groups[-1] if groups else None
        if g and g.get("key") == key:
            g["n"] += 1
            g["shaders"].update([e.get("ps") or e.get("cs")])
            for k, v in (e.get("srv") or {}).items():
                if v["r"] not in capsum.GLOBAL:
                    g["srv"][v["r"]] = v
        else:
            groups.append({"i": i, "kind": "Disp" if e["op"].startswith("Disp") else "Draw", "key": key, "n": 1,
                           "out": outputs(e), "shaders": Counter([e.get("ps") or e.get("cs")]),
                           "srv": {v["r"]: v for v in (e.get("srv") or {}).values() if v["r"] not in capsum.GLOBAL},
                           "vp": e.get("vp")})
    for g in groups:
        if "key" not in g:
            e = g["e"]
            if g["kind"].startswith("Copy"):
                print(f"{g['i']:5d} {g['kind'][:8]:8s} {capsum.tex(e['src'])} -> {capsum.tex(e['dst'])}")
            else:
                print(f"{g['i']:5d} {g['kind'][:8]:8s} {capsum.tex(e.get('view'))}")
            continue
        sh = ",".join(f"{s[:8] if s else '-'}" + (f"x{c}" if c > 1 else "") for s, c in g["shaders"].most_common(4))
        if len(g["shaders"]) > 4:
            sh += f",+{len(g['shaders'])-4}"
        outs = " ".join(capsum.tex(o) for o in g["out"])
        ins = " ".join(capsum.tex(v) for v in list(g["srv"].values())[:8])
        print(f"{g['i']:5d} {g['kind']} n={g['n']:<4d} [{sh}] -> {outs}  <- {ins}")


if __name__ == "__main__":
    main()
