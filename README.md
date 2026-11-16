# aco-dlss

NVIDIA DLSS 4.5 Super Resolution for **Assassin's Creed Origins** (PC, v1.62), replacing the game's
TAA. Built from source with mingw; no game code is patched (the exe is Denuvo + VMProtect protected),
everything happens at the D3D11 API level through a `d3d11.dll` proxy.

Status: DLSS SR working (all quality modes + DLAA). Reflex and Frame Generation are next.

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

## Build

```sh
scripts/fetch-deps     # NVIDIA DLSS SDK 310.9.16 (headers + nvngx_dlss.dll) into third_party/
scripts/package        # CMake + mingw-w64 -> build/mod/{d3d11.dll,nvngx_dlss.dll,aco_dlss.ini}
```

## Install

Copy `build/mod/*` next to `ACOrigins.exe`. Under Proton/Wine (Lutris), the game must load the
native `d3d11.dll` from its directory: add `WINEDLLOVERRIDES=d3d11=n,b` to the environment of the
Ubisoft Connect runner (the game inherits Connect's environment). NVAPI must be enabled
(`PROTON_ENABLE_NVAPI=1`, default in GE-Proton).

Set the render resolution with the game's *Resolution Scale* option (or `render_scale` in
`aco_dlss.ini`): 0.5 = Performance, 0.58 = Balanced, 0.667 = Quality, 1.0 = DLAA. Keep
*Adaptive Quality* off. See `dist/aco_dlss.ini` for all options; most are reloaded live.

## Development workflow (headless, Linux)

`scripts/aco` runs the game in podman containers (gamescope headless, NVIDIA via CDI) on
copy-on-write clones of the Ubisoft Connect prefix:

```sh
scripts/aco gold                 # snapshot the Lutris Connect prefix (credentials stripped)
scripts/aco create t1            # reflink clone (free on btrfs)
scripts/aco up t1                # boot Connect; logs in itself (see CREDS_FILE in the script)
scripts/package && scripts/aco game t1 --mod build/mod
scripts/aco-dt t1                # drive menus into Discovery Tour
scripts/aco shot t1 out.png      # screenshot; scripts/aco keys t1 <xdotool args> for input
touch ~/Games/aco-dlss-instances/t1/out/capture   # frame capture -> out/cap-NNN/
tools/passes.py <cap-dir>        # pass-level view of a capture; tools/capsum.py for per-draw
tools/bench t1 "label" 0.5 enabled=1 backend=dlss
tools/abtest t1 <crop> "a=key:val" "b=key:val"     # live A/B of aco_dlss.ini settings
```

Ubisoft login tokens are single-use with reuse detection (reusing one logs out every holder), so
instances never share tokens: each logs in with the stored password once and keeps its own chain.

## Roadmap

- Reflex (NVAPI low-latency markers) and DLSS Frame Generation (D3D11 has no native DLSS-G path;
  needs D3D11->D3D12/Vulkan interop or a Vulkan-level implementation under DXVK).
- Feed the engine's `ResponsiveAA` mask (particles) to DLSS as bias-current-colour mask.
- Longer jitter sequence for upscaling ratios (engine uses ~8-16 phases).
- HDR (pre-tonemap) DLSS placement, running tonemap/bloom at display resolution.
- Camera-cut detection for DLSS reset.
