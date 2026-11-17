#!/usr/bin/env bash
# mt.sh <instance> <commands...> : run memtool commands (one per arg) in the running game, print the
# results (waits up to 90 s).
set -euo pipefail
t=$1; shift
O=/home/ajg/Games/aco-dlss-instances/$t/out
L=$O/pharos.log
n0=$(wc -l < "$L")
printf '%s\n' "$@" > "$O/memtool.txt"
end=$((SECONDS + 90))
until tail -n +$((n0 + 1)) "$L" | grep -q "memtool: done"; do
  ((SECONDS < end)) || { echo "mt.sh: timeout" >&2; exit 1; }
  sleep 0.5
done
tail -n +$((n0 + 1)) "$L" | grep "memtool" | sed 's/^[0-9:.]* \[ *[0-9]*\] memtool: //'
