#!/usr/bin/env bash
# freezetest.sh <instance> <mod dir> <secs> <k:max>... : per spec a fresh story launch with
# npc_units=k and npc_units_max=max, then watch the mod log for <secs>: a log silent for > 15 s
# means the game froze. Prints the extra-unit count at the end / at the freeze.
set -uo pipefail
t=$1 mod=$2 secs=$3; shift 3
W=$(cd "$(dirname "$0")/../.." && pwd)
L=/home/ajg/Games/aco-dlss-instances/$t/out/pharos.log
for spec in "$@"; do
  IFS=: read -r k mx <<<"$spec"
  sed -i "s/^npc_units=.*/npc_units=$k/" "$mod/pharos.ini"
  grep -q '^npc_units_max=' "$mod/pharos.ini" || sed -i "s/^npc_units=.*/&\nnpc_units_max=0/" "$mod/pharos.ini"
  sed -i "s/^npc_units_max=.*/npc_units_max=$mx/" "$mod/pharos.ini"
  if "$W/tools/npc-re/userplaying.sh"; then echo "not launching: the user is playing"; exit 2; fi
  "$W/scripts/aco" kill-game "$t" >/dev/null 2>&1
  "$W/scripts/aco" fixture "$t" story-alexandria-port >/dev/null 2>&1
  timeout 180 "$W/scripts/aco" game "$t" --mod "$mod" >/dev/null 2>&1 || { echo "$spec: launch failed"; continue; }
  timeout 420 "$W/scripts/aco-dt" "$t" --story >/dev/null 2>&1 || { echo "$spec: aco-dt failed"; continue; }
  end=$((SECONDS + secs)); froze=0
  while ((SECONDS < end)); do
    sleep 5
    if (( $(date +%s) - $(stat -c %Y "$L") > 15 )); then froze=1; break; fi
  done
  ex=$(grep 'npc_units: [0-9]* [a-z ]*extra' "$L" | tail -1 | sed 's/.*npc_units: \([0-9]*\) [a-z ]*extra units (\([0-9]*\) units handled.*/\1 extras, \2 handled/')
  fps=$(grep -o 'perf: [0-9.]* fps' "$L" | tail -5 | awk '{s+=$2; n++} END {if (n) printf "%.1f", s/n}')
  echo "k=$k max=$mx: $( ((froze)) && echo "FROZE after $((secs - (end - SECONDS))) s" || echo "ok for $secs s"), $ex, last fps $fps"
  ACO_INST=$t python3 "$W/tools/npc-re/bulkcount.py" | sed "s/^/    bulk: /"
  ACO_INST=$t python3 "$W/tools/npc-re/spawnbands.py" | head -1 | sed "s/^/    /"
done
