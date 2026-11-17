#!/usr/bin/env bash
# inview-now.sh <instance> [secs=60] : in-view character load right now = mean skinning-compute thread
# groups per frame over the next <secs> (only drawn characters are skinned), plus MetaAI units near
# the player (mgrunits.py). Unlike inview.sh it changes no settings (for runs compared across launches,
# e.g. [world] npc_units, whose extra units cannot be removed live).
set -uo pipefail
t=$1 secs=${2:-60}
here=$(cd "$(dirname "$0")" && pwd)
I=/home/ajg/Games/aco-dlss-instances/$t
L=$I/out/aco_dlss.log
n0=$(wc -l < "$L")
touch "$I/out/geo"
sleep "$secs"
rm -f "$I/out/geo"
tail -n +"$((n0 + 1))" "$L" | grep "geo:" | sed 's/.*skinning \([0-9.]*\) dispatches \([0-9]*\) groups.*/\2/' |
  awk '{s+=$1; ss+=$1*$1; n++} END {if (n) printf "%.0f skinning groups/frame (sd %.0f, n=%d)\n", s/n, sqrt(ss/n-(s/n)^2), n; else print "no geo samples"}'

ACO_INST=$t python3 "$here/mgrunits.py"
