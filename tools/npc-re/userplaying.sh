#!/usr/bin/env bash
# userplaying.sh : exit 0 (and print the pid) if the user's own game is running: an ACOrigins.exe
# outside the test containers (WINEPREFIX under ~/Games/ubi-prefixes/, or any ACOrigins process not
# in a libpod cgroup). Test scripts call this before every launch and refuse to start a game then.
for p in $(pgrep -x ACOrigins.exe); do
  [[ -r /proc/$p/cgroup ]] || continue
  grep -q libpod /proc/$p/cgroup 2>/dev/null && continue
  pfx=$(tr '\0' '\n' < /proc/$p/environ 2>/dev/null | sed -n 's/^WINEPREFIX=//p')
  if [[ $pfx == "$HOME"/Games/ubi-prefixes/* ]] || ! grep -q libpod /proc/$p/cgroup; then
    echo "user game running: pid $p ${pfx:+(WINEPREFIX $pfx)}"
    exit 0
  fi
done
exit 1
