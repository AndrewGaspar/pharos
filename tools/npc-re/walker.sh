#!/usr/bin/env bash
# walker.sh <instance> <minutes> : wander the player (walk/run forward, turn) so the world streams and
# NPCs spawn/despawn naturally (no teleports), for soak tests.
set -uo pipefail
t=$1 mins=$2
A=$(cd "$(dirname "$0")/../.." && pwd)/scripts/aco
end=$((SECONDS + mins * 60)); i=0
while ((SECONDS < end)); do
  turn=$(( (RANDOM % 600) - 300 ))
  "$A" keys "$t" mousemove_relative -- $turn 0 >/dev/null 2>&1
  "$A" keys "$t" keydown w sleep $((8 + RANDOM % 12)) keyup w >/dev/null 2>&1
  i=$((i + 1))
  ((i % 6 == 0)) && "$A" keys "$t" keydown s sleep 3 keyup s >/dev/null 2>&1  # unstick from walls
done
