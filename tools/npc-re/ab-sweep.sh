#!/usr/bin/env bash
# ab-sweep.sh <instance> <mod dir> <settle s> <k> <density:distance>... : fresh launch of the story
# fixture with [world] npc_units=k, the same hop as ab-units.sh (180 m west for 30 s and back), then
# capsweep.sh over the given npc_density:npc_distance steps (spawned count + in-view load per step).
set -uo pipefail
t=$1 mod=$2 settle=$3 k=$4; shift 4
W=$(cd "$(dirname "$0")/../.." && pwd)
sed -i "s/^npc_units=.*/npc_units=$k/" "$mod/pharos.ini"
if "$W/tools/npc-re/userplaying.sh"; then echo "not launching: the user is playing"; exit 2; fi
"$W/scripts/aco" kill-game "$t" >/dev/null 2>&1
"$W/scripts/aco" fixture "$t" story-alexandria-port >/dev/null 2>&1
timeout 180 "$W/scripts/aco" game "$t" --mod "$mod" --fps 30 >/dev/null 2>&1 || { echo "k=$k: launch failed"; exit 1; }
timeout 420 "$W/scripts/aco-dt" "$t" --story >/dev/null 2>&1 || { echo "k=$k: aco-dt failed"; exit 1; }
sleep 20
timeout 90 "$W/tools/teleport" "$t" -930 3570 215 0 -8 >/dev/null 2>&1
sleep 30
timeout 90 "$W/tools/teleport" "$t" alexandria-dt-start >/dev/null 2>&1
echo "== k=$k $(date -u +%H:%M:%S)"
"$W/tools/npc-re/capsweep.sh" "$t" "$settle" "$@"
