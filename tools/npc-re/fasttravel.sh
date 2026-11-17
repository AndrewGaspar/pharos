#!/usr/bin/env bash
# fasttravel.sh <instance> <city> : the game's own fast travel through the map's Atlas view (768x480
# instance): M (map) -> Tab (Atlas) -> hover the city's fast-travel icon -> hold right mouse. Waits
# (deadline 240 s) until the player is back in the world near the city and prints the position.
# Cities (Atlas icon positions at 768x480): alexandria, siwa, memphis, faiyum, sinai (other world).
set -uo pipefail
t=$1 city=$2
W=$(cd "$(dirname "$0")/../.." && pwd)
A=$W/scripts/aco
case $city in
  alexandria) ix=356 iy=100 cx=-700 cy=3570;;
  siwa)       ix=228 iy=210 cx=-6400 cy=-260;;
  memphis)    ix=423 iy=255 cx=1470 cy=-1130;;
  faiyum)     ix=351 iy=326 cx=-890 cy=-4245;;
  sinai)      ix=568 iy=242 cx= cy=;;  # another world: arrival = moved > 1000 m
  *) echo "unknown city $city"; exit 1;;
esac
read -r x0 y0 _ < <(timeout 15 "$W/tools/where" "$t" 2>/dev/null)
"$A" keys "$t" keydown m sleep 0.15 keyup m >/dev/null; sleep 4
"$A" keys "$t" keydown Tab sleep 0.15 keyup Tab >/dev/null; sleep 3
"$A" keys "$t" mousemove "$ix" "$iy" sleep 0.8 mousedown 3 sleep 2.5 mouseup 3 >/dev/null
t0=$SECONDS
while ((SECONDS - t0 < 240)); do
  sleep 10
  read -r x y _ < <(timeout 15 "$W/tools/where" "$t" 2>/dev/null)
  [[ -n ${x:-} ]] || continue
  if [[ -z $cx ]]; then
    m=$(awk -v x="$x" -v y="$y" -v a="${x0:-0}" -v b="${y0:-0}" 'BEGIN{printf "%.0f", sqrt((x-a)^2+(y-b)^2)}')
    if ((m > 1000)); then echo "fast travel to $city: arrived at $x $y (moved $m m) after $((SECONDS - t0)) s"; exit 0; fi
    continue
  fi
  d=$(awk -v x="$x" -v y="$y" -v cx="$cx" -v cy="$cy" 'BEGIN{printf "%.0f", sqrt((x-cx)^2+(y-cy)^2)}')
  if ((d < 2500)); then echo "fast travel to $city: arrived at $x $y ($d m from the city centre) after $((SECONDS - t0)) s"; exit 0; fi
done
echo "fast travel to $city: not arrived after 240 s (last position ${x:-?} ${y:-?})"; exit 1
