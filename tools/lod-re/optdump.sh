#!/usr/bin/env bash
# optdump.sh <label> : run alloptions.memtool in the running t1 game, save the dumps to <label>.opts
set -euo pipefail
O=/home/ajg/Games/aco-dlss-instances/t1/out
D=/tmp/claude-1000/-home-ajg-code-ac-dlss/249d0810-7bc6-42cc-9509-5599a9b80e14/scratchpad/lod
n0=$(wc -l < $O/aco_dlss.log)
cp $D/alloptions.memtool $O/memtool.txt
for i in $(seq 120); do tail -n +$((n0 + 1)) $O/aco_dlss.log | grep -q "memtool: done" && break; sleep 1; done
tail -n +$((n0 + 1)) $O/aco_dlss.log | grep "memtool" | sed 's/^[0-9:.]* \[ *[0-9]*\] //' > $D/$1.opts
grep -c "memtool:   " $D/$1.opts
