#!/usr/bin/env bash
# senu-ab.sh <instance> <mod dir> <out dir> <tag:key=value,...>... : for each variant, edit pharos.ini
# in the mod dir (key=value pairs, comma separated), fresh story launch in Alexandria, 60 s on foot,
# then senu.sh (heading 75, fly $FLY s (60), hover $HOVER s (120) panning 8 deg per sample). Saves
# <out>/senu-<tag>.txt (senu.sh samples) and <out>/senu-<tag>.log (pharos.log).
# Example: senu-ab.sh t1 build/mod-uncap /tmp/x k1:npc_units=1 k8:npc_units=8
set -uo pipefail
W=$(cd "$(dirname "$0")/../.." && pwd)
t=$1 mod=$2 out=$3; shift 3
A=$W/scripts/aco
L=/home/ajg/Games/aco-dlss-instances/$t/out/pharos.log
mkdir -p "$out"
for v in "$@"; do
  tag=${v%%:*} kvs=${v#*:}
  if "$W/tools/npc-re/userplaying.sh"; then echo "stopping: the user is playing"; "$A" kill-game "$t" >/dev/null 2>&1; exit 2; fi
  IFS=, read -ra pairs <<< "$kvs"
  for kv in "${pairs[@]}"; do
    k=${kv%%=*}
    if grep -q "^$k=" "$mod/pharos.ini"; then sed -i "s|^$k=.*|$kv|" "$mod/pharos.ini"; else echo "unknown key $k in $mod/pharos.ini"; exit 1; fi
  done
  "$A" kill-game "$t" >/dev/null 2>&1
  "$A" fixture "$t" story-alexandria-port >/dev/null 2>&1
  timeout 180 "$A" game "$t" --mod "$mod" >/dev/null 2>&1 || { echo "$tag: launch failed"; continue; }
  timeout 420 "$W/scripts/aco-dt" "$t" --story >/dev/null 2>&1 || { echo "$tag: aco-dt failed"; continue; }
  sleep 60
  if "$W/tools/npc-re/userplaying.sh"; then echo "stopping: the user is playing"; "$A" kill-game "$t" >/dev/null 2>&1; exit 2; fi
  echo "== $tag ($kvs) $(date -u +%H:%M:%S)"
  timeout 600 "$W/tools/npc-re/senu.sh" "$t" 75 "${FLY:-60}" "${HOVER:-120}" 0 8 > "$out/senu-$tag.txt" 2>&1
  tail -1 "$out/senu-$tag.txt"
  cp "$L" "$out/senu-$tag.log"
  "$A" kill-game "$t" >/dev/null 2>&1
done
