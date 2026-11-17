#!/usr/bin/env bash
# fttour.sh <instance> <stay s> <city>... : chain of the game's own fast travels (fasttravel.sh), staying
# <stay> s in each city while roaming (roamer.sh) and reporting spawned / extras / TLS / fps there.
set -uo pipefail
t=$1 stay=$2; shift 2
W=$(cd "$(dirname "$0")/../.." && pwd)
L=/home/ajg/Games/aco-dlss-instances/$t/out/aco_dlss.log
for city in "$@"; do
  "$W/tools/npc-re/fasttravel.sh" "$t" "$city" || { echo "stopping: fast travel failed"; exit 1; }
  timeout $((stay + 60)) "$W/tools/npc-re/roamer.sh" "$t" $(( (stay + 59) / 60 )) > /dev/null 2>&1
  age=$(( $(date +%s) - $(stat -c %Y "$L") ))
  sp=$(grep 'npc: [0-9]* spawned' "$L" | tail -1 | sed 's/.*npc: \([0-9]*\) spawned.*TLS slots free \([0-9]*\) (min \([0-9]*\)), virtual TLS \([0-9]*\).*/\1 spawned, TLS free \2 (min \3), virtual \4/')
  ex=$(grep 'npc_units: [0-9]* [a-z ]*extra' "$L" | tail -1 | sed 's/.*npc_units: \([0-9]*\) [a-z ]*extra units.*(\([0-9]*\) failed).*/\1 extras, \2 failed/')
  fr=$(grep -o 'perf: [0-9.]* fps' "$L" | tail -3 | awk '{s+=$2; n++} END {if (n) printf "%.1f fps", s/n}')
  echo "$(date -u +%H:%M:%S) $city: $sp | $ex | $fr | log age ${age}s"
  if ((age > 20)); then echo "HANG in $city"; exit 1; fi
done
echo "tour done"
