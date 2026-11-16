#!/usr/bin/env bash
# waitunpause.sh : wait until aco-t1 is not paused (another workstream's gpu-exclusive)
for i in $(seq 120); do
  s=$(podman ps -a --format '{{.Names}} {{.Status}}' | grep '^aco-t1 ')
  [[ $s == *Paused* ]] || { echo "$s"; exit 0; }
  sleep 5
done
echo "still paused"; exit 1
