#!/usr/bin/env bash
# gamepid.sh <instance> : host pid of ACOrigins.exe running in container aco-<instance>.
set -uo pipefail
id=$(podman inspect "aco-$1" --format '{{.Id}}' 2>/dev/null | cut -c1-12)
[[ -n $id ]] || { echo "gamepid: no container aco-$1" >&2; exit 1; }
for p in $(pgrep -f '^C:.*ACOrigins.exe'); do
  grep -q "libpod-$id" /proc/$p/cgroup 2>/dev/null && { echo "$p"; exit 0; }
done
echo "gamepid: game not running in aco-$1" >&2
exit 1
