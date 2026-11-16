#!/usr/bin/env bash
# abworld.sh <label> <key> <value> : screenshot, set [world] key=value live in t1, screenshot; report diff + geo
set -euo pipefail
label=$1 key=$2 val=$3
W=/home/ajg/code/ac-dlss/.claude/worktrees/agent-a205672bea6c9d67f
I=/home/ajg/Games/aco-dlss-instances/t1
INI="$I/prefix/drive_c/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/games/Assassin's Creed Origins/aco_dlss.ini"
D=/tmp/claude-1000/-home-ajg-code-ac-dlss/249d0810-7bc6-42cc-9509-5599a9b80e14/scratchpad/lod/shots; mkdir -p $D
shot() { "$W/scripts/aco" shot t1 "$D/$1.png" >/dev/null; }
mae() { (magick compare -metric MAE "$D/$1.png" "$D/$2.png" null: 2>&1 || true) | sed -n 's/.*(\(.*\)).*/\1/p'; }
shot "$label-a"; sleep 1; shot "$label-b"
sed -i "/^\[world\]/,\$ s/^$key=.*/$key=$val/" "$INI"
sleep 5; shot "$label-c"
echo "$label: noise(a,b)=$(mae $label-a $label-b) change(b,c)=$(mae $label-b $label-c)"
grep -E "world:|geo:" $I/out/aco_dlss.log | tail -2 | cut -c1-200
