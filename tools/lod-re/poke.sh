#!/usr/bin/env bash
# poke.sh <label> <memtool commands (\n-separated)> : screenshot before (x2), apply, screenshot after; print diffs
set -euo pipefail
label=$1 cmds=$2
W=/home/ajg/code/ac-dlss/.claude/worktrees/agent-a205672bea6c9d67f
O=/home/ajg/Games/aco-dlss-instances/t1/out
D=/tmp/claude-1000/-home-ajg-code-ac-dlss/249d0810-7bc6-42cc-9509-5599a9b80e14/scratchpad/lod/shots; mkdir -p $D
shot() { "$W/scripts/aco" shot t1 "$D/$1.png" >/dev/null; }
shot "$label-a"; sleep 1; shot "$label-b"
printf '%b\n' "$cmds" > $O/memtool.txt
for i in $(seq 60); do [ -f $O/memtool.txt ] || break; sleep 1; done
sleep 4; shot "$label-c"
mae() { (magick compare -metric MAE "$D/$1.png" "$D/$2.png" null: 2>&1 || true) | sed -n 's/.*(\(.*\)).*/\1/p'; }
echo "$label: noise(a,b)=$(mae $label-a $label-b) change(b,c)=$(mae $label-b $label-c)"
