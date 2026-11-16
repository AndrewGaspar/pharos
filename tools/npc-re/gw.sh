#!/usr/bin/env bash
# gw.sh <addr hex> [len=4] [kind=rwatch|awatch|watch] [hits=30] [secs=20] : hardware watchpoint on the
# running game (found by process name), via gdb from the host. Prints accessing instructions as RVAs.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
pid=$("$here/gamepid.sh" "${ACO_INST:?set ACO_INST=t1/t3}")
[[ -n $pid ]] || { echo "gw: game not running" >&2; exit 1; }
base=$(grep -m1 'ACOrigins.exe' /proc/$pid/maps | cut -d- -f1)
secs=${5:-20}
( sleep $((secs + 8)); pkill -INT -f "^gdb -p $pid -batch -x $here/hwwatch.py" ) &
killer=$!
BSTACK=${BSTACK:-0} WPER=${WPER:-1} WADDR=$1 WLEN=${2:-4} WKIND=${3:-rwatch} WN=${4:-30} WSECS=${5:-20} WBASE=$base \
  timeout -s INT -k 30 $(( ${5:-20} + 40 )) gdb -p "$pid" -batch -x "$here/hwwatch.py" 2>&1 |
  grep -vE '^\[New Thread|New LWP|^Old value|^New value|^Value = |hit Hardware|hit Read|hit Acc|^\[Thread|^warning|^$|Reading symbols|Missing separate|^0x0000|Download|debuginfod'
kill $killer 2>/dev/null
