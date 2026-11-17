#!/usr/bin/env bash
# capsweep.sh <instance> <settle s> <density:distance>... : live sweep of [world] npc_density /
# npc_distance (written to the instance's pharos.ini; reloaded live), reporting per step the
# spawned count (mean of the mod's 10 s log lines over the last half of the settle time) and the
# in-view skinning load. Leaves the last setting in place.
set -uo pipefail
t=$1 settle=$2; shift 2
I=/home/ajg/Games/aco-dlss-instances/$t
INI="$I/prefix/drive_c/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/games/Assassin's Creed Origins/pharos.ini"
L=$I/out/pharos.log
for arg in "$@"; do
  d=${arg%%:*} dist=${arg#*:}
  sed -i "/^\[world\]/,\$ s/^npc_density=.*/npc_density=$d/; /^\[world\]/,\$ s/^npc_distance=.*/npc_distance=$dist/" "$INI"
  sleep $((settle / 2))
  n0=$(wc -l < "$L")
  touch "$I/out/geo"
  sleep $((settle - settle / 2))
  rm -f "$I/out/geo"
  tail -n +"$((n0 + 1))" "$L" > /tmp/capsweep.$$
  sp=$(grep 'npc: [0-9]* spawned' /tmp/capsweep.$$ | sed 's/.*npc: \([0-9]*\) spawned, maxSpawned \([0-9]*\).*/\1 \2/' |
       awk '{s+=$1; n++; c=$2} END {if (n) printf "%.0f spawned (cap %d)", s/n, c}')
  g=$(grep 'geo:' /tmp/capsweep.$$ | sed 's/.*dispatches \([0-9]*\) groups.*/\1/' | awk '{s+=$1; n++} END {if (n) printf "%.0f", s/n}')
  echo "npc_density=$d npc_distance=$dist: $sp, in view $g groups/frame"
  ACO_INST=$t python3 "$(dirname "$0")/bands.py" | sed 's/^/  /'

  rm -f /tmp/capsweep.$$
done
