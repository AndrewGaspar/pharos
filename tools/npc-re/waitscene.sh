#!/usr/bin/env bash
# waitscene.sh <instance> <out.png> [timeout s=120] [settle s=15] : wait until the display is not
# (nearly) black (loading screen), then let streaming settle and take a screenshot.
set -uo pipefail
t=$1 out=$2 to=${3:-120} settle=${4:-15}
A=$(cd "$(dirname "$0")/../.." && pwd)/scripts/aco
end=$((SECONDS + to))
until (( SECONDS > end )); do
  "$A" shot "$t" "$out" >/dev/null
  mean=$(magick "$out" -colorspace gray -format '%[fx:mean*100]' info: 2>/dev/null || echo 0)
  if awk "BEGIN{exit !($mean > 8)}"; then
    sleep "$settle"
    "$A" shot "$t" "$out" >/dev/null
    echo "scene ready (mean $mean)"
    exit 0
  fi
  sleep 3
done
echo "waitscene: timeout" >&2
exit 1
