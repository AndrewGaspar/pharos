#!/usr/bin/env bash
# ft-sinai.sh <instance> <mod dir> : the fast-travel repro from the user's v11 log: start in
# Alexandria, the game's own fast travel to Sinai (another world: a new MetaAI manager), roam 90 s,
# fast travel back to Alexandria, roam 120 s. Prints the npc_units lines after each leg.
set -uo pipefail
t=$1 mod=$2
W=$(cd "$(dirname "$0")/../.." && pwd)
L=/home/ajg/Games/aco-dlss-instances/$t/out/aco_dlss.log
if "$W/tools/npc-re/userplaying.sh"; then echo "not launching: the user is playing"; exit 2; fi
"$W/scripts/aco" kill-game "$t" >/dev/null 2>&1
"$W/scripts/aco" fixture "$t" story-alexandria-port >/dev/null 2>&1
timeout 180 "$W/scripts/aco" game "$t" --mod "$mod" >/dev/null 2>&1 || { echo "launch failed"; exit 1; }
timeout 420 "$W/scripts/aco-dt" "$t" --story >/dev/null 2>&1 || { echo "aco-dt failed"; exit 1; }
report() {
  echo "-- $1 $(date -u +%H:%M:%S)"
  grep -E 'npc_units: ([0-9]+ live extra|steps skipped|of [0-9]+ steps|stepping manager|MetaAI manager .* hooked)|npc: [0-9]+ spawned' "$L" | tail -6 | sed 's/^[0-9:.]* \[ *[0-9]*\] /  /' | cut -c1-170
}
sleep 60; report "Alexandria (start)"
"$W/tools/npc-re/fasttravel.sh" "$t" sinai
timeout 150 "$W/tools/npc-re/roamer.sh" "$t" 2 >/dev/null 2>&1; report "Sinai"
"$W/tools/npc-re/fasttravel.sh" "$t" alexandria
timeout 180 "$W/tools/npc-re/roamer.sh" "$t" 2 >/dev/null 2>&1; report "Alexandria (after travel)"
ACO_INST=$t python3 "$W/tools/npc-re/spawnbands.py" | head -1 | sed 's/^/  /'
