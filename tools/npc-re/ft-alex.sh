#!/usr/bin/env bash
# ft-alex.sh <instance> : from Siwa (Discovery Tour start), fast travel to the "Alexandria: A Commercial
# Hub" tour station via the map (768x480 instance coordinates), then wait for the scene.
set -uo pipefail
t=$1
here=$(cd "$(dirname "$0")" && pwd)
A=$here/../../scripts/aco
S=${SHOTS:-/tmp/claude-1000/-home-ajg-code-ac-dlss/249d0810-7bc6-42cc-9509-5599a9b80e14/scratchpad/npc}
res=$(cat "/home/ajg/Games/aco-dlss-instances/$t/res" 2>/dev/null || echo 2560x1600)
W=${res%x*}
sc() { echo $(( $1 * W / 768 )) $(( $2 * W / 768 )); }  # 768x480 reference coordinates
k() { "$A" keys "$t" "$@"; }
k keydown m sleep 0.15 keyup m; sleep 4
k mousemove $(sc 600 400) sleep 0.3; for i in $(seq 14); do k click 5 sleep 0.25; done; sleep 1
k keydown d keydown w sleep 3 keyup w keyup d; sleep 0.5
k keydown s sleep 1.2 keyup s; sleep 0.5
k mousemove $(sc 500 250) sleep 0.3; for i in $(seq 5); do k click 4 sleep 0.3; done; sleep 2
k mousemove $(sc ${HX:-528} ${HY:-233}) sleep 1.5
"$A" shot "$t" "$S/ft-hover.png" >/dev/null
[[ ${NOGO:-0} == 1 ]] && exit 0
k mousedown 3 sleep 2.5 mouseup 3; sleep 5
"$here/waitscene.sh" "$t" "$S/ft-arrived.png" 150 20
