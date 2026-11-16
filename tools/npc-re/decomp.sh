#!/usr/bin/env bash
# decomp.sh <rva>... : decompile functions of the image dump with headless Ghidra (raw binary import,
# no auto-analysis; the project is created once and reused). Output: C on stdout.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
IMG=${IMG:-/tmp/claude-1000/-home-ajg-code-ac-dlss/249d0810-7bc6-42cc-9509-5599a9b80e14/scratchpad/npc/img}
P=${GPROJ:-/tmp/claude-1000/-home-ajg-code-ac-dlss/249d0810-7bc6-42cc-9509-5599a9b80e14/scratchpad/npc/ghidra}
H=/opt/ghidra/support/analyzeHeadless
mkdir -p "$P"
base=$(python3 -c "import struct;b=open('$IMG','rb').read(0x200);pe=struct.unpack_from('<I',b,0x3c)[0];print(hex(struct.unpack_from('<Q',b,pe+48)[0]))")
if [[ ! -d $P/aco.rep ]]; then
  "$H" "$P" aco -import "$(readlink -f "$IMG")" -loader BinaryLoader -loader-baseAddr "$base" \
    -processor x86:LE:64:default -cspec windows -noanalysis >/dev/null 2>&1
fi
out=$(mktemp)
DECOMP_OUT=$out "$H" "$P" aco -process "$(basename "$(readlink -f "$IMG")")" -noanalysis -scriptPath "$here/ghidra" \
  -postScript Decomp.java "$@" >/dev/null 2>&1 || true
base_dec=$((base))
# Show addresses as RVAs: FUN_<abs> / DAT_<abs> -> FUN_r<rva>
python3 - "$out" "$base_dec" <<'PY'
import re, sys
s = open(sys.argv[1]).read(); b = int(sys.argv[2])
s = re.sub(r'(FUN|DAT|LAB|PTR|uRam|_DAT|switchD|caseD)_([0-9a-f]{12})', lambda m: '%s_r%x' % (m[1], int(m[2], 16) - b), s)
s = re.sub(r'func_0x([0-9a-f]{12})', lambda m: 'f_%x' % (int(m[1], 16) - b), s)
print(s)
PY
rm -f "$out"
