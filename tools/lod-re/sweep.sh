#!/usr/bin/env bash
# sweep.sh <vtable-rva> "<off>:<kind>:<test>:<default>" ... : apply each change, average indirect draws, restore
set -uo pipefail
vt=$1; shift
O=/home/ajg/Games/aco-dlss-instances/t1/out
L=$O/aco_dlss.log
avg() { # average indirect/frame of the last 3 geo lines
  grep "geo:" $L | tail -3 | sed 's/.*, \([0-9]*\) indirect.*/\1/' | awk '{s+=$1} END {printf "%.0f", s/NR}'
}
apply() { printf '%s\n' "$1" > $O/memtool.txt; for i in $(seq 30); do [ -f $O/memtool.txt ] || break; sleep 0.5; done; }
sleep 7; echo "baseline: $(avg)"
for spec in "$@"; do
  IFS=: read -r off kind test def <<< "$spec"
  apply "vw$kind $vt $off $test"; sleep 7; v=$(avg)
  apply "vw$kind $vt $off $def"; sleep 7; b=$(avg)
  echo "+$off=$test: $v  (restored $def: $b)"
done
