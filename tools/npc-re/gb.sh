#!/usr/bin/env bash
# gb.sh <rva>[,<rva>...] [hits=5] [exprs="$rcx"] [secs=20] : hardware execution breakpoints at image RVAs in
# the running game (gdb from the host); prints the expressions (comma-separated gdb exprs) per hit.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
pid=$("$here/gamepid.sh" "${ACO_INST:?set ACO_INST=t1/t3}")
[[ -n $pid ]] || { echo "gb: game not running" >&2; exit 1; }
base=$(grep -m1 'ACOrigins.exe' /proc/$pid/maps | cut -d- -f1)
abs=$(python3 -c "import sys;b=int('$base',16);print(','.join('%x'%(b+int(x,16)) for x in '$1'.split(',')))")
secs=${4:-20}
( sleep $((secs + 8)); pkill -INT -f "^gdb -p $pid -batch -x $here/hwbreak.py" ) &
killer=$!
BSTACK=${BSTACK:-0} BADDR=$abs BN=${2:-5} BEXPR=${3:-\$rcx} WSECS=$secs WBASE=$base \
  timeout -s INT -k 30 $((secs + 40)) gdb -p "$pid" -batch -x "$here/hwbreak.py" 2>&1 | grep -E '^(hit|stopped|error|   stack)'
kill $killer 2>/dev/null
