#!/usr/bin/env bash
# roamer.sh <instance> <minutes> [cx cy radius] : walk the player around for soak tests WITHOUT
# teleports, and keep actually travelling: run in 8 s legs; after each leg compare the position
# (tools/where): if the player moved less than 4 m it is stuck, so back up and turn 100-260 degrees;
# if it is farther than <radius> m (default 450) from (cx, cy) (default: the start position), head
# back toward the centre. Prints one line per minute with the distance covered.
set -uo pipefail
t=$1 mins=$2
W=$(cd "$(dirname "$0")/../.." && pwd)
A=$W/scripts/aco
where() { timeout 20 "$W/tools/where" "$t" 2>/dev/null | awk '{print $1, $2}'; }
read -r cx cy < <(where)
cx=${3:-$cx} cy=${4:-$cy} R=${5:-450}
[[ -n $cx ]] || { echo "roamer: no position"; exit 1; }
end=$((SECONDS + mins * 60)); last_report=$SECONDS; travelled=0; stuck=0
read -r px py < <(where)
while ((SECONDS < end)); do
  "$A" keys "$t" keydown shift keydown w sleep 8 keyup w keyup shift >/dev/null 2>&1
  read -r x y < <(where)
  [[ -n ${x:-} ]] || { echo "roamer: lost position at $(date -u +%H:%M:%S)"; break; }
  d=$(awk -v a="$px" -v b="$py" -v x="$x" -v y="$y" 'BEGIN{printf "%.1f", sqrt((x-a)^2+(y-b)^2)}')
  travelled=$(awk -v a="$travelled" -v d="$d" 'BEGIN{print a+d}')
  far=$(awk -v x="$x" -v y="$y" -v cx="$cx" -v cy="$cy" -v r="$R" 'BEGIN{print (sqrt((x-cx)^2+(y-cy)^2) > r)}')
  if ((far)); then
    yaw=$(awk -v x="$x" -v y="$y" -v cx="$cx" -v cy="$cy" 'BEGIN{printf "%.0f", atan2(cy-y, cx-x)*57.2958 + (rand()*60-30)}')
    timeout 25 "$W/tools/camera" "$t" "$yaw" -8 >/dev/null 2>&1
  elif awk "BEGIN{exit !($d < 4)}"; then
    stuck=$((stuck + 1))
    "$A" keys "$t" keydown s sleep 1.5 keyup s >/dev/null 2>&1
    "$A" keys "$t" mousemove_relative -- $(( (RANDOM % 2 ? 1 : -1) * (1000 + RANDOM % 1600) )) 0 >/dev/null 2>&1
  elif ((RANDOM % 4 == 0)); then
    "$A" keys "$t" mousemove_relative -- $(( (RANDOM % 600) - 300 )) 0 >/dev/null 2>&1
  fi
  px=$x py=$y
  if ((SECONDS - last_report >= 60)); then
    echo "$(date -u +%H:%M:%S) at $x $y, travelled ${travelled} m, stuck turns $stuck"
    last_report=$SECONDS
  fi
done
echo "roamer done: travelled ${travelled} m, stuck turns $stuck"
