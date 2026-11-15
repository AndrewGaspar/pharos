#!/usr/bin/env python3
"""Summarize an aco-dlss frame capture (events.jsonl).

usage: capsum.py <cap-dir> [--frame N] [--full] [--res WxH] [--grep HASH]

Prints one line per draw/dispatch/copy/clear of the chosen frame (default: the second complete
frame), with shader hashes and the bound textures as WxH:fmt@id, where `id` is a short stable alias
for the resource pointer so data flow between passes is easy to follow.
"""
import argparse
import json
import sys
from pathlib import Path

DXGI = {
    2: "RGBA32F", 10: "RGBA16F", 11: "RGBA16", 13: "RGBA16S", 16: "RG32F", 24: "RGB10A2", 26: "R11G11B10F",
    28: "RGBA8", 29: "RGBA8_SRGB", 34: "RG16F", 35: "RG16", 37: "RG16S", 39: "R32_TL", 40: "D32F", 41: "R32F",
    42: "R32U", 43: "R32I", 44: "R24G8_TL", 45: "D24S8", 46: "R24X8", 49: "RG8", 54: "R16F", 55: "D16",
    56: "R16", 61: "R8", 62: "R8U", 65: "A8", 71: "BC1", 74: "BC2", 77: "BC3", 80: "BC4", 83: "BC5",
    87: "BGRA8", 91: "BGRA8_SRGB", 95: "BC6H_UF", 98: "BC7", 99: "BC7_SRGB", 19: "R32G8X24_TL", 20: "D32S8",
    21: "R32X8X24", 1: "RGBA32_TL", 9: "RGBA16_TL", 27: "RGBA8_TL", 33: "RG16_TL", 38: "RGB10A2_TL",
    15: "RG32_TL", 47: "X24G8", 48: "RG8_TL", 53: "R16_TL", 60: "R8_TL", 23: "RGB10A2_TL", 0: "-",
}

aliases = {}


def alias(ptr):
    if ptr not in aliases:
        aliases[ptr] = f"r{len(aliases)}"
    return aliases[ptr]


def tex(d):
    if d is None:
        return "?"
    a = alias(d["r"])
    t = d.get("t")
    fmt = DXGI.get(d.get("vfmt") or d.get("fmt"), str(d.get("vfmt") or d.get("fmt")))
    if t == "2d":
        s = f"{d['w']}x{d['h']}:{fmt}"
        if d.get("arr", 1) > 1:
            s += f"[{d['arr']}]"
        if d.get("mip", 0) not in (0, -1, None):
            s += f"m{d['mip']}"
    elif t == "3d":
        s = f"{d['w']}x{d['h']}x{d['d']}:{fmt}"
    elif t == "buf":
        s = f"buf{d['size']}"
    else:
        s = t or "?"
    if d.get("name"):
        s += f"'{d['name']}'"
    return f"{s}@{a}"


GLOBAL = set()  # resources bound in most passes (engine-wide texture tables); hidden from output


def binds(m):
    return " ".join(f"{k}={tex(v)}" for k, v in sorted((m or {}).items(), key=lambda kv: int(kv[0]))
                    if v.get("r") not in GLOBAL)


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
    ap.add_argument("--frame", type=int, default=1, help="0-based index of complete frame")
    ap.add_argument("--res", help="only show passes writing this resolution, e.g. 2560x1600")
    ap.add_argument("--grep", help="only lines mentioning this text")
    args = ap.parse_args()

    events = load_events(args.cap)
    frames, cur = [], []
    for e in events:
        if e["op"] == "Present":
            frames.append(cur)
            cur = []
        else:
            cur.append(e)
    # frames[0] is partial (capture armed mid-frame)
    complete = frames[1:]
    print(f"{len(events)} events, {len(complete)} complete frames", file=sys.stderr)
    fr = complete[args.frame]
    from collections import Counter
    passes = [e for e in fr if e["op"].startswith(("Draw", "Dispatch"))]
    cnt = Counter(v["r"] for e in passes for v in (e.get("srv") or {}).values())
    GLOBAL.update(r for r, c in cnt.items() if c > 0.5 * len(passes))
    print(f"hiding {len(GLOBAL)} globally-bound resources", file=sys.stderr)
    prev, rep = None, 0
    out = []
    for i, e in enumerate(fr):
        op = e["op"]
        cl = "*" if e.get("in_cl") else " "
        if op.startswith("Draw"):
            line = f"{i:5d}{cl}{op[:6]:6s} vs={e.get('vs','-')[:8]} ps={e.get('ps','-')[:8]} rt[{binds(e.get('rt'))}]"
            if e.get("ds"):
                line += f" ds={tex(e['ds'])}"
            line += f" srv[{binds(e.get('srv'))}]"
            if e.get("vp"):
                line += f" vp={int(e['vp'][2])}x{int(e['vp'][3])}"
        elif op.startswith("Dispatch"):
            line = f"{i:5d}{cl}{'Disp':6s} cs={e.get('cs','-')[:8]} args={e.get('args')} uav[{binds(e.get('uav'))}] srv[{binds(e.get('srv'))}]"
        elif op.startswith("Copy"):
            line = f"{i:5d}{cl}{op[:10]:6s} {tex(e['src'])} -> {tex(e['dst'])}"
        elif op.startswith("Clear"):
            line = f"{i:5d}{cl}{op:6s} {tex(e.get('view'))}"
        else:
            line = f"{i:5d}{cl}{op}"
        if args.res and args.res not in line:
            continue
        if args.grep and args.grep not in line:
            continue
        key = line[5:]
        if key == prev:
            rep += 1
            continue
        if rep:
            out.append(f"      ... x{rep + 1}")
        prev, rep = key, 0
        out.append(line)
    if rep:
        out.append(f"      ... x{rep + 1}")
    print("\n".join(out))


if __name__ == "__main__":
    main()
