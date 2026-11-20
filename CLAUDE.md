# Pharos

DLSS 4.5 / Reflex / DLSS Frame Generation / stereo / crowd mod for **Assassin's Creed Origins**
(Ubisoft, `ubisoft:3539`, `ACOrigins.exe`, AnvilNext, **Direct3D 11** under DXVK), packaged as a
Quiver mod with an options surface (`dist/quiver-options.toml`, surface `aco-dlss`).

## Layout

- `src/proxy.cpp`: the `d3d11.dll` proxy (needs `WINEDLLOVERRIDES=d3d11=n,b`); forwards to DXVK's
  d3d11 and hooks devices, factories, swapchains (Present runs every feature's per-frame hook).
- `src/core/`: log (`pharos.log` and debug outputs in `out_dir()`: `ACO_DLSS_OUT`, else the lab run
  folder `QUIVER_LAB_RUN_DIR`, else the game folder), config (`pharos.ini`, re-read every 64
  presents), lab channel (`lab.cpp`: `lab_state`, `lab_verb`, `on_present`), hooks, Quiver SDK.
- `src/dlss/`, `src/fg/`, `src/reflex/`, `src/stereo/`, `src/world/` (NPCs, LOD, teleport), `src/cloth/`.
- `dist/`: `pharos.ini` (shipped defaults) and `quiver-options.toml`. **Change them together** and
  lint: `quiver mods origins options lint build/mod`.
- `docs/`: ENGINE_NOTES.md (engine RE and the Tools section), STEREO.md, FRAME_GENERATION.md.
- `tools/lab/`: the lab harness (setup, launch, dt, screen, teleport, camera, profiles).

## Build and run (Quiver lab)

```sh
tools/npc-re/userplaying.sh && echo STOP   # exit 0 = the user is playing Origins: launch nothing
scripts/package                            # mingw-w64 -> build/mod/{d3d11.dll,nvngx_*.dll,pharos.ini,...}
tools/lab/launch --dt giza-sphinx [section.key=value ...]   # launch build/mod + ini overrides, menus -> Discovery Tour, teleport
quiver lab wait origins in-world           # Pharos's lab state: menu | in-world
quiver lab where origins                   # player and camera
quiver lab teleport origins <location>     # quiver lab teleport origins list; tools/lab/teleport also turns the camera
quiver lab set origins 'pharos:[stereo]mode=sbs' --wait     # live config change (waits for the reload line)
quiver lab call origins cfg section.key [value]             # read / set one key through the mod
quiver lab shot origins out.png --settle 2
quiver lab logs origins                    # run folder: pharos.log is there
quiver lab kill origins && quiver lab down origins          # when done
```

Rules: pass `--source agent:<name>` to quiver commands (the tools/lab scripts use
`$QUIVER_SOURCE`, default `agent:pharos-lab`). Follow `quiver lab brief 3539`: no pkill/pgrep -f,
poll real signals (`quiver lab wait`, `--wait`), `--settle` before screenshots, never touch the
user's real prefix or install, never sign in to stores or read credential files, never run Ubisoft
Connect. Don't change the user's lab limits, Quiver settings, profiles or mods, or other sessions'
instances (Alan Wake agents use the lab too: check `quiver lab host` before heavy runs).

The instance `origins` runs **1280x720@60 with a 30 fps cap** (Pharos `[reflex] fps_limit=30`
from the `lab` graphics profile; the game's own MaxFPS doesn't hold) unless a test needs
otherwise; for stereo/performance numbers use `display=1920x1080@60 graphics=measure` (see
ENGINE_NOTES, Tools) under `quiver lab gpu`. `tools/lab/setup` re-applies the instance config.
The virtual clock (`clock=virtual:...`) currently breaks Pharos's hooks: keep `clock=real`.

## Conventions

- Addresses in notes and code are RVAs of ACOrigins.exe v1.62; code checks vtables / signatures
  before using them and disables the feature on a mismatch.
- Every claim in `docs/` says how it was established (static, live, measured).
- Features are inert when their config is off; lab code is inert outside the lab.
