#!/usr/bin/env bash
# strobe-ab.sh <instance> <mod dir> <out dir> <tag:section.key=value,...>... : for each variant, set
# the keys in <mod dir>/pharos.ini (section-aware), fresh story launch, teleport to the Alexandria
# centre (-411, 3899; where the user flew; the game sets Bayek down near (-276, 3815)), 60 s on foot,
# Senu flying at heading 145 (over the busiest streets) with the camera pitched down ~35 deg, and
# 14 s into the flight a framedump of 40 consecutive frames (the game's frames as presented by the
# mod; Frame Generation's generated frames are not in it). Hover mode is not used: it blurs the
# view. tools/strobe.py aligns the frames (global shift) and counts A -> B -> A pixel flips (shadows /
# highlights popping on and off). Out: <out>/strobe-<tag>.txt,
# a 6-frame strip <out>/strobe-<tag>.png and the first frame <out>/strobe-<tag>-0.png; frames deleted
# (STROBE_KEEP=WxH+X+Y keeps that crop of the first 12 as PNGs). HOVER=1: still shot from a hover
# (where Senu stops varies from run to run); FGTRACE=1 (FG on): also the mod's present trace, real
# and generated frames, analysed by tools/fgstrobe.py: a generated frame that interpolates its two
# real neighbours can never be brighter or darker than both, so on-off-on spikes in G presents are
# Frame Generation artifacts, also in flight (R spikes are the motion baseline). In flight the flip count is
# dominated by parallax (roof tiles); compare variants, and look at the kept crops.
set -uo pipefail
W=$(cd "$(dirname "$0")/../.." && pwd)
t=$1 mod=$2 out=$3; shift 3
A=$W/scripts/aco
O=/home/ajg/Games/aco-dlss-instances/$t/out
mkdir -p "$out"
for v in "$@"; do
  tag=${v%%:*} kvs=${v#*:}
  if "$W/tools/npc-re/userplaying.sh"; then echo "stopping: the user is playing"; "$A" kill-game "$t" >/dev/null 2>&1; exit 2; fi
  python3 - "$mod/pharos.ini" "$kvs" <<'EOF' || exit 1
import re, sys
path, kvs = sys.argv[1], sys.argv[2]
lines = open(path).read().split('\n')
for kv in kvs.split(','):
    sk, val = kv.split('=', 1)
    sec, key = sk.split('.', 1)
    cur, done = None, False
    for i, l in enumerate(lines):
        m = re.match(r'\s*\[(\w+)\]', l)
        if m: cur = m[1]; continue
        if cur == sec and re.match(r'%s\s*=' % re.escape(key), l):
            lines[i] = '%s=%s' % (key, val); done = True; break
    if not done: sys.exit('no %s in %s' % (sk, path))
open(path, 'w').write('\n'.join(lines))
EOF
  "$A" kill-game "$t" >/dev/null 2>&1
  "$A" fixture "$t" story-alexandria-port >/dev/null 2>&1
  timeout 180 "$A" game "$t" --mod "$mod" >/dev/null 2>&1 || { echo "$tag: launch failed"; continue; }
  timeout 420 "$W/scripts/aco-dt" "$t" --story >/dev/null 2>&1 || { echo "$tag: aco-dt failed"; continue; }
  timeout 90 "$W/tools/teleport" "$t" -411 3899 226 0 -8 --hold 6 >/dev/null 2>&1
  sleep 60
  if "$W/tools/npc-re/userplaying.sh"; then echo "stopping: the user is playing"; "$A" kill-game "$t" >/dev/null 2>&1; exit 2; fi
  if [[ ${HOVER:-0} == 1 ]]; then
    # hover over the stairs crowd (5 s of flight from Bayek), camera pitched down ~55 deg; the hover
    # view is blurred at the edges only, so the centre is a still shot of people below
    timeout 120 "$W/tools/npc-re/senu.sh" "$t" 145 5 30 0 0 > "$out/strobe-$tag-senu.txt" 2>&1 &
    sp=$!
    sleep 14
    "$A" keys "$t" mousemove_relative -- 0 300 sleep 0.2 mousemove_relative -- 0 200 >/dev/null
    sleep 4
  else
    timeout 120 "$W/tools/npc-re/senu.sh" "$t" 145 30 1 0 0 > "$out/strobe-$tag-senu.txt" 2>&1 &
    sp=$!
    sleep 8
    "$A" keys "$t" mousemove_relative -- 0 200 >/dev/null   # pitch down (-20 -> ~-34 deg)
    sleep 9
  fi
  if [[ ${FGTRACE:-0} == 1 ]]; then
    # the present trace: a band (rows 9/32 .. 9/32 + 1/16) of every present, real and generated
    rm -rf "$O/fgtrace"
    touch "$O/fgtrace-start"
    end=$((SECONDS + 60))
    until [[ -s $O/fgtrace/bands.bin ]] || ((SECONDS > end)); do sleep 1; done
    sleep 2
    rm -rf "$out/fgtrace-$tag" && cp -r "$O/fgtrace" "$out/fgtrace-$tag" 2>/dev/null
    python3 "$W/tools/fgstrobe.py" "$out/fgtrace-$tag" 24 "$out/fgstrobe-$tag.png" | tee "$out/fgstrobe-$tag.txt"
    rm -f "$out/fgtrace-$tag/bands.bin"
  fi
  rm -rf "$O/frames"
  echo 40 > "$O/framedump"
  end=$((SECONDS + 90))
  until grep -q 'framedump: wrote' <(tail -50 "$O/pharos.log") || ((SECONDS > end)); do sleep 1; done
  wait $sp
  n=$(ls "$O/frames" 2>/dev/null | wc -l)
  echo "== $tag ($kvs): $n frames, $(grep -o 'npc: [0-9]* spawned' "$O/pharos.log" | tail -1)"
  if ((n >= 4)); then
    python3 "$W/tools/strobe.py" "$O/frames" 24 "$out/strobe-$tag.png" | tee "$out/strobe-$tag.txt"
    magick "$(ls -d "$O"/frames/*.ppm | head -1)" "$out/strobe-$tag-0.png"
    # STROBE_KEEP=WxH+X+Y: keep that crop of the first 12 frames (for looking at shadows by eye)
    if [[ -n ${STROBE_KEEP:-} ]]; then
      mkdir -p "$out/strobe-$tag-frames"
      for f in $(ls "$O"/frames/*.ppm | head -12); do magick "$f" -crop "$STROBE_KEEP" +repage "$out/strobe-$tag-frames/$(basename "$f" .ppm).png"; done
    fi
  fi
  rm -rf "$O/frames"
  "$A" kill-game "$t" >/dev/null 2>&1
done
