#!/usr/bin/env bash
# dumprun.sh <profile> <label> : fresh launch with profile, enter Discovery Tour, dump image -> scratch/lod/<label>.image
set -euo pipefail
profile=$1 label=$2
W=/home/ajg/code/ac-dlss/.claude/worktrees/agent-a205672bea6c9d67f
O=/home/ajg/Games/aco-dlss-instances/t1/out
D=/tmp/claude-1000/-home-ajg-code-ac-dlss/249d0810-7bc6-42cc-9509-5599a9b80e14/scratchpad/lod
cd "$W"
scripts/aco kill-game t1
scripts/aco game t1 --mod build/mod --profile "$profile" >/dev/null
timeout 400 scripts/aco-dt t1 | tail -1
sleep 20
n0=$(grep -c "dump: wrote" $O/pharos.log || true)
touch $O/dump-image
for i in $(seq 90); do [ "$(grep -c "dump: wrote" $O/pharos.log || true)" -gt "$n0" ] && break; sleep 2; done
grep "dump: wrote" $O/pharos.log | tail -1
mv $O/ACOrigins.image "$D/$label.image"
grep "geo:" $O/pharos.log | tail -2
echo "dumped $label"
