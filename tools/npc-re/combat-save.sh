#!/usr/bin/env bash
# combat-save.sh <instance> <mod dir> : hardening steps after the soak/travel tests, on a fresh story
# launch of the Alexandria fixture: (1) combat in the crowd (draw the weapon, chase and hit walking
# characters), (2) kill three extra units with the engine's kill ([world] npc_units_kill), (3) quit
# to the title screen (the game saves), relaunch and continue the story, check the game runs and what
# came back (dead / restored units), (4) a second quit + reload. Ends with the game stopped.
set -uo pipefail
t=$1 mod=$2
W=$(cd "$(dirname "$0")/../.." && pwd)
A=$W/scripts/aco
I=/home/ajg/Games/aco-dlss-instances/$t
L=$I/out/pharos.log
INI="$I/prefix/drive_c/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/games/Assassin's Creed Origins/pharos.ini"
SD="$I/prefix/drive_c/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/savegames/$(cat "$I/account_id")/3539"
alive() { (( $(date +%s) - $(stat -c %Y "$L") < 20 )); }
status() { echo "  $(grep 'npc: [0-9]* spawned' "$L" | tail -1 | sed 's/.*npc: //' | cut -c1-140)"; }
launch() {
  if "$W/tools/npc-re/userplaying.sh"; then echo "not launching: the user is playing"; exit 2; fi
  "$A" kill-game "$t" >/dev/null 2>&1
  timeout 180 "$A" game "$t" --mod "$mod" >/dev/null 2>&1 || { echo "launch failed"; exit 1; }
  timeout 420 "$W/scripts/aco-dt" "$t" --story >/dev/null 2>&1 || { echo "aco-dt failed"; exit 1; }
}
quit_to_title() {
  local m0; m0=$(stat -c %Y "$SD/3.save")
  "$A" keys "$t" keydown Escape sleep 0.15 keyup Escape >/dev/null; sleep 3
  "$A" keys "$t" mousemove 144 286 sleep 0.3 mousedown 1 sleep 0.12 mouseup 1 >/dev/null; sleep 3
  "$A" keys "$t" keydown space sleep 0.15 keyup space >/dev/null
  for i in $(seq 1 12); do sleep 5; (( $(stat -c %Y "$SD/3.save") > m0 )) && break; done
  echo "  quit to title: 3.save $( (( $(stat -c %Y "$SD/3.save") > m0 )) && echo written || echo 'NOT written') at $(date -u -r "$SD/3.save" +%H:%M:%S)"
}
"$A" fixture "$t" story-alexandria-port >/dev/null 2>&1
grep -q '^npc_units_kill=' "$mod/pharos.ini" || sed -i 's/^npc_units=.*/&\nnpc_units_kill=0/' "$mod/pharos.ini"
sed -i 's/^npc_units_kill=.*/npc_units_kill=0/' "$mod/pharos.ini"
launch
echo "== launched $(date -u +%H:%M:%S)"; sleep 60; status
echo "== combat"
timeout 300 /tmp/claude-1000/-home-ajg-code-ac-dlss/249d0810-7bc6-42cc-9509-5599a9b80e14/scratchpad/du/hunt2.sh "$t" 14 >/dev/null 2>&1
"$A" shot "$t" /tmp/claude-1000/-home-ajg-code-ac-dlss/249d0810-7bc6-42cc-9509-5599a9b80e14/scratchpad/combat.png >/dev/null
alive && echo "  game alive after combat" || { echo "  HANG after combat"; exit 1; }
status
echo "== kill three extras"
for n in 1 2 3; do
  sed -i "s/^npc_units_kill=.*/npc_units_kill=$n/" "$INI"
  timeout 30 bash -c "until [ \$(grep -c 'kill test' '$L') -ge $n ]; do sleep 2; done"
done
grep 'kill test' "$L" | sed 's/.*npc_units: /  /' | cut -c1-140
ACO_INST=$t python3 "$W/tools/npc-re/corpses.py" | grep -c dead | sed 's/^/  dead units now: /'
echo "== quit + reload 1"
quit_to_title
launch; sleep 60
alive && echo "  reload 1: game alive" || { echo "  HANG after reload 1"; exit 1; }
status
ACO_INST=$t python3 "$W/tools/npc-re/corpses.py" | awk '/restored/ {r++} /dead/ {d++} END {printf "  after reload: %d dead units, %d restored from the save\n", d, r}'
echo "== quit + reload 2"
quit_to_title
launch; sleep 60
alive && echo "  reload 2: game alive" || { echo "  HANG after reload 2"; exit 1; }
status
"$A" kill-game "$t" >/dev/null 2>&1
echo "== done $(date -u +%H:%M:%S)"
