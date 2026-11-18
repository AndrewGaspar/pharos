#!/usr/bin/env bash
# v12-places.sh <instance> <mod dir> <tag> : one fresh story launch, then spawned-by-band, the urban /
# rural / hostile split of the extras and a screenshot in: Alexandria (spawn anchor), Memphis (fast
# travel), a farm in the Faiyum (short teleport), Sinai (fast travel); finally back to Alexandria
# (fast travel) and a quit-to-title + reload. Screenshots go to <scratch>/v12-<tag>-<place>.png.
set -uo pipefail
t=$1 mod=$2 tag=$3
W=$(cd "$(dirname "$0")/../.." && pwd)
A=$W/scripts/aco
I=/home/ajg/Games/aco-dlss-instances/$t
L=$I/out/pharos.log
SHOT=/tmp/claude-1000/-home-ajg-code-ac-dlss/249d0810-7bc6-42cc-9509-5599a9b80e14/scratchpad
SD="$I/prefix/drive_c/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/savegames/$(cat "$I/account_id")/3539"
guard() { if "$W/tools/npc-re/userplaying.sh"; then echo "stopping: the user is playing"; "$A" kill-game "$t" >/dev/null 2>&1; exit 2; fi; }
launch() {
  guard
  "$A" kill-game "$t" >/dev/null 2>&1
  timeout 180 "$A" game "$t" --mod "$mod" >/dev/null 2>&1 || { echo "launch failed"; exit 1; }
  timeout 420 "$W/scripts/aco-dt" "$t" --story >/dev/null 2>&1 || { echo "aco-dt failed"; exit 1; }
}
measure() {
  local place=$1
  sleep 90; guard
  (( $(date +%s) - $(stat -c %Y "$L") < 20 )) || { echo "HANG in $place"; exit 1; }
  echo "== $place $(date -u +%H:%M:%S) at $(timeout 15 "$W/tools/where" "$t" | cut -d' ' -f1-2)"
  ACO_INST=$t python3 "$W/tools/npc-re/spawnbands.py" | head -1 | sed 's/^/  /'
  grep 'npc_units: made in 10 s' "$L" | tail -9 | awk '{u+=$(NF-5); r+=$(NF-3); h+=$(NF-1)} END {printf "  extras made over the last 90 s: %d urban, %d rural, %d hostile\n", u, r, h}'
  grep -o 'npc: [0-9]* spawned.*' "$L" | tail -1 | sed 's/^/  /' | cut -c1-150
  grep -o 'perf: [0-9.]* fps' "$L" | tail -5 | awk '{s+=$2; n++} END {if (n) printf "  %.1f fps\n", s/n}'
  "$A" shot "$t" "$SHOT/v12-$tag-$place.png" >/dev/null
}
"$A" kill-game "$t" >/dev/null 2>&1
"$A" fixture "$t" story-alexandria-port >/dev/null 2>&1
launch; measure alexandria
"$W/tools/npc-re/fasttravel.sh" "$t" memphis | sed 's/^/  /'; measure memphis
timeout 90 "$W/tools/teleport" "$t" -1500 -3800 260 0 -8 --hold 6 >/dev/null 2>&1; measure faiyum-farm
"$W/tools/npc-re/fasttravel.sh" "$t" sinai | sed 's/^/  /'; measure sinai
"$W/tools/npc-re/fasttravel.sh" "$t" alexandria | sed 's/^/  /'; measure alexandria-after-travel
m0=$(stat -c %Y "$SD/3.save")
"$A" keys "$t" keydown Escape sleep 0.15 keyup Escape >/dev/null; sleep 3
"$A" keys "$t" mousemove 144 286 sleep 0.3 mousedown 1 sleep 0.12 mouseup 1 >/dev/null; sleep 3
"$A" keys "$t" keydown space sleep 0.15 keyup space >/dev/null
for i in $(seq 1 12); do sleep 5; (( $(stat -c %Y "$SD/3.save") > m0 )) && break; done
echo "== quit to title: 3.save $( (( $(stat -c %Y "$SD/3.save") > m0 )) && echo written || echo 'NOT written')"
launch; measure reload
"$A" kill-game "$t" >/dev/null 2>&1
echo "== done $(date -u +%H:%M:%S)"
