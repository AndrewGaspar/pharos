#!/usr/bin/env bash
# dis.sh <start-rva> <end-rva>... : disassemble the decrypted image (file offset = RVA); pairs of start/end
IMG=${IMG:-/tmp/claude-1000/-home-ajg-code-ac-dlss/249d0810-7bc6-42cc-9509-5599a9b80e14/scratchpad/npc/img}
while [ $# -ge 2 ]; do
  echo "== $1"
  x86_64-w64-mingw32-objdump -D -b binary -m i386:x86-64 --start-address=$(($1)) --stop-address=$(($2)) "$IMG" |
    tail -n +8 | grep -v "int3" | cut -c1-8,30-
  shift 2
done
