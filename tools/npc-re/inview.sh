#!/usr/bin/env bash
# inview.sh <instance> <settle s> <density:distance>... : in-view character load per setting = mean
# skinning-compute thread groups per frame (only drawn characters are skinned), from the geo stats.
set -uo pipefail
t=$1 settle=$2; shift 2
I=/home/ajg/Games/aco-dlss-instances/$t
INI="$I/prefix/drive_c/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/games/Assassin's Creed Origins/aco_dlss.ini"
L=$I/out/aco_dlss.log
touch "$I/out/geo"
for arg in "$@"; do
  d=${arg%%:*} dist=${arg#*:}
  sed -i "/^\[world\]/,\$ s/^npc_density=.*/npc_density=$d/; /^\[world\]/,\$ s/^npc_distance=.*/npc_distance=$dist/" "$INI"
  sleep "$settle"
  g=$(grep "geo:" "$L" | tail -15 | sed 's/.*skinning \([0-9.]*\) dispatches \([0-9]*\) groups.*/\2/' |
      awk '{s+=$1; n++} END {printf "%.0f skinning groups/frame (n=%d)", s/n, n}')
  echo "npc_density=$d npc_distance=$dist: $g"
done
sed -i "/^\[world\]/,\$ s/^npc_density=.*/npc_density=1/; /^\[world\]/,\$ s/^npc_distance=.*/npc_distance=1/" "$INI"
rm -f "$I/out/geo"
