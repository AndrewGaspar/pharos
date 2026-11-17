#!/usr/bin/env bash
# ab-units.sh <instance> <mod dir> <k>... : one measurement per k of [world] npc_units, each on a fresh
# launch of the story fixture story-alexandria-port: load (player at the Alexandria spawn anchor),
# hop 180 m west for 30 s (so the units around the spawn anchor deactivate and, with k > 1, get their
# extras), hop back, settle, then inview-now.sh 120 s (skinning groups/frame + units near) and the
# spawned count. The mod dir's aco_dlss.ini is edited (npc_units=k) before each launch.
set -uo pipefail
t=$1 mod=$2; shift 2
W=$(cd "$(dirname "$0")/../.." && pwd)
L=/home/ajg/Games/aco-dlss-instances/$t/out/aco_dlss.log
for k in "$@"; do
  sed -i "s/^npc_units=.*/npc_units=$k/" "$mod/aco_dlss.ini"
  "$W/scripts/aco" kill-game "$t" >/dev/null 2>&1
  "$W/scripts/aco" fixture "$t" story-alexandria-port >/dev/null 2>&1
  timeout 180 "$W/scripts/aco" game "$t" --mod "$mod" --fps 30 >/dev/null 2>&1 || { echo "k=$k: launch failed"; continue; }
  timeout 420 "$W/scripts/aco-dt" "$t" --story >/dev/null 2>&1 || { echo "k=$k: aco-dt failed"; continue; }
  sleep 20
  timeout 90 "$W/tools/teleport" "$t" -930 3570 215 0 -8 >/dev/null 2>&1
  sleep 30
  timeout 90 "$W/tools/teleport" "$t" alexandria-dt-start >/dev/null 2>&1
  sleep 30
  echo "== k=$k $(date -u +%H:%M:%S) $("$W/tools/where" "$t" | cut -d' ' -f1-3)"
  "$W/tools/npc-re/inview-now.sh" "$t" 120
  grep 'npc: [0-9]* spawned' "$L" | tail -1 | sed 's/.*npc: /  /'
  grep 'npc_units: [0-9]* extra\|npc_units: cost' "$L" | tail -2 | sed 's/.*npc_units: /  /'
done
