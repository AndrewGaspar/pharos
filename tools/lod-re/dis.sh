#!/usr/bin/env bash
# dis.sh <start-rva> <end-rva>  — disassemble the decrypted image (file offset = RVA)
IMG=/home/ajg/Games/aco-dlss-instances/t1/out/ACOrigins.image
x86_64-w64-mingw32-objdump -D -b binary -m i386:x86-64 --start-address=$(($1)) --stop-address=$(($2)) "$IMG" | tail -n +8 | grep -v "int3"
