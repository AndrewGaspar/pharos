# Assassin's Creed Origins (AnvilNext 2.0, v1.62) — rendering notes

Everything here was derived from frame captures taken with this mod's tracer (`touch <out>/capture`)
and from reflection data still embedded in the game's DXBC shaders. Resource names below are the
HLSL names from that reflection data.

## Executable

- `ACOrigins.exe` is protected by Denuvo (`.arch`, `.srdata`, `.xtls`) and VMProtect
  (`.vmp0`, `.vmp1`). We therefore don't patch code; everything happens at the D3D11 API level.
- Imports only `D3D11CreateDevice` and `CreateDXGIFactory1`; a `d3d11.dll` next to the exe is
  loaded instead of the system one (Proton: `WINEDLLOVERRIDES=d3d11=n,b`).
- Creates 3 D3D11 devices (two probes, then the real one) and 9 deferred contexts. Almost all
  rendering is recorded on deferred contexts and executed on the immediate context.

## Frame structure (post-processing end of the frame)

| Pass | PS identification | Output |
|---|---|---|
| G-buffer | many | RGBA8_SRGB albedo, RGBA8 x3, R11G11B10F, D32S8 |
| Linear Z (`ConvertLinearZParams_DepthBufferSampler`) | | RT0 = linear view Z (R32F), RT1 = raw device depth copy (R32F) |
| Lighting / transparents / half-res particles | | HDR scene colour RGBA16F |
| Bloom chain | | RGBA16F mips |
| Tonemap (`HdrLightingPass_*`, ACES 3D LUT) | | **LDR RGBA8** + R8 "responsive AA" mask |
| **TAA resolve** (`TemporalAA_*`) | | RT0 = output, RT1 = colour history, RT2 = responsive history |
| UI composite (`UICompositingParameters_*`) | | swapchain-sized; samples the scene with UVs (trilinear) |

TAA therefore runs **after tonemapping, in LDR**, at render resolution. With `PixelDensity < 1`
everything up to and including TAA runs at render resolution and the UI composite upscales
bilinearly. The TAA output is also read by the *next* frame's lighting passes (SSR-style reuse).

Tonemap output / TAA output / TAA input rotate through 3 RGBA8 textures; TAA histories ping-pong.

## TemporalAA resolve

Bindings: `t86 TemporalAA_Color`, `t87 TemporalAA_Motion`, `t88 TemporalAA_Depth` (linear Z),
`t89 TemporalAA_ResponsiveAA`, `t90 TemporalAA_ColorAccumulation`, `t91 ..._ResponsiveAAAccumulation`;
`cb1 PassParams`, `cb3 InstanceParams`, `cb5 TemporalAA`. ~58 permutations exist; all share the names.

`TemporalAA_cbuffer` (128 bytes):

```
uint4  IsolationVars;            //  0
float2 FullResSize, FullResSizeInv;  // 16
float2 LowResSize,  LowResSizeInv;   // 32
float2 SubPixelJitter;           // 48  -- pixels, Halton-like; directly usable as DLSS jitter
float3 NoiseOffsetScale; float RejectionAvoidanceMax; ...
float4 MaxViewDepthParams;       // 112 -- (x-y)/(z-w) = linear-Z threshold for "far" pixels
```

Motion buffer (`t87`, RGBA8) encodes `currentUV - previousUV`:

```
g = G*255/16;  x = R*4080 + floor(g);  y = frac(g)*4096 + B*255
v = clamp(xy * 2/4095 - 1, -1, 1);  motion = sign(v) * 0.2 * v^4
```

Pixels with linear Z beyond the `MaxViewDepthParams` threshold (sky) use camera reprojection instead:
clip -> `RevProjParams.ClipXYZToViewPos` (cb1[57..60]) -> `viewToWorld` (cb1[4..6]) ->
`InstanceParams.worldViewProjPrevFrame` (cb3[12..15]).

## Depth / projection

Reversed-Z with an infinite far plane: `projMatrix` z row = `(0, 0, 1.67e-6, 0.1)`, w = -z_view;
`zFrontBackValue = (1, 0)`. DLSS gets the raw depth copy (LinearZ RT1) with `DepthInverted`.

`PassParams.gradientAdjustMipBias` stays 0 when rendering below native resolution: the engine does
not bias texture LOD itself.

## Constant buffers

CB contents are written with `Map(WRITE_DISCARD)` on deferred contexts; draws bind ranges with
`*SetConstantBuffers1`. The tracer/upscaler shadow writes per (context, buffer) to read values at
record time (`core/cbshadow`).

## Threads and input (for latency work)

- **Window/message thread**: busy-polls `PeekMessage` (~15,000 calls per frame). Mouse motion
  arrives as `WM_INPUT` and is read there with `GetRawInputData`, then buffered for the simulation.
- **Simulation thread**: consumes the buffered input at the start of its frame. It makes no user32
  calls, so its frame boundary isn't visible at the API level.
- **Render thread**: records on deferred contexts (9 worker threads) and calls `Present`.
- The game does **not** call user32 through its import table (Denuvo/VMProtect resolve imports
  privately). Wine's user32 calls win32u through *its* IAT, so hooking user32's imports
  (`NtUserPeekMessage`, `NtUserGetRawInputData`, ...) does see every call.

Measured input-to-frame latency (mouse-look injected through XTEST, camera change detected from
`PassParams.viewToWorld`; `tools/latency`): the game needs ~3 frames (22-29 ms at 115-145 fps)
from an input event to recording the frame that shows it (input buffering + simulation +
render-thread handoff). When GPU-bound, DXVK's present queue adds ~2 more frames, which is what
Reflex removes.

## Cloth simulation rate

MotionCloth (Ubisoft's cloth; `MotionClothComponent`, reflection size 0x610, vtable RVA 0x3686e48)
is simulated from the `UpdateMotionComponentsSim` job (job graph set up at RVA ~0xa690f0; per
component callback RVA 0x1536700 -> 0x1536a80) with a **fixed timestep chosen by cloth LOD**:

```
lod  = word [owner + 0x184] & 7, clamped to 3
step = interval[lod] * timescale(+0x378) * 0.5,  interval = {1/30, 1/20, 1/15, 1/10}  (RVA 0x368c4d0)
       -> 60 / 40 / 30 / 20 Hz
now = world clock (+0x36c); last = cloth's last sim time (+0x94)
while now >= last + step (at most 3 substeps; more than 3 steps behind -> larger step): solve(step)
```

The rendered cloth is not interpolated between steps, so above 60 fps even the nearest cloth moves
at a lower rate than everything else (every other frame at 120 fps), and NPC cloth at 30-20 Hz.
The per-frame clock the job uses (`[RVA 0x48b4a10] + 0x18`, with 1/dt at +0x1c and the previous
dt at +0x20) is the real frame delta; the throttle is only this table.

`[cloth] mode=render` rewrites the table each frame (min(original, 2 * 0.8 * frame time)) so every
cloth steps at least once per rendered frame. Verified with `mode=scale scale=20` (cloth visibly
freezes for ~0.3 s between steps). The code is ordinary (not virtualized); the patch only touches
the read-only data table after checking its values and the referencing instruction.

## Graphics settings and draw distance

- The ACO.ini quality keys are a 24-entry settings table (descriptor table at RVA 0x49af000, 0x2c
  bytes each: level count, default, flag, name). Current levels live in a settings object
  (`levels[idx]` at +0x1c + 4*idx; getter RVA 0xa58ec0, setter 0xa668b0); the applied copy is a
  global array at RVA 0x48717b0 (Environment = idx 10 at 0x48717d8, Shadow = 16 at 0x48717f0).
- **Environment** only scales streaming memory pools: x1 for levels 0-3, x2 for 4-5 (RVA 0x89a970,
  reader `GetEnvironmentLevel` 0x8983c0). It is not a distance scale.
- **Clutter** maps to a clutter spacing factor (RVA 0x13cd0f0: 100 / 2 / 1.25 / 1 / 0.8 for levels
  0-4; lower = denser).
- Character, Shadow, CrowdDensity, ... are not read by engine code directly; CrowdDensity, Culling,
  Lighting, Particle, PostEffect and Wind have a single level on PC.
- The ~45 `DisplayOptionSection` classes (LODOptions, ShadowRenderSettings, CullingOptions, ...;
  vtables mapped in tools/lod-re/children.py) exist as 4 identical template blocks + 1 active
  copy, identical across quality presets, and are not read live (writes have no effect).
- **World geometry LOD / culling is GPU-driven**: ~16k indirect draws per frame; the instance
  culling compute shaders (12 permutations, cbuffer `MiscCullingConsts` at b8) compute
  `d' = (dist * objScale * FOVDistanceFactor + GeometricObjectLODDistanceBias) *
  GeometricObjectLODDistanceScale` and select/cull by the mesh's LOD ranges. Game values: scale 1,
  bias 0, FOV factor ~1.0055, MinTriangleArea 0. `[world] lod_distance` patches the scale.
- Characters (NPCs) are not part of that GPU path (they stay visible when world LOD is cut).
- Shadow cascades are set up on the CPU (shaders only see cascade matrices / fade params).

## Player entity, camera and teleport (`src/world/teleport.cpp`, `tools/teleport`)

World coordinates are metres, **Z up**; Alexandria is around (-700, 3570), Giza (900, 160), Memphis
(1470, -1130), Siwa (-6400, -260), Cyrene (-5780, 3720). Heading convention used by the tools:
yaw 0 = +X, 90 = +Y.

- **Entity** (AnvilNext `Entity`, Anvil TypeInfo size 0x250, vtable RVA **0x365e460**, shared by
  ~37k world entities): world matrix at **+0x20** as 4 float4 rows: right (+0x20), forward (+0x30),
  up (+0x40), **position (+0x50**, w = 1). Found by scanning for float3 copies of a point next to
  the camera (`memtool fs/fc/fr`) and bisecting writes over the ~200 copies that follow the player:
  only the entity's +0x50 moves the player; every other copy (components with their own matrices,
  e.g. a visual component whose +0x20 points back to the entity, nav/AI proxies) is derived from it
  each frame.
- **Static path to the player**: `[[base + 0x4c23c40] + 0] = player entity` (also
  `[[base + 0x4c5d788] + 0x78]`). Both survive restarts and the entity being recreated (respawn);
  found with the reverse pointer scan (`memtool pp/pn`), validated over several launches. Other
  level-2 roots pointed at the player in one run only (e.g. `[base + 0x48bdd10] + 0x278`).
- **Writing the position** from the render thread (Present) works, with caveats: the simulation
  thread updates the entity concurrently, so a write is lost now and then (re-check after a few
  frames and write again). Rewriting it every frame instead fights the character controller: it
  derives motion from the displacement and the player was flung hundreds of metres or the game
  respawned it through the Animus loading void at (-100, 33.5, 0.2). Single writes over any
  distance (tested 4-5 km, region to region) are stable; streaming follows on its own. Placed exactly
  on the ground, the player's model stays invisible (only its shadow is drawn) until the character
  moves, so the player is placed 1 m above the ground and lands. The rotation rows are overwritten
  by the character controller (writing a heading has no effect).
- **Story mode** behaves the same (same entity class, same static chains; 4.8 km Alexandria ->
  Memphis tested). The table coordinates belong to the **Egypt** world: the DLC regions are separate
  worlds with their own coordinates (The Hidden Ones = Sinai; the user's latest save is there), and
  placing the player at Egypt coordinates in Sinai drops it through empty terrain until the game
  respawns it. Also the story's terrain differs in places (Giza's sand is ~20 m higher than in the
  Discovery Tour, so the DT Sphinx point is underground). Once the player is airborne the
  character's physics body owns the position (entity writes stop taking, also when rewritten every
  frame), so a bad target can't be rescued; the game respawns the player at a nearby safe point.
  World switch: map -> Atlas (Tab) -> hover a region's fast-travel icon -> hold right mouse.
- **Saves**: the game keeps everything in `savegames/<account>/3539/{1..6}.save` (pairs of file +
  backup): 1/2 profile/settings (2.5 KB), **3/4 the story** (~108 KB, autosaved e.g. on entering a
  named location, which a teleport does), 5/6 Discovery Tour state (~40 KB, written on exit).
  There is no manual save on PC. `scripts/aco fixture-save` / `scripts/aco fixture` archive and
  install all six; `scripts/aco-dt <inst> --story` continues the first profile.
- **Arrival**: the player hangs at the target while collision streams in (about 1 s across the
  map), then falls the 1 m. Textures/LODs are complete within ~5 s at 768x480. NPCs keep spawning
  for longer (compare with the same place after the game's own fast travel).
- **Render camera**: global pointers at RVA **0x49e86b8** and 0x49f39d8 (object vtable RVA
  0x36afd88): position at +0x130, unit forward vector at +0x140, then the view rotation; this is what
  `PassParams.viewToWorld` is built from. The camera controller's yaw/pitch were not found as plain
  floats (no stored heading vector either), so `tools/camera` turns the camera with injected
  relative mouse motion in a closed loop on this readout (~0.1 deg/px yaw, ~0.07 deg/px pitch,
  with smoothing; converges to 0.5 deg in 2-4 iterations).
- Game-side alternatives seen in the strings (not used): `FastTravelManager`, `FastTravelCommand`,
  `TeleportEvent`, `ActionPlayerTeleportAndRespawn`, `DiscoveryModeTeleportToTourEvent`.
  Discovery Tour fast travel itself: map (M), hover a tour/viewpoint icon, **hold right mouse**.
- Time of day keeps cycling in Discovery Tour (`TimeOfDayManager2`, TypeInfo RVA 0x42eff68, size
  0x3c0, has no constructor/vtable link in its TypeInfo); not controlled yet.
