#!/usr/bin/env bash
# relaunch.sh <instance> [aco game args...] : kill the game, start it with build/mod, enter Discovery Tour.
set -uo pipefail
t=$1; shift
W=$(cd "$(dirname "$0")/../.." && pwd)
"$W/scripts/aco" kill-game "$t" >/dev/null 2>&1
for try in 1 2 3; do
  timeout 180 "$W/scripts/aco" game "$t" --mod "$W/build/mod" "$@" && timeout 420 "$W/scripts/aco-dt" "$t" && exit 0
  echo "relaunch: attempt $try failed; retrying in 30 s" >&2
  "$W/scripts/aco" kill-game "$t" >/dev/null 2>&1
  sleep 30
done
exit 1
