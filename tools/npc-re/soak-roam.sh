#!/usr/bin/env bash
# soak-roam.sh <instance> <mod dir> <minutes> : fresh story launch of the fixture, then roamer.sh for
# <minutes> (travels, no teleports), with a status line per minute (position, spawned, extras, hook
# cost, log freshness). A log silent for > 20 s is reported as a hang (and the roamer stopped).
set -uo pipefail
W=$(cd "$(dirname "$0")/../.." && pwd)
t=$1 mod=$2 mins=$3
L=/home/ajg/Games/aco-dlss-instances/$t/out/aco_dlss.log
if "$W/tools/npc-re/userplaying.sh"; then echo "not launching: the user is playing"; exit 2; fi
"$W/scripts/aco" kill-game "$t" >/dev/null 2>&1
"$W/scripts/aco" fixture "$t" story-alexandria-port >/dev/null 2>&1
timeout 180 "$W/scripts/aco" game "$t" --mod "$mod" >/dev/null 2>&1 || { echo "launch failed"; exit 1; }
timeout 420 "$W/scripts/aco-dt" "$t" --story >/dev/null 2>&1 || { echo "aco-dt failed"; exit 1; }
echo "soak start $(date -u +%H:%M:%S)"
"$W/tools/npc-re/roamer.sh" "$t" "$mins" > /tmp/roamer.$$ 2>&1 &
rpid=$!
end=$((SECONDS + mins * 60 + 30)); hang=0
while ((SECONDS < end)) && kill -0 $rpid 2>/dev/null; do
  sleep 60
  age=$(( $(date +%s) - $(stat -c %Y "$L") ))
  sp=$(grep 'npc: [0-9]* spawned' "$L" | tail -1 | sed 's/.*npc: \([0-9]*\) spawned.*/\1/')
  ex=$(grep 'npc_units: [0-9]* extra' "$L" | tail -1 | sed 's/.*npc_units: \([0-9]*\) extra units (\([0-9]*\) units handled, \([0-9]*\) failed).*/\1 extras, \3 failed/')
  co=$(grep 'npc_units: cost' "$L" | tail -1 | sed 's/.*ours \([0-9.]*\) ms avg \/ \([0-9.]*\) ms max/ours \1 ms avg, \2 max/')
  fr=$(grep -o 'fg real [0-9.]* fps\|perf: [0-9.]* fps' "$L" | tail -1)
  echo "$(date -u +%H:%M:%S) $(tail -1 /tmp/roamer.$$) | spawned ${sp:-?} | ${ex:-?} | ${co:-?} | ${fr:-?} | log age ${age}s"
  if ((age > 20)); then echo "HANG: log silent ${age}s"; hang=1; break; fi
done
kill $rpid 2>/dev/null; wait $rpid 2>/dev/null
tail -1 /tmp/roamer.$$; rm -f /tmp/roamer.$$
echo "soak end $(date -u +%H:%M:%S)$( ((hang)) && echo ' (hang)')"
