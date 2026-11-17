#!/usr/bin/env bash
# soak.sh <instance> <minutes> <density> <distance> <loc> [<loc>...] : stability soak. Sets the npc
# settings, then alternates teleports between the given locations every 90 s (spawn/despawn churn)
# and checks every 30 s that the game is still presenting frames. Restores npc settings at the end.
set -uo pipefail
t=$1 mins=$2 d=$3 dist=$4; shift 4
here=$(cd "$(dirname "$0")" && pwd)
W=$here/../..
I=/home/ajg/Games/aco-dlss-instances/$t
INI="$I/prefix/drive_c/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/games/Assassin's Creed Origins/pharos.ini"
L=$I/out/pharos.log
locs=("$@")
sed -i "/^\[world\]/,\$ s/^npc_density=.*/npc_density=$d/; /^\[world\]/,\$ s/^npc_distance=.*/npc_distance=$dist/" "$INI"
end=$((SECONDS + mins * 60)); i=0; next_tp=$((SECONDS + 90)); status=ok
while ((SECONDS < end)); do
  sleep 30
  last=$(grep -E "perf: [0-9.]+ fps" "$L" | tail -1)
  ts=${last:0:8}; now=$(date -u +%T)
  age=$(( $(date -u -d "$now" +%s) - $(date -u -d "$ts" +%s) ))
  cnt=$(ACO_INST=$t python3 "$here/spawncount.py" 1 2>/dev/null | sed 's/.*spawned=//')
  echo "$(date +%T) spawned=$cnt last-perf-age=${age}s ${last:22:60}"
  if ((age > 20)); then echo "soak: game stopped presenting (last perf line ${age}s old)"; status=hang; break; fi
  if ((SECONDS >= next_tp && ${#locs[@]})); then
    loc=${locs[$((i % ${#locs[@]}))]}; i=$((i + 1))
    "$W/tools/teleport" "$t" "$loc" >/dev/null 2>&1 && echo "  teleported to $loc"
    next_tp=$((SECONDS + 90))
  fi
done
sed -i "/^\[world\]/,\$ s/^npc_density=.*/npc_density=1/; /^\[world\]/,\$ s/^npc_distance=.*/npc_distance=1/" "$INI"
echo "soak: $status"
