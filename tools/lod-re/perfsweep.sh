#!/usr/bin/env bash
# perfsweep.sh <values...> : set [world] lod_distance live in t1 for each value, report fps + upscale gpu (avg of 3 reports)
set -uo pipefail
I=/home/ajg/Games/aco-dlss-instances/t1
INI="$I/prefix/drive_c/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/games/Assassin's Creed Origins/pharos.ini"
L=$I/out/pharos.log
for v in "$@"; do
  sed -i "/^\[world\]/,\$ s/^lod_distance=.*/lod_distance=$v/" "$INI"
  sleep 14
  fps=$(grep -E "perf: [0-9.]+ fps" $L | tail -5 | sed 's/.*perf: \([0-9.]*\) fps.*/\1/' | awk '{s+=$1} END {printf "%.1f", s/NR}')
  echo "lod_distance=$v: $fps fps"
done
sed -i "/^\[world\]/,\$ s/^lod_distance=.*/lod_distance=1/" "$INI"
