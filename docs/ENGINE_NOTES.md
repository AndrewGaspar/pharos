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
  bias 0, FOV factor ~1.0055, MinTriangleArea 0. `[world] lod_distance` extends the far end of
  the LOD ranges (see "LOD ranges and the lod_distance gap" below).
- Characters (NPCs) are not part of that GPU path (they stay visible when world LOD is cut).
- Shadow cascades are set up on the CPU (shaders only see cascade matrices / fade params).

### LOD ranges and the lod_distance gap (planters missing near the camera)

Per entry the LOD shaders (7 of the 13 culling permutations: 16d3da0d87a4a82a, 2247ab14c6f61ec4,
62365b83ef726f77, 8c1d007b8c1ffcfb, b38632d3a98fe938, cd4f32cb6d3c8302, f34dc216bf3ed4ae) load a range
`R = t81[...]` with **R.y = min, R.z = max, R.w = fade-in width** (next element .x = fade-out
width) and, with d' as above times a per-entry factor:

```
cull     if d' < max(R.y - R.w, 0)  or  d' >= R.z
fade in  over [R.y - R.w, R.y), fade out over [R.z - fadeOut, R.z)   (dither value written to u3)
```

Entries with min > 0 are representations that exist only from a distance (lower LODs and merged
far proxies). The near representation that replaces them is **not** governed by this test alone:
it is only there once the CPU side has streamed/submitted it, by real distance. The old
`lod_distance=k` (dividing GeometricObjectLODDistanceScale) moved the min end too, so a far proxy
hid at k x its distance while its replacement still arrived at the game's distance: nothing is
drawn in between.

Reproduced in story Alexandria (street east of the DT spawn, x -600..-550, y 3572; 1280x800,
DLSS Performance): with `lod_distance=2` the stone planter pedestal (the bowl on top is a separate
object and stays, floating) is missing from ~60 m down to ~43 m and present again closer; at 1.5
it is dithered (fade zone). Identical with DLSS off (game TAA), with npc_density=3 / npc_distance=2
/ cloth=render on or off (those don't touch geometry), and fine at `lod_distance=1`.
`docs/img/lod_distance-planter-gap.jpg`: rows lod 1 / old lod 2 / fixed lod 2, two repeats.

Fix (`[world] lod_mode=far`, default): the LOD shaders are patched instead of the cbuffer. At the
first use of the dispatch's culling shader a copy is created with one instruction inserted after
the LOD distance (`mul X, X, cb8[35].w`): `mul R.z, R.z, l(k)` (R found as the temp read by the
first `ge` after it; DXBC container checksum recomputed, src/world/lodpatch.cpp), bound around that
dispatch only. Only the max of every range (incl. the final cull distance) moves out; min ends and
fade-ins stay at the game's distances, so no representation is hidden before its replacement
exists. k < 1 (and `lod_mode=all`) keeps the old cbuffer scaling. Verified at the same spots: the
pedestal stays visible at every distance with `lod_distance=2` and 3. Shadows of foliage reach
further as before (e.g. leaf shadows on near planters). No double-drawn LOD pairs were visible in
street/vista views; if one shows up it would be an entry whose lower LOD is submitted at the same
time (its fade-in no longer meets the extended fade-out).

Other resolution-related culling constants (logged per shader by `world_on_present`):
`VPosToUV` = (1/w, 1/h, w, h) of the **render** resolution (640x400 at 1280x800 Performance; half
that for the HZB-sized permutations, 4096 for shadow maps). The HZB permutations (CullingFlags & 4)
cull an instance whose projected bounds don't straddle a pixel centre at that resolution, i.e.
objects below ~1 render pixel (2 display pixels at Performance). That is the only screen-size test
and it only affects sub-pixel objects; it is not what hid the planters.

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
  There is no manual save on PC. Loading puts the player at the region's spawn anchor (Alexandria:
  -749.7 3574.2, the same point Discovery Tour starts at), not the exact saved position.
  `scripts/aco fixture-save` / `scripts/aco fixture` archive and install all six;
  `scripts/aco-dt <inst> --story [location]` continues the first profile (and teleports).
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

## NPC population

Found with host-side tools in `tools/npc-re/` (live memory through `/proc/<pid>/mem`, gdb hardware
watchpoints/breakpoints with the game running, headless Ghidra on the image dump; see "Tools" below).

### What spawns ambient NPCs

- Civilians, workers, guards and animals are units of the **MetaAI** virtual population
  (`MetaAIVirtualObjective*`, ~25k static objective nodes loaded around the player). The MetaAI
  manager (World+0x1688, vtable RVA 0x377e7e8; update RVA 0x1daf5f0) activates an objective when the
  player is within `m * r[0] + r[2]` (deactivates beyond `m * r[1] + r[2]`; per-objective ranges,
  e.g. {80, 82, 40} m; RVA 0x1d8e4e0) and registers its units as **spawn candidates** with the
  SpawningManager's sources (call chain 0x1daf9b4 -> ... -> 0x123b650 -> source vtable +0x78).
- The **SpawningManager** (World+0x13c0 -> +0x38, vtable RVA 0x363beb0; job
  `Ai::SpawningManagerSpawningUpdate`, RVA 0x12685c0) owns 6 spawning sources ("strategies": sphere,
  blob, bulk, ingredient, rideable...) at +0x18. Each frame it asks the sources to spawn up to
  `SpawnSettings+0x68` (1) entities (RVA 0x12683f0 -> source vtable +0xd8). The main source (vtable
  RVA 0x37a5280, spawn RVA 0x1ec42d0) spawns candidates within their template's spawn distance
  (`SphereStrategyParams` +0x2c / +0x30: spawn / despawn distance, 80 / 82 m for most of the ~1500
  templates). Budget pressure: from 0.8 * cap the spawn distance is scaled by 0.9, from 0.9 * cap by
  0.8, at the cap nothing spawns (constants at RVAs 0x356d60c / 0x356d668).
- **The cap** (RVA 0x1268e30 evicts above it, 0x1259050 / 0x12683f0 stop spawning at it):
  `count >= SpawnSettings.maxSpawned (+0x38) * multiplier(quality)`. The multiplier (RVA 0x12559c0)
  is picked on PC by the **Environment** quality level (settings index 10) from +0x4c..+0x60
  (0.75, 0.875, 1, 1, 1.1, 1.2 in the shipped data), so Ultra = 200 * 1.2 = 240. The count is the
  sum of the manager's pool list (+0x58: {entries, n << 17}, 0x28-byte entries, count at +0x10).
- `SpawnSettings` (vtable RVA 0x363cc80, size 0xa8; live object = World+0x1648 = SpawningManager
  +0x10; reached in-process via `[RVA 0x4c27f18] + 0xc0` = World) is a data object, not saved.
  Other fields: +0x6c despawns per frame (1), +0x70 spawn-distance multiplier (1.0; scales every
  MetaAI objective's activation range, and a candidate's spawn distance only when the candidate is
  inside the camera frustum: RVA 0x12f9150 tests against the camera's planes at +0x310 with a 1.2
  margin), +0x7c 40, +0xa0 flag.
- Measured: the cap binds in every populated place tried. Siwa (Discovery Tour start) sits at 231
  spawned with 200 * 1.0 (t3, Environment=5 profile in 768x480 shows multiplier 1.0 at that
  moment); Alexandria's market at night held ~220 and climbed to ~400 within 2 minutes once the cap
  was lifted (the source then stops at its demand). Lowering the cap again evicts down to it within
  seconds.

### Demand: what limits the population once the cap is lifted

With the cap out of the way the count stops where the sources run out of candidates within spawn
distance (Alexandria by day, 2560x1600, t1):

| change (live) | spawned |
|---|---|
| game (cap 240) | 191-218 |
| cap x5 (`npc_density=5`) | 337-359 |
| + SpawnSettings+0x70 = 2 | ~410 |
| + SpawnSettings+0x70 = 4 | ~425 |
| cap x5, +0x70 = 2, SphereStrategyParams spawn/despawn x2 (`npc_distance=2`) | 495-570 |

The template distances are what matter (the +0x70 multiplier only extends spawning inside the
view frustum). Beyond that, more NPCs would need more MetaAI units per objective (spawner data:
`MetaAIObjectiveSpawnerCustomData`, 3962 instances, unit lists at +0x7c) or more stations, i.e.
changing what the virtual population is built from at load; not attempted.

### Mod settings (`src/world/npc.cpp`) and measurements

- `[world] npc_density=k`: SpawnSettings+0x38 = round(game value * k), checked every second
  (re-applied after a world reload; restored at k=1). Pointer chain validated by vtables, the three
  readers' instruction bytes are checked once (v1.62 only; otherwise disabled).
- `[world] npc_distance=k`: SpawnSettings+0x70 = 1.0 * k, plus a background thread (idle priority,
  every 10 s while active) that finds `SphereStrategyParams` (vtable RVA 0x37a48f8, +0x28 == 4) in
  private RW memory and writes spawn/despawn distance = original * k (originals tracked per object;
  all reads/writes through Read/WriteProcessMemory so memory freed meanwhile cannot fault). ~700-950
  templates live at a time in Alexandria.

2560x1600, DLSS Performance + FG, Ultra High (Environment=5 -> game cap 240), t1 under
`tools/gpu-exclusive`, 120 s settle per step, fixed camera:

| place | density / distance | spawned | real fps | game VRAM |
|---|---|---|---|---|
| Story, Alexandria harbour market (fixture `story-alexandria-port`) | 1 / 1 | 208 | 85.2 | 4915 MiB |
| | 3 / 1 | 343 | 84.8 | 4979 MiB |
| | 5 / 2 | 579 (2.8x) | 85.7 | 4979 MiB |
| Discovery Tour, Alexandria street (DT start) | 1 / 1 | 191 | 79.8 | 4465 MiB |
| | 3 / 1 | 336 | 78.2 | 4465 MiB |
| | 5 / 2 | 563 (2.95x) | 81.4 | 4465 MiB |

No measurable frame-rate cost at this point: the game stays GPU-bound (Reflex still sleeps
0.4-2.5 ms per frame) and the busiest task threads go from ~63% to ~70% of a core. Discovery Tour
and story mode use the same spawning system and budget (same counts at the same cap); DT just has
no hostile/quest population.

### Stability

One hang in ~4 test runs: ~40 s after jumping from ~415 to ~566 spawned within seconds
(distance x2 applied live with the cap at x5), every task thread blocked and TaskThread0 spinning
on a lock in a Havok object destructor (RVA 0x2dbf530 -> 0x2e169d0 -> 0x2e78ef0, unregistering
from the registry at [RVA 0x52521f0]). The same configuration then ran 5+ minutes without
problems on a fresh launch. A lock-order deadlock in Havok object teardown that heavy spawn/despawn
churn makes more likely is the best guess; not proven to be caused by the higher count itself.
After that: story mode, `npc_density=5 npc_distance=2`, 12-minute soak with 8 teleports between the
Alexandria harbour (535-607 spawned) and Memphis (130-306), 84-89 fps, no hang (`tools/npc-re/soak.sh`).

### Visible density near the player (why the settings mostly add people far away)

- Every ambient NPC is a placed **MetaAI virtual unit** (base vtable RVA 0x377eab8, built by
  ctor 0x1d5d360 and its derived ctors 0x1d58fd0 / 0x1d596a0 / 0x1d5d520 / 0x1d5ddc0 / 0x1d17b10):
  ~9300 loaded around Alexandria, position at +0x10. Units belong to virtual spawners (vtable RVA
  0x377d218, ~3950 loaded; unit array at +0xa0, 0-8 units each, mostly 1-4; the spawner data
  `MetaAIObjectiveSpawnerCustomData` holds spawn positions at +0x7c {x, y, z, heading}).
- Around the Alexandria harbour market (story fixture position): 22 units within 20 m, 68-78 within
  40 m, ~240 within 80 m, ~395 within 120 m (the activation range), ~1200 within 250 m. The game
  spawns nearest first and its cap (240) is reached at roughly 70-80 m, so the ring the player can
  see up close is already fully populated in vanilla; raising the cap/distance fills the rim.
- Measured in view (skinning compute thread groups per frame, fixed camera, only drawn characters
  are skinned; 120 s per step): 1726 at 1/1, 1870 at npc_density=5 npc_distance=2, 1943 back at
  1/1 - no change beyond the +-10% noise, although the spawned total goes 208 -> 579.
- So "3x people within 40 m" needs ~3x the *units*: more units per spawner / spawn position at
  world load (patching the data before the virtual spawners are built, or hooking their unit
  creation), or cloning units at runtime through the engine's creation path. Not done: it means
  hooking/calling game code and finding the slot semantics (many units stand at CrowdLife station
  slots - stalls, seats - where extra units would stack; free wanderers
  `MetaAIBehaviourWanderOnAINetwork` are few).

#### Cloning units: findings so far (not implemented)

- Unit owners: class vtable RVA 0x377d218 (ctor ~0x1d5daa0, created from 8 call sites:
  0x1d18401, 0x1d583a9, 0x1d587b0, 0x1d59419, 0x1d59499, 0x1d5c7d9, 0x1d5cbc9, 0x1d5d289) holds its
  units in an Anvil array at +0xa0 (1-60 units; heavy tail: 252 of 600 sampled own none). These
  owners do not reach `MetaAIObjectiveSpawnerCustomData` within two links, so the spawn-position
  list of the custom data is not simply "one unit per position".
- The MetaAI manager activates objectives from its list at +0x7b0 (count +0x7b8; RVA 0x1dbc400 ->
  0x1d8e4e0 per entry). A clone would have to be allocated with the engine allocator (pattern
  0x8cc350/0x872440), built with the unit's derived ctor, given a unique id, appended to the
  owner's +0xa0 array and to the manager's +0x7b0 list (Anvil arrays, realloc 0x840180), and be
  destroyed with its owner on unload.
- **Save risk**: units carry id-like values at +0x8 (e.g. 0x1_000935d7), and the MetaAI save keeps
  per-spawner dead units as 16-byte keys (`MetaAISpawnerSaveData` {handle, dead-unit array at +0x8,
  tagged-unit array at +0x14}; `MetaAISpawnerDeadUnitSaveData` = two qwords + handle). Killing a
  clone in story mode would most likely write a record for an id the vanilla game never created;
  how a load treats that is unknown. This is the stop condition for runtime cloning until the
  dead-unit write path (and whether clones can be excluded from it) is understood. (Resolved
  below: there is no per-unit id in the save.)

#### What the MetaAI save stores about units (dead / tagged units)

Found statically (headless Ghidra) and checked against a loaded story save (t1, fixture
`story-alexandria-port`, hardware breakpoints on the serializers during the load).

- **Layouts.** `MetaAISaveData` (TypeInfo size 0x48, serializer RVA 0x1df9990) = Anvil arrays at
  +0x0 (16-byte elements), **+0xc `MetaAISpawnerSaveData`** (0x20), +0x18 (0x18), +0x24 (8-byte
  links: `MetaAITaggedStaticSaveData`). The live object that the game fills also has two
  runtime-only arrays: +0x30 (pointers to virtual spawners) and +0x3c (0xf0-byte per-spawner
  "recently killed" counters with timestamps, expired by RVA 0x1dcea40 after
  `[0x4c59408]+0x187c` seconds; used to throttle respawns, RVA 0x1df1200 / 0x1def2a0; not saved).
  `MetaAISpawnerSaveData` (serializer 0x1df9ea0) = {+0 link to the spawner's *data* objective,
  +0x8 dead-unit array, +0x14 tagged-unit array}. `MetaAISpawnerDeadUnitSaveData` (0x20,
  serializer 0x1df9e20) = {+0 Vec4 = the unit's position (x, y, z, 0), +0x10 link to the unit's
  data objective}. `MetaAISpawnerTaggedUnitSaveData` (0x10) = {link, u32 tag, u32 count}.
  Anvil property type codes seen: 0x0d = Vec4, 0x12 = object link, 0x16 = array.
- **The "unit id" is not saved.** Every virtual object (spawner or unit) has a link to its *data*
  objective at +0x38 (link record = {object, refcount, 64-bit object id}). For units that data
  object is a **`MetaAIObjectiveHuman`** (vtable RVA 0x3775820) which is an *archetype* shared by
  many units (Alexandria: 9322 units, only 225 distinct +0x38 links; one archetype is used by 421
  units across 100+ spawners). The u32 at unit+0x8 is not unique either (6907 distinct values for
  9322 units) and is not part of any save record. The spawner key is the spawner's data object
  (virtual spawner +0x60, the link created with hash 0xe112dcef).
- **Write path = rebuilt at every save, not on death.** The world save (RVA 0x1f00390) calls
  RVA **0x1e03b50** (MetaAISaveData, world): clears the saved arrays, then 0x1df8d60 (tagged units)
  and **0x1df8a70** (dead units). 0x1df8a70 walks the spawner list at +0x30 and each spawner's
  units (+0xa0) and appends `{unit+0x10 (16 bytes), unit+0x38}` to the spawner's record (found or
  created by 0x1ddd380, keyed by spawner+0x60; append = 0x1dcb2a0) when **all** of:
  1. unit byte +0x5a bit 1 is set: the unit is dead (set by the unit kill handler RVA 0x1da4a70 /
     0x1da4a10);
  2. unit byte +0x90 bit 6 is clear (set by RVA 0x1d65f60 when a system takes a unit over with
     spawn/scripting parameters that have flag bit 6, i.e. scripted/mission-controlled units are
     never recorded);
  3. squared distance from the unit position to the player entity ([[world+0x10]+0x98]) is below
     `[world+0x358]+0x3a0`^2 and, when the player's +0x1f8 link resolves to an object with
     (+0x5a & 3) != 0, above `[world+0x358]+0x3a4`^2;
  4. the spawner has a data object (+0x60 != 0).
  So only bodies lying near the player at save time are kept (the user's save had exactly one
  record). Tagged units (0x1df8d60) are keyed the same way (archetype link + tag value + count).
- **Load path.** RVA **0x1decae0** walks the loaded records; for each spawner record whose data
  object has a live virtual spawner it calls **0x1dab8a0**(spawner, world, &archetype link, &pos)
  per dead record. That looks the archetype up in the spawner's archetype list (packed array at
  spawner **+0xc2**, count at +0xca; e.g. 3 archetypes for a 4-unit spawner) and, if found,
  **instantiates a new unit** of that archetype at the saved position (archetype data vtable
  +0xa8 = create virtual object; then spawner vtable +0x240 (add unit), append to +0xa0 via
  0x136f630, unit +0x90 |= 0x8000). A record whose archetype is not in the spawner's list is
  skipped. Nothing matches existing units or ids.
- **Consequence for extra units.** An extra unit that copies an existing unit's spawner and
  archetype produces, when it is dead near the player at a save, a record of exactly the vanilla
  form (same spawner link, an archetype the spawner lists, a position). The loader handles any
  number of such records by creating one restored unit each, as it does for vanilla bodies. A unit
  with +0x90 bit 6 set is never recorded (but that bit also changes behaviour: scripted units).
- **Creating units at runtime, the engine's way.** 0x1dab8a0 is the game's own "add a unit of
  archetype A at position P to spawner S" routine; generic runtime creation of a virtual objective
  from a data objective is 0x1d61e40 / wrapper 0x1d62100 (returns the existing virtual if the data
  object already has one, so it cannot duplicate). Data objective classes create their virtual
  through vtable +0xa8 (e.g. 0x1d181a0 for `MetaAIObjectiveSpawner` -> virtual spawner ctor
  0x1d5da90, 0x1d18460 for `MetaAIObjectiveSpawnerDynamic` -> 0x1d5db60). The MetaAI manager
  update (0x1daf5f0) is virtual slot 47 (+0x178) of the manager vtable 0x377e7e8 (89 slots).

#### `[world] npc_units` experiment (src/world/npc_units.cpp, default off)

- Runs on the MetaAI update thread: the manager's vtable pointer (World+0x1688, vtable 0x377e7e8)
  is swapped to a heap copy whose slot 47 calls the game's update (0x1daf5f0), then ours (a data
  write; no code bytes are modified). It walks the manager's list of all virtual objectives
  (+0x3b8, plain u32 count at +0x3c0: 49546 objectives, 3952 `MetaAIVirtualObjectiveSpawner`s
  around Alexandria) in slices; every unit (vtable 0x377eab8) of such a spawner within 260 m of the
  player that the MetaAI has not activated yet (+0x1b8 == 0) gets k-1 units of its archetype 1-2 m
  away, built with the 0x1dab8a0 sequence minus its two kill calls. The engine gives each a fresh
  id (+0x8). Units already activated near the player are left alone until they deactivate (~120 m),
  so a fresh load only gets extras beyond that ring until the player moves.
- **A/B** (t1, 768x480, FG off, 30 fps cap, story fixture, both at `npc_density=3`, interleaved
  1/2/1/2 on fresh launches; each run: load at the Alexandria spawn anchor, hop 180 m west for 30 s,
  hop back, 30 s settle, 120 s of samples; `tools/npc-re/ab-units.sh`):

  | k | in-view skinning groups/frame (sd) | units <=40 / 80 / 120 m (all active) | spawned (cap 600) |
  |---|---|---|---|
  | 1 | 2704 (387) | 58 / 181 / 342 | 339 |
  | 2 | 3421 (540) | 106 / 296 / 517 | 349 |
  | 1 | 2474 (350) | 63 / 186 / 348 | 336 |
  | 2 | 3461 (508) | 102 / 293 / 529 | 346 |

  k=2: +33% characters drawn in view (3441 vs 2589), 1.7x units within 40 m, about the same total
  spawned: the spawner fills the near ring first, so the extras replace far NPCs. With
  `npc_density=1` the cap binds and k=2 shows almost nothing (2330 vs 2177).
- **Cost**: our part of the MetaAI update is 0.064-0.085 ms on average (max 0.4-1.0 ms while a
  batch of up to 48 units is created) against 0.18-0.31 ms for the game's update, ~300 updates per
  10 s: about 2 ms per second of one task thread. ~1100-1460 extras exist after a few minutes in
  Alexandria (cap 4000 per world).
- **Stability**: 23-minute walker soak at k=2 / npc_density=3 (no teleports, 290-357 spawned, 0
  failed creations): no hang. Plus ~45 minutes over the A/B and save runs, with short teleports:
  no hang or crash.
- **Save test** (`npc_units_kill=1` kills the nearest extra with the engine's kill; dead flag set):
  neither the autosave during play nor the save made by Quit to title screen (both write 3/4.save)
  runs the MetaAI save builder: breakpoints on 0x1e03b50 / 0x1df8a70 did not fire while
  `MetaAISaveData` was serialized (0x1df9990, from the AssassinSaveGameData serializer 0x1ef59b9).
  Those saves write the `MetaAISaveData` that was loaded, unchanged. After reloading the
  post-kill save, the loaded `MetaAISaveData` held only the fixture's old record (1257, -617), the
  apply 0x1decae0 ran once and 0x1dab8a0 never ran (that record's spawner is not loaded), and no
  restored body appeared. The 18 vanilla dead units near the player were not recorded either. The
  save loaded cleanly twice. So in these save paths a dead extra leaves nothing, the same as a dead
  vanilla unit. The builder is gated by a local flag in the world save 0x1f00390 (call at
  0x1f00d93, caller 0x1ef3930); what sets it was not found (world change / fast travel are
  candidates). If it runs, records are in the vanilla format (see above).
- The game autosaves its story slot right after a story load and now and then while playing. Quit
  to title screen also saves. Saves rebuild nothing else in MetaAI as far as seen.
- Not tested: fast travel / world change with extras (spawner destruction with added units),
  combat around extras, visual stacking at CrowdLife stations (extras share their original's slot
  parameter).

### The Havok deadlock is a vanilla engine bug

Same stack (TaskThread0 spinning in 0x2ffd9e4 <- 0x2e78f07 <- 0x2e16a8d <- 0x2dbf560 (Havok object
destructor unregistering from [RVA 0x52521f0]) <- 0xf8c5ce <- 0xf0f4e9 <- 0xf1f482 <- 0x1327601 <-
0x131aa31 <- 0x131ade4; other task threads waiting in 0x1316a4b -> 0x2ffd9e4 under 0x2e16a8d), seen:
1. DT, cap x5 + distance x2 applied live (spawn burst 415 -> 566);
2. story, `npc_density=3`, right after `tools/teleport` Alexandria -> Memphis;
3. story, **vanilla** (npc settings at 1, the npc code never engaged), right after the same teleport;
4. story, vanilla, while gdb hardware breakpoints were pausing threads repeatedly.
Abrupt mass spawn/despawn (instant teleports, debugger stalls) triggers it; it is not caused by the
density settings. Long soaks must move the player normally (`tools/npc-re/walker.sh`), not teleport.
Shipping candidate `npc_density=3 npc_distance=1`: 26-minute story-mode soak in Alexandria with
the walker (229-353 spawned, cap 600), no hang.

### Dead ends

- `CrowdDensity` (settings index 7) has one level on PC and nothing reads it except the settings
  copy code (refs at RVAs 0xa47127, 0xa47134, 0xa5e677; the getter 0xa58ec0 is never called with 7).
- `MetaAIBudgetSettings` (vtable 0x377e518: 250 / 30.0 / 100 / 1000 + 10 `MetaAIBudgetPriority`
  entries), `BulkWorldComponent` (+0xd0..: 8192 / 160 / 70 / 40), `BulkGlobalSettings` and
  `BlobSettings` (vtable 0x37a35f0) are never read while playing (hardware read watchpoints for
  20-120 s, also across a fast travel): consumed at world load, or unused. `BlobSettings` values are
  copied into the blob source's helper (spawn radius^2 5625 = 75 m at +0xf0); the blob source had no
  candidates in Alexandria / Siwa. Changing them live had no effect on the count.
- `FakeCrowd*` (the distant fake crowd: `FakeCrowdHerder` holds `FakeCrowdDensityModifiers` at
  +0x60, four 1.0 multipliers) has no live instances in Discovery Tour (Siwa, Alexandria).

### Tools

- `tools/npc-re/live.py` (host-side reads of the game's memory, `ACO_INST=t1|t3`), `hist.py`
  (heap objects per class, from `vtnames.json` built by `vtnames.py` from the TypeInfo records),
  `fields.py` (per-offset value distribution across a class's instances), `spawncount.py`.
- `gw.sh` / `gb.sh`: hardware data watchpoints / execution breakpoints via gdb from the host
  (no code bytes modified; the script always deletes the debug registers and detaches; killing gdb
  with the watchpoint armed leaves DR7 set and crashes the game on the next hit). In-process
  guard-page watching (PAGE_GUARD + single step) hangs the game under Wine and was removed.
- `decomp.sh <rva> | @<rva inside a function>`: headless Ghidra decompile of the raw image dump.
