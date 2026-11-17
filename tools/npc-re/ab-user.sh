#!/usr/bin/env bash
# ab-user.sh <instance> <mod dir> <k:fg>... : the user's settings (run the instance at 2560x1600, mod
# ini render_scale=0.6, DLSS), one fresh story launch per spec with npc_units=k and [fg] enabled=fg;
# after 150 s at the Alexandria spawn anchor (fixed camera), 60 s of samples: real and presented
# fps from the mod's perf lines, CPU per thread / GPU utilization / VRAM (threadcpu.py), spawned per
# band (spawnbands.py). Discards and redoes a step whose samples caught the game paused.
set -uo pipefail
t=$1 mod=$2; shift 2
W=$(cd "$(dirname "$0")/../.." && pwd)
I=/home/ajg/Games/aco-dlss-instances/$t
L=$I/out/aco_dlss.log
run_one() {
  sed -i "s/^npc_units=.*/npc_units=$k/" "$mod/aco_dlss.ini"
  sed -i "/^\[fg\]/,/^\[/ s/^enabled=.*/enabled=$fg/" "$mod/aco_dlss.ini"
  if "$W/tools/npc-re/userplaying.sh"; then echo "not launching: the user is playing"; exit 2; fi
  "$W/scripts/aco" kill-game "$t" >/dev/null 2>&1
  "$W/scripts/aco" fixture "$t" story-alexandria-port >/dev/null 2>&1
  timeout 180 "$W/scripts/aco" game "$t" --mod "$mod" >/dev/null 2>&1 || { echo "k=$k fg=$fg: launch failed"; return 1; }
  timeout 420 "$W/scripts/aco-dt" "$t" --story >/dev/null 2>&1 || { echo "k=$k fg=$fg: aco-dt failed"; return 1; }
  sleep 150
  n0=$(wc -l < "$L")
  touch "$I/out/geo"
  out=$(
    echo "== k=$k fg=$fg $(date -u +%H:%M:%S)"
    ACO_INST=$t python3 "$W/tools/npc-re/spawnbands.py" | head -1 | sed 's/^/  /'
    ACO_INST=$t python3 "$W/tools/npc-re/threadcpu.py" 40 | sed 's/^/  /'
    ACO_INST=$t python3 "$W/tools/npc-re/spawnbands.py" | head -1 | sed 's/^/  /'
  )
  rm -f "$I/out/geo"
  tail -n +"$((n0 + 1))" "$L" > /tmp/abuser.$$
  g=$(grep 'geo:' /tmp/abuser.$$ | sed 's/.*dispatches \([0-9]*\) groups.*/\1/' | awk '{s+=$1; n++} END {if (n) printf "%.0f", s/n}')
  real=$(grep -o 'fg real [0-9.]* fps' /tmp/abuser.$$ | awk '{s+=$3; n++} END {if (n) printf "%.1f", s/n}')
  pres=$(grep -o '= [0-9.]* presents/s' /tmp/abuser.$$ | awk '{s+=$2; n++} END {if (n) printf "%.1f", s/n}')
  fps=$(grep -o 'perf: [0-9.]* fps' /tmp/abuser.$$ | awk '{s+=$2; n++} END {if (n) printf "%.1f", s/n}')
  extra=$(grep 'npc_units: [0-9]* extra\|npc_units: cost' /tmp/abuser.$$ | tail -2 | sed 's/.*npc_units: /  /')
  tls=$(grep -o 'TLS slots free [0-9]* (min [0-9]*)' /tmp/abuser.$$ | tail -1)
  rm -f /tmp/abuser.$$
  task=$(echo "$out" | sed -n 's/.*TaskThread# \([0-9]*\).*/\1/p')
  echo "$out"
  echo "  ${tls:-TLS: no reading}"
  echo "  frames: ${real:+real $real fps, presented $pres/s}${real:-$fps fps} | in view $g groups/frame"
  [[ -n $extra ]] && echo "$extra"
  if [[ -z $task ]] || (( task < 100 )) || [[ -z $fps$real ]]; then echo "  ^ game paused during the samples: discarded"; return 3; fi
  return 0
}
for spec in "$@"; do
  IFS=: read -r k fg <<<"$spec"
  for try in 1 2 3; do
    run_one && break
    rc=$?
    [[ $rc == 3 ]] || break
    echo "  retry $((try + 1))"
  done
done
