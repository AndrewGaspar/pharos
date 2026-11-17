#!/usr/bin/env bash
# ab-bands.sh <instance> <mod dir> <k:density:distance>... : one fresh launch of the story fixture
# per spec (player at the Alexandria spawn anchor, fixed camera, no movement). After 120 s: 60 s of
# samples: fps (the mod's perf lines; run the mod dir uncapped for meaningful numbers), in-view
# skinning load, the game's spawned count, and spawnbands.py (spawned civilians per distance band)
# three times; then the npc_units cost line.
set -uo pipefail
t=$1 mod=$2; shift 2
W=$(cd "$(dirname "$0")/../.." && pwd)
I=/home/ajg/Games/aco-dlss-instances/$t
L=$I/out/aco_dlss.log
run_one() {  # one launch + measurement of $k $d $dist; prints the block; returns 3 if the game was paused
  sed -i "s/^npc_units=.*/npc_units=$k/; s/^npc_density=.*/npc_density=$d/; s/^npc_distance=.*/npc_distance=$dist/" "$mod/aco_dlss.ini"
  if "$W/tools/npc-re/userplaying.sh"; then echo "$(date -u +%H:%M:%S) not launching: the user is playing"; exit 2; fi
  "$W/scripts/aco" kill-game "$t" >/dev/null 2>&1
  "$W/scripts/aco" fixture "$t" story-alexandria-port >/dev/null 2>&1
  timeout 180 "$W/scripts/aco" game "$t" --mod "$mod" >/dev/null 2>&1 || { echo "k=$k: launch failed"; return 1; }
  timeout 420 "$W/scripts/aco-dt" "$t" --story >/dev/null 2>&1 || { echo "k=$k: aco-dt failed"; return 1; }
  sleep 120
  n0=$(wc -l < "$L")
  touch "$I/out/geo"
  out=$(
    echo "== k=$k npc_density=$d npc_distance=$dist $(date -u +%H:%M:%S)"
    for i in 1 2; do sleep 15; ACO_INST=$t python3 "$W/tools/npc-re/spawnbands.py" | sed 's/^/  /'; done
    ACO_INST=$t python3 "$W/tools/npc-re/threadcpu.py" 25 | sed 's/^/  /'
    ACO_INST=$t python3 "$W/tools/npc-re/spawnbands.py" | sed 's/^/  /'
  )
  rm -f "$I/out/geo"
  tail -n +"$((n0 + 1))" "$L" > /tmp/abbands.$$
  fps=$(grep -o 'perf: [0-9.]* fps' /tmp/abbands.$$ | awk '{s+=$2; n++} END {if (n) printf "%.1f", s/n}')
  g=$(grep 'geo:' /tmp/abbands.$$ | sed 's/.*dispatches \([0-9]*\) groups.*/\1/' | awk '{s+=$1; n++} END {if (n) printf "%.0f", s/n}')
  sp=$(grep 'npc: [0-9]* spawned' /tmp/abbands.$$ | sed 's/.*npc: \([0-9]*\) spawned.*/\1/' | awk '{s+=$1; n++} END {if (n) printf "%.0f", s/n}')
  extra=$(grep 'npc_units: [0-9]* extra\|npc_units: cost' /tmp/abbands.$$ | tail -2 | sed 's/.*npc_units: /  /')
  rm -f /tmp/abbands.$$
  task=$(echo "$out" | sed -n 's/.*TaskThread# \([0-9]*\).*/\1/p')
  echo "$out"
  echo "  fps $fps | in view $g groups/frame | game spawned count $sp"
  [[ -n $extra ]] && echo "$extra"
  # Paused game (Ubisoft service hiccup, menu): task threads idle or no frames measured.
  if [[ -z $fps || -z $task ]] || (( task < 100 )); then echo "  ^ game paused during the samples: discarded"; return 3; fi
  return 0
}
for spec in "$@"; do
  IFS=: read -r k d dist <<<"$spec"
  for try in 1 2 3; do
    run_one && break
    rc=$?
    [[ $rc == 3 ]] || break
    echo "  retry $((try + 1)) for k=$k"
  done
done
