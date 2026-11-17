#!/usr/bin/env bash
# sweep.sh <instance> <settle s> <density>... : set [world] npc_density live for each value, let the
# population settle, then report spawned count, fps, reflex sleep (GPU-bound slack), the game's
# VRAM, the busiest threads' CPU use, and a screenshot. Restores npc_density=1 at the end.
# Run under tools/gpu-exclusive for performance numbers.
set -uo pipefail
t=$1 settle=$2; shift 2
here=$(cd "$(dirname "$0")" && pwd)
W=$here/../..
I=/home/ajg/Games/aco-dlss-instances/$t
INI="$I/prefix/drive_c/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/games/Assassin's Creed Origins/pharos.ini"
L=$I/out/pharos.log
S=${SHOTS:-/tmp/claude-1000/-home-ajg-code-ac-dlss/249d0810-7bc6-42cc-9509-5599a9b80e14/scratchpad/npc/sweep}
mkdir -p "$S"
pid=$("$here/gamepid.sh" "$t") || exit 1
grep -q '^npc_density=' "$INI" || sed -i '/^\[world\]/a npc_density=1' "$INI"
grep -q '^npc_distance=' "$INI" || sed -i '/^\[world\]/a npc_distance=1' "$INI"
threads() {  # top threads by CPU over 10 s: name %cpu
  declare -A a
  for f in /proc/$pid/task/*/stat; do read -r -a s < "$f" 2>/dev/null || continue; a[${f%/stat}]=$(( ${s[13]} + ${s[14]} )); done
  sleep 10
  for f in /proc/$pid/task/*/stat; do
    read -r -a s < "$f" 2>/dev/null || continue
    k=${f%/stat}; [[ -n ${a[$k]:-} ]] || continue
    d=$(( ${s[13]} + ${s[14]} - ${a[$k]} ))
    echo "$d $(cat "$k/comm" 2>/dev/null | tr ' ' _)"
  done | sort -rn | head -6 | awk '{printf "%s:%d%% ", $2, $1/10}'   # 100 ticks/s over 10 s -> % = d/10*10
}
for arg in "$@"; do  # <density>[:<distance>]
  d=${arg%%:*}; dist=1; [[ $arg == *:* ]] && dist=${arg#*:}
  sed -i "/^\[world\]/,\$ s/^npc_density=.*/npc_density=$d/; /^\[world\]/,\$ s/^npc_distance=.*/npc_distance=$dist/" "$INI"
  sleep "$settle"
  cnt=$(ACO_INST=$t python3 "$here/spawncount.py" 3 5 | awk '{split($2,a,"="); s+=a[2]} END {printf "%.0f", s/NR}')
  fps=$(grep -E "perf: [0-9.]+ fps" "$L" | tail -4 | sed 's/.*perf: \([0-9.]*\) fps (\([0-9.]*\) ms avg, \([0-9.]*\) ms worst.*/\1 \3/' |
        awk '{f+=$1; if ($2>w) w=$2} END {printf "%.1f fps (worst %.0f ms)", f/NR, w}')
  rs=$(grep "reflex sleep avg" "$L" | tail -4 | sed 's/.*avg \([0-9.]*\) ms.*/\1/' | awk '{s+=$1} END {printf "%.2f", s/NR}')
  vram=$(nvidia-smi --query-compute-apps=pid,used_memory --format=csv,noheader,nounits | awk -F', ' -v p=$pid '$1==p {print $2}')
  th=$(threads)
  "$W/scripts/aco" shot "$t" "$S/$t-density-$d-dist-$dist.png" >/dev/null
  echo "npc_density=$d npc_distance=$dist: spawned=$cnt | $fps | reflex sleep ${rs} ms | VRAM ${vram} MiB | threads(10s) $th"
done
sed -i "/^\[world\]/,\$ s/^npc_density=.*/npc_density=1/; /^\[world\]/,\$ s/^npc_distance=.*/npc_distance=1/" "$INI"
