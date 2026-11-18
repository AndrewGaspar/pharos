#!/usr/bin/env bash
# senu.sh <instance> [heading=75] [fly s=60] [hover s=60] [boost=0|1] [look deg/sample=0] : fly Senu
# over a city, then hover. Keys from ControlMappings.xml: EagleMode = scancode 47 (V), EagleAccelerate
# = 42 (LShift), EagleStop = mouse button 1 (right; held = hover). Senu flies straight along the
# camera heading she starts with (mouse look in flight turns only the camera, not her course; A / D
# steer her), so the script turns the on-foot camera first (tools/camera), then presses V. No clicks at screen positions: works at any resolution.
# CIRCLE=<deg> (env): steer that much clockwise per sample while flying (D held deg/80 s; Senu circles).
# While hovering, "look" turns the camera by that many degrees per sample (a pan over the city).
# Every ~2 s: elapsed, phase, Senu's camera position, distance from the player, spawned
# (spawncount.py), virtual TLS in use and the newest perf line (fps, worst frame) from pharos.log.
# Default heading 75 from the Alexandria DT/story anchor (-749.7, 3574.2) crosses the city centre
# (most civilian-spawner units lie at 30-120 deg, 200-600 m). Leaves Senu (V) at the end.
set -uo pipefail
W=$(cd "$(dirname "$0")/../.." && pwd)
t=${1:?instance} heading=${2:-75} fly=${3:-60} hover=${4:-60} boost=${5:-0} look=${6:-0}
A=$W/scripts/aco
L=/home/ajg/Games/aco-dlss-instances/$t/out/pharos.log
where() { timeout 10 "$W/tools/where" "$t"; }
read -r px py _ _ _ _ _ cz0 _ < <(where)
[[ -n ${cz0:-} ]] || { echo "senu: no position (not in the world?)"; exit 1; }
timeout 40 "$W/tools/camera" "$t" "$heading" -8 >/dev/null || { echo "senu: camera turn failed"; exit 1; }
"$A" keys "$t" keydown v sleep 0.2 keyup v >/dev/null
sleep 3
read -r _ _ _ _ _ _ _ cz _ < <(where)
awk -v a="$cz0" -v b="$cz" 'BEGIN {exit !(b - a > 8)}' || { echo "senu: camera did not rise ($cz0 -> $cz); not in Senu mode"; exit 1; }
cleanup() { "$A" keys "$t" keyup shift mouseup 3 >/dev/null 2>&1; }
trap cleanup EXIT
((boost)) && "$A" keys "$t" keydown shift >/dev/null
start=$SECONDS phase=fly
sample() {
  read -r _ _ _ _ _ x y z yaw _ < <(where)
  local sp vt pf
  sp=$(cd "$W/tools/npc-re" && ACO_INST=$t timeout 10 python3 spawncount.py 2>/dev/null | tail -1 | sed -n 's/.*spawned=\([0-9]*\).*/\1/p')
  vt=$(grep -o 'virtual TLS [0-9]*\|[0-9]* virtual indices' "$L" | tail -1 | grep -o '[0-9]*')
  pf=$(grep -o 'perf: [0-9.]* fps ([0-9.]* ms avg, [0-9.]* ms worst)' "$L" | tail -1 | sed 's/perf: //')
  printf '%4d s %-5s senu %.0f %.0f %.0f yaw %.0f  %4.0f m from player  spawned %s  vTLS %s  %s\n' \
    $((SECONDS - start)) "$phase" "${x:-0}" "${y:-0}" "${z:-0}" "${yaw:-0}" \
    "$(awk -v x="${x:-0}" -v y="${y:-0}" -v a="$px" -v b="$py" 'BEGIN {print sqrt((x-a)^2 + (y-b)^2)}')" \
    "${sp:-?}" "${vt:-0}" "${pf:-?}"
}
while ((SECONDS - start < fly)); do
  sample
  # CIRCLE=deg per sample: steer with D (MovementRight turns Senu clockwise at ~80 deg/s; mouse look
  # only turns the camera, not her course)
  [[ -n ${CIRCLE:-} ]] && "$A" keys "$t" keydown d sleep "$(awk -v d="$CIRCLE" 'BEGIN {print d / 80}')" keyup d >/dev/null
  sleep 1
done
((boost)) && "$A" keys "$t" keyup shift >/dev/null
"$A" keys "$t" mousedown 3 >/dev/null
phase=hover
h0=$SECONDS
while ((SECONDS - h0 < hover)); do
  sample
  if [[ $look != 0 ]]; then
    "$A" keys "$t" mousemove_relative -- "$(awk -v d="$look" 'BEGIN {print int(-d / 0.11)}')" 0 >/dev/null
  fi
  sleep 1
done
"$A" keys "$t" mouseup 3 >/dev/null
"$A" keys "$t" keydown v sleep 0.2 keyup v >/dev/null
