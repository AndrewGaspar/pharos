# Pharos

A from-source mod for **Assassin's Creed Origins** (PC, v1.62): NVIDIA DLSS 4.5 Super Resolution in
place of the game's TAA, NVIDIA Reflex, DLSS Frame Generation / Multi Frame Generation, and denser
city crowds. Named after the Lighthouse of Alexandria.

Development ports for Odyssey (Aegis) and Valhalla (Bifrost) are documented in [the port notes](docs/PORTS.md).

Built with mingw; it loads as a `d3d11.dll` proxy. The exe is Denuvo + VMProtect protected and is
never modified on disk: rendering changes happen at the D3D11 API level, and the world tweaks
(crowds, LOD, cloth) adjust engine data and object vtables in memory at runtime.

Status: DLSS SR (all quality modes + DLAA), NVIDIA Reflex and DLSS Frame Generation (D3D12 route via
vkd3d-proton, F9 toggle) working; see [docs/FRAME_GENERATION.md](docs/FRAME_GENERATION.md).
Game-data tweaks: `[world] npc_density` / `npc_distance` (bigger crowds: the engine's spawn cap
and spawn distance), `[world] lod_distance`, `[cloth] mode=render`; see `dist/pharos.ini` and
docs/ENGINE_NOTES.md.

## Results (RTX 5070 Laptop, 2560x1600, Discovery Tour / Siwa village, uncapped)

Render scale 0.5 (DLSS Performance, 1280x800 -> 2560x1600), same view, GPU-bound:

| Mode | FPS | Upscaler GPU cost |
|---|---|---|
| No AA, bilinear upscale (reference) | 160 | 0.05 ms |
| Game TAA | 150 | — |
| DLSS, preset K (DLSS 4) | 140 | 1.6 ms |
| DLSS, preset M (DLSS 4.5) | 129 | 2.2 ms |

(Numbers before commit f6383a3 were inflated: a constant-buffer shadowing bug made ~70% of
frames silently fall back to the game's TAA.)

At the same render resolution DLSS is far cleaner than the game's TAA (which, below 100% scale,
is simply bilinear-upscaled by the UI composite).

### Latency (Ultra High, DLSS Performance, ~118 fps GPU-bound; `tools/latency-bench`)

Median input → frame finished on the GPU (mouse-look injection, 40 samples per fresh launch;
display scanout not included):

| | Reflex off | Reflex on |
|---|---|---|
| DLSS Performance | 48.6 ms | **36.5 ms** (no fps cost) |
| Game TAA (CPU-bound, 144 fps) | 27.0 ms | 26.5 ms (142 fps) |

Press **F10** in game (`toggle_key`) to switch between DLSS and the game's own TAA live.

Press **F9** in game (`[fg] toggle_key`) to turn DLSS Frame Generation on and off live (with
`[fg] mode=d3d12`, the default; `[fg] enabled` sets the state at start). Off presents each frame
straight away like `mode=off`, so the latency difference can be felt directly.

DLSS Multi Frame Generation (RTX 50): `[fg] count=2..6` sets the multiplier (frames presented per
real frame, up to 6x with the SDK's nvngx_dlssg 310.9.16), `count=auto` picks it per second so that
real fps x multiplier reaches the display refresh (`[fg] target_hz`, default the detected refresh;
the real rate is capped at refresh / multiplier). See docs/FRAME_GENERATION.md, "Multi Frame Generation".

## How it works

See [docs/ENGINE_NOTES.md](docs/ENGINE_NOTES.md) for the reverse-engineered frame structure.

1. **Pass classification** — shaders are classified at creation by names in their DXBC reflection
   data (`TemporalAA_*`, `ConvertLinearZParams_*`, `UICompositingParameters_*`), so all ~60 TAA
   permutations are recognised without hashes.
2. **TAA interception** — the TAA resolve draw (recorded on a deferred context) is skipped. In its
   place we record:
   - our motion-vector kernel, using the TAA pass's *own* constant buffers: decodes the engine's
     packed RGBA8 velocity buffer and reprojects sky/far pixels with the camera matrices;
   - a copy of the input colour into the TAA output (the next frame's lighting reads it);
   - a **command-list split** (`FinishCommandList(TRUE)` mid-recording).
3. **DLSS evaluation** — when the split list is executed on the immediate context, DLSS runs
   between the two halves (on a private `ID3DDeviceContextState`), inputs: tonemapped LDR colour,
   raw reversed-Z depth, motion vectors, the engine's own sub-pixel jitter.
4. **Composite** — the UI composite samples the scene with UVs, so swapping in the
   display-resolution DLSS output is all that's needed.
5. **Texture LOD bias** — material samplers get `log2(render/display)` bias.

NGX is loaded dynamically from the driver (`_nvngx.dll`); NVIDIA's static loader library (MSVC
only) isn't used. `NVSDK_NGX_Parameter` is called through its MSVC vtable layout explicitly.

### Reflex

The game has no Reflex support, so it is injected at the frame boundary we control: right after
`Present` returns, the render thread calls `NvAPI_D3D_Sleep`, and the frame is delimited with
latency markers (`SIMULATION_START` after the sleep … `PRESENT_START/END`, one app frame per
Present). Under Proton this goes NVAPI (dxvk-nvapi) → DXVK's D3D11 low-latency device →
`VK_NV_low_latency2` in the driver. The sleep holds the render thread back just enough that the
frame reaches the GPU as it frees up, so the GPU render queue (~2 frames when GPU-bound) drains.

Markers matter: without them DXVK must make each sleep wait for its own submission thread to
finish the previous frame, which costs ~20% frame rate when the game is CPU-bound.

What injection can't reach is the game's own simulation → render pipeline (~3 frames from input
to the frame being recorded): the simulation thread's frame start isn't visible at the API level
(see docs/ENGINE_NOTES.md, "Threads and input").

## Build

```sh
scripts/fetch-deps     # NVIDIA DLSS SDK 310.9.16 (headers + nvngx_dlss.dll) into third_party/
scripts/package        # CMake + mingw-w64 -> build/mod/{d3d11.dll,nvngx_dlss.dll,pharos.ini}
```

## Install

Copy `build/mod/*` next to `ACOrigins.exe` (settings: `pharos.ini`, log: `pharos.log`; an
older install's `aco_dlss.ini` is still read if there is no `pharos.ini`). Under Proton/Wine (Lutris), the game must load the
native `d3d11.dll` from its directory: add `WINEDLLOVERRIDES=d3d11=n,b` to the environment of the
Ubisoft Connect runner (the game inherits Connect's environment). NVAPI must be enabled
(`PROTON_ENABLE_NVAPI=1`, default in GE-Proton).

Set the render resolution with the game's *Resolution Scale* option (or `render_scale` in
`pharos.ini`): 0.5 = Performance, 0.58 = Balanced, 0.667 = Quality, 1.0 = DLAA. Keep
*Adaptive Quality* off. See `dist/pharos.ini` for all options; most are reloaded live.

## Development workflow (headless, Linux)

Tests run in a Quiver lab instance (`quiver lab`, a disposable headless clone of the installed game
in podman with gamescope and the GPU, licensed through quiverd; see `CLAUDE.md` and
docs/ENGINE_NOTES.md, Tools):

```sh
tools/lab/setup origins            # once: configure the instance for Pharos (d3d11=n,b, build/mod, aliases)
scripts/package
tools/lab/launch --dt giza-sphinx  # launch with build/mod, into Discovery Tour, teleport to Giza
quiver lab where origins           # player and camera
quiver lab set origins 'pharos:[reflex]fps_limit=60' --wait   # live pharos.ini edit
quiver lab shot origins out.png --settle 2
quiver lab logs origins            # the run folder: pharos.log, captures, DXVK and game logs
```

The old podman harness (`scripts/aco`, `aco-t*` containers) is deprecated.

## Roadmap

- Reflex (NVAPI low-latency markers) and DLSS Frame Generation (D3D11 has no native DLSS-G path;
  needs D3D11->D3D12/Vulkan interop or a Vulkan-level implementation under DXVK).
- Feed the engine's `ResponsiveAA` mask (particles) to DLSS as bias-current-colour mask.
- Longer jitter sequence for upscaling ratios (engine uses ~8-16 phases).
- HDR (pre-tonemap) DLSS placement, running tonemap/bloom at display resolution.
- Camera-cut detection for DLSS reset.
