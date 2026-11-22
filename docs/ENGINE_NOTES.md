# Assassin's Creed Origins (AnvilNext 2.0, v1.62) — rendering notes

The rendering notes use frame captures from this mod's tracer (`touch <out>/capture`)
and reflection data embedded in the game's DXBC shaders. Resource names below are the
HLSL names from that reflection data.

## Executable

- Imports only `D3D11CreateDevice` and `CreateDXGIFactory1`; a `d3d11.dll` next to the exe is
  loaded instead of the system one (Proton: `WINEDLLOVERRIDES=d3d11=n,b`).
- Creates 3 D3D11 devices (two probes, then the real one) and 9 deferred contexts. Almost all
  rendering is recorded on deferred contexts and executed on the immediate context.

## Startup artwork

Pharos v26 restores the Bayek artwork window at startup. **Skip intro videos** in
Quiver still controls the opening movies independently. In mono, Origins draws its
original image. In stereo, Pharos gives each eye a distinct view of the scene,
Bayek and the title in the game's original window. The splash uses half side by
side even when the game uses full side by side. The **Swap eyes** setting applies
to both the artwork and the game.

With **Stereo 3D** set to **On stereo displays**, Pharos uses the stereo artwork
on a HypXRland stereo output when the compositor confirms that it will split the
window. A headless stream or mono desktop shows the game's original artwork.

Four fresh v26 game processes covered headless automatic mono, two half-side-by-side
starts and full-side-by-side packing with the eyes swapped. Captured splash pixels
matched the expected stock or packed artwork. The title prompt accepted input in
all three launches tested for input. Separately, HypXRland displayed both eye orders
at the native window size and after a compositor resize, with opaque artwork in
both eyes.

For comparison, with intro videos hidden, v24 (artwork visible) and v25 (artwork
skipped) both reached “Press any key” in a median 14.62 s. Each release ran three
times in the Quiver lab at 960×540 SDR and 30 FPS on an idle RTX 5070 Laptop GPU.
The artwork was visible for roughly five seconds in v24.

## Launch-screen content check

On Origins 1.62, the content-loading worker waits three engine frames at each of three
points in a batch of local add-on records. At 10 FPS each delay costs about 300 ms,
even when streaming has already finished. Pharos shortens these waits for the title
screen's request once the engine has acquired the assets, finished preparation or
acknowledged publication on the game thread, respectively. It verifies the original
instructions before enabling the change. Unsupported preparations and batches whose
completion cannot be established use the game's original deadline.

On v23, the worker also recognizes completed `TagDLCAddon` preparation. It checks
that all three retained resources and the child handles consumed by the game thread
are ready. The game thread still runs the publication callback.

| Measured rate | v21 press A to menu | v22 | v23 | Worker frames, v21 → v23 |
| --- | ---: | ---: | ---: | ---: |
| 10 FPS | 64.1–64.5 s | 34.2–34.8 s | 30.9–31.4 s | 522 → 199 |
| 20 FPS | 32.3–32.9 s | 18.0–18.4 s | 16.9–17.4 s | 519 → 207 |

Each range spans the last screenshot before the menu and the first screenshot with
it visible. All runs used the same 28 installed packages and produced matching
results for all 255 add-on records and three registered entries. The retained
resources and child handles of all 16 tag add-ons matched between v22 and v23.
Actual asset publication and queued callbacks still run on game updates, so the
remaining check takes longer at lower frame rates.

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

## Player entity, camera and teleport (`src/world/teleport.cpp`, `quiver lab teleport`, `tools/lab/teleport`)

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
  `quiver lab fixture save|install` archive and install them. The old harness's six story saves
  were imported (`quiver lab fixture import`, measured: 6 files each) as lab fixtures
  `story-{alexandria-port,cyrene,faiyum-dionysias,krokodilopolis,memphis-temple,siwa}`
  (`quiver lab fixture list origins`; install with the game stopped); originals still in
  `~/Games/aco-dlss-instances/fixtures`. `tools/lab/dt --story [location]` continues the first
  profile (and teleports).
- **Arrival**: the player hangs at the target while collision streams in (about 1 s across the
  map), then falls the 1 m. Textures/LODs are complete within ~5 s at 768x480. NPCs keep spawning
  for longer (compare with the same place after the game's own fast travel).
- **Render camera**: global pointers at RVA **0x49e86b8** and 0x49f39d8 (object vtable RVA
  0x36afd88): position at +0x130, unit forward vector at +0x140, then the view rotation; this is what
  `PassParams.viewToWorld` is built from. The camera controller's yaw/pitch were not found as plain
  floats (no stored heading vector either), so the camera is turned with injected relative mouse
  motion (XTEST from the host; SendInput from inside the game doesn't reach its raw-input
  mouse-look under Wine, tested live in the lab) in a closed loop on this readout.
  `quiver lab teleport` does it for a location's heading and `quiver lab turn` standalone
  (measured live: converges to <0.5 deg in 1-2 moves; learned gains ~0.116 deg/px yaw,
  ~0.077 deg/px pitch, saved per instance in `camera-gain.json`). Pharos's old `tools/lab/camera`
  measured the same thing and was removed.
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
  combat around extras.
- **Stacking**: none seen. With `npc_units_log=1` (logs every extra with its original) and
  `tools/npc-re/stack.py`: of 155 activated extra/original pairs within 120 m of the harbour market,
  140 were more than 10 m apart and 2 within 2 m. Once activated, an extra gets its own place.
- **What limits the total** (live `capsweep.sh` after the same hop as the A/B, 120 s per step,
  each k in its own launch; units = units of civilian spawners per band, from `bands.py`):

  | npc_density:distance | k | spawned (log) | in view | units 0-40 / 40-80 / 80-120 / 120-200 / 200-400 m |
  |---|---|---|---|---|
  | 3:1 | 1 | 329 | 2571 | 61 / 112 / 166 / 566 / 1553 |
  | 3:1 | 2 | 345 | 3052 | 86 / 205 / 258 / 831 / 1929 |
  | 8:1 | 1 | 367 | 3068 | 87 / 110 / 162 / 594 / 1517 |
  | 8:1 | 2 | 567 | 4275 | 109 / 214 / 233 / 821 / 1947 |
  | 8:2 | 1 | 658 | 3974 | 78 / 132 / 161 / 559 / 1527 |
  | 8:2 | 2 | 930 | 5386 | 104 / 202 / 240 / 796 / 1972 |

  (k=2 with extras for activated units beyond 60 m as well, commit 7909570; an earlier run without
  that gave 346 / 507 / 524 / 916 spawned at 3:1 / 5:1 / 8:1 / 8:2.)
  - With k=1 the total stops at ~365 whatever the cap: demand-limited (candidates within the
    templates' spawn distance), as found earlier.
  - With k=2 at `npc_density=3` the total does not move (345 vs 329) and k=2 only shifts people
    toward the player: the cap binds through the budget pressure (spawn distance x0.9 from 0.8 x cap,
    x0.8 from 0.9 x cap; the effective cap is maxSpawned x the Environment multiplier, which looks
    like 0.75 here: the plateau sits at 0.8 x 600 x 0.75 = 360).
  - From `npc_density=5` up, k=2 raises the total 1.4-1.55x (567 vs 367, 930 vs 658) and every
    distance band has more units (1.25-1.95x). Spawned people go up in proportion to the units that
    exist, so there is no other hidden limiter. The total stays below 2x because of coverage:
    units within 60 m of the player that are already activated only get extras after the player
    leaves, and only units within 260 m of where the player has been are handled.
  - Per-band *spawned* counts are not measured yet: no reliable per-unit "spawned" field was found
    (activation record +0x30 is always 0 in story mode; unit +0x30 is a behaviour object present
    for far units too). In-view skinning load and the total spawned are the measures used.
- **Bug fixed in `npc.cpp`** (also in the shipped build): the SpawningManager switches between
  SpawnSettings objects (maxSpawned 200 and 300 in Alexandria), and `npc_on_present` re-read "the
  original" whenever the object changed, so returning to an object it had already patched
  multiplied again (seen: 300 -> 900 -> 9000 at npc_density=10). The distance multiplier had the
  same pattern. Originals are now kept per object (commit dce5893).
- **Per-band spawned measure** (`tools/npc-re/spawnbands.py`): the MetaAI manager keeps the spawn
  records of its spawned (or spawning) objectives at +0x48 ({ptr, n << 17}, vtable 0x363be88; a
  unit's record is its +0x1b8; +0x34 == 1 marks the pending ones). The array's size follows the
  game's spawned count (355 vs 341; after a live cap cut 223 vs 207, and the 80-120 m band emptied
  first). Not spawn markers: activation record +0x30 (always 0 in story), unit +0x30 / +0x108
  (behaviour objects, present for far units too), manager +0x498 (far objectives).
- **Scaling** (k = npc_units, `npc_density=40` so the cap never binds, npc_distance 1, 768x480
  uncapped, FG off, fresh launch at the Alexandria spawn anchor, 120 s settle, `ab-bands.sh`;
  spawned civilians per band, two or three samples averaged; full-coverage build 4650ca2/c7061c6):

  | k | 0-40 m | 40-80 m | 80-120 m | 120-200 m | total | fps | task threads (% of a core) | GPU util |
  |---|---|---|---|---|---|---|---|---|
  | 1 | 59 | 114 | 173 | 15 | 361 | 91.6 | 472 | 31% |
  | 2 | 93 | 187 | 243 | 12 | 535 | 76.6 | 689 | 39% |
  | 4 | 150 | 277 | 322 | 15 | 763 | - | - | - |
  | 4 (fixed) | 185 | 347 | 387 | 21 | 940 | 61.1 | 726 | 34% |
  | 6 (fixed) | 368 | 400 | 321 | 18 | 1107 | (game paused) | | |
  | 8 (fixed) | 465 | 410 | 497 | 21 | 1393 | (game paused) | | |

  k=6 and k=8: the samples were taken while the game was paused (Ubisoft service restarts during
  the runs: task threads 0%, GPU 0%, frozen counts), so only their counts at that moment are
  usable and fps is missing. In view at k=4: 6805 skinning groups/frame (2.7x k=1).
- **Hard limits** (`tools/npc-re/freezetest.sh`, 240 s per step after entering story mode):
  - k>=6 froze the game every time, 25 s to 3 min after the load: all threads waiting (task
    threads 0%, GPU 0%), last frame on screen, no exception logged, no crash dump. Not the Havok
    spin. Before a per-spawner cap it froze after ~1430 originals had their extras whatever k
    was (k=6: 7075 / 7295 extras, k=8: ~10000), while k=4 (spawners up to ~240 units) never
    froze; the frozen game had spawners with 256-422 units. So a spawner must stay below 256
    units (u8 somewhere): extras now stop at 200 per spawner. A cap of 250 still froze once in a
    walker soak at k=4 (spawner at exactly 250, 4606 extras), 200 ran a 12-minute soak.
  - With that cap: k=6 ran 240 s (6535 extras, 54 fps uncapped), k=8 froze at 7635 extras
    (155 s), k=10 at 7453 (30 s): a second, global limit at ~7500 extras, i.e. ~16.5-17k civilian
    units in the loaded world (16658 counted in the frozen k=10 game). Which pool it is was not
    found (a 16384-entry table is the obvious suspect). `[world] npc_units_max` caps the extras.
  - With the per-spawner cap, k=6 gave no more spawned people than k=4 (883 vs 940): the big
    spawners that hold most city units are already capped.
- **Far-field crowd systems in story mode**: the `BulkWorldComponent` is live (World+0xe0 ->
  +0x1e8; settings at +0xd0: 8192 / 160 / 70 / 40 / 0.69 / 40.0 / 1e4; pools of 8192 and 24576;
  counters at +0x190 / +0x208: 556 / 578 in Alexandria at k=1, 1137 / 1162 at k=6). Its count
  tracks the spawned characters (+ animals etc.), so it looks like the representation of spawned
  MetaAI characters (jobs `SetupMetaAISpawnedUnitBulk` / `UpdateMetaAISpawnedUnitBulk`), not a
  separate cheap far crowd. No `FakeCrowdHerder` is reachable from the World object.
- **Soak**: k=4, npc_density=40, per-spawner cap 200: 12-minute walker soak (30 fps cap), no hang,
  745-870 spawned (vs ~360 at k=1), 0 failed creations, our hook 0.03-0.1 ms per update (one 11 ms
  spike while a large batch was created).
- **The real ceiling: ~1000 spawned characters at once.** A 30-minute roaming soak at k=4 (user
  settings, `tools/npc-re/soak-roam.sh`) froze after 35 s at 1056 spawned; every freeze so far
  happened with the game's spawned count at ~1000-1400 (k=4 soaks 1023-1073 / 1056, k=6-10
  1100-1400), none at <= ~940. Not an exception (a vectored exception logger saw none), not the
  creation rate (slow creation at 36 units/100 ms froze the same way, 265 s in, with creation
  almost finished), not the per-spawner count alone. With maxSpawned clamped to 800 raw
  (`kMaxSafeSpawned`, npc.cpp) k=6 and k=10 ran 300 s clean (they froze within 25-265 s without
  it), but the spawn budget pressure then empties the 80-120 m band (k=10: 197 / 307 / 12 vs k=1
  59 / 114 / 173): redistribution again. The limit is the process's TLS indices: see
  "Spawned-character limit" below (`[world] tls_extend` lifts it).
- **GPU cost at the user's settings** (t1 2560x1600, DLSS Performance at render_scale 0.6,
  lod_distance 2, cloth render, npc_density 10, fixed camera at the Alexandria spawn anchor,
  `tools/npc-re/ab-user.sh`):

  | FG | k | real fps | presented/s | GPU util | VRAM | spawned |
  |---|---|---|---|---|---|---|
  | on | 1 | 71.9 | 143.7 | 96% | 4.9 GB | 355 |
  | on | 4 | 69.7 | 139.4 | 96% | 5.4 GB | 776 |
  | off | 1 | 89.4 | - | 86% | 4.7 GB | 358 |
  | off | 4 | 76.7 | - | 73% (CPU-bound) | 4.8 GB | 815 |

  With FG the frame is GPU-bound and k=4 costs ~3%; without FG k=4 makes the game CPU-bound
  (-14%).
  (First run interrupted at k=4.) Every band rises, and spawned ~= units within 120 m (the activation
  range is the spawn limit once the cap has headroom). Coverage at k=4 was only ~2-2.8x per band:
  spawners that had grown past 128 units were skipped (fixed in f4209f1, snapshot up to 4096). At
  768x480 the game is CPU-bound (GPU 31-39%): k=2 costs 16% of the frame rate, almost all in the
  task threads (+46% CPU). Our hook is 0.04 ms per MetaAI update.

### Spawned-character limit: TLS index exhaustion (the ~1000-character freeze)

Found on frozen games (t3, `npc_units=6 npc_density=40` with the 800 clamp lifted, `roamer.sh`;
froze at 994 and ~1150-1220 spawned) with `pestack.py`, `jobq.py`, `jobwhy.py` (below).

- **What hangs.** The engine's frame is a job graph (scheduler RVA 0x4873e38; per-frame root jobs
  `Engine::BeginFrame` at RVA 0x4873c18 and the engine graph at `[RVA 0x4c298b8] + 0x238`, built by
  RVA 0x1617880). In the frozen game every queue is empty, seven task threads sleep in the worker
  idle wait (RVA 0x8cd1a0 -> 0x675a40), and `EndEngineFrame` / `EndFrame` wait on prerequisites whose
  only unfinished leaf is the parallel job **`KinoAfterSkeleton`** (state 2, one thread still inside).
  That thread (TaskThread3) is the only one not idle: inside the job it is blocked in
  `AcquireSRWLockExclusive` (engine write lock RVA 0x613a20, called from the Kino code at 0x71496a,
  lock = `[obj + 0x30]`). The SRW lock word is `0x00020002`: **2 shared owners, 2 exclusive
  waiters**, yet no thread references the lock on its stack. EngineWindowThread at 100% CPU is
  normal (its message loop spins in every state).
- **Why it is held.** Engine reader/writer lock (ctor RVA 0x60ebc0, dtor 0x60ef10): `{+0 SRWLOCK,
  +8 int, +0xc owner thread id, +0x10 TLS index}`. The ctor stores `TlsAlloc()` at +0x10 for the
  per-thread read-recursion count. Read lock (RVA 0x6139d0): `TlsGetValue(idx) == 0` ->
  `AcquireSRWLockShared`, `TlsSetValue(idx, n + 1)`. Read unlock (RVA 0x617c20): `n = TlsGetValue -
  1`; `ReleaseSRWLockShared` only when `n == 0`. When TlsAlloc fails the index is
  `TLS_OUT_OF_INDEXES` (-1): TlsGetValue returns 0 and TlsSetValue fails, so every read lock takes the
  SRW lock shared again and the unlock (0 - 1 = -1 != 0) never releases it. The first writer then
  waits forever, the job never ends and neither does the frame. The frozen lock's +0x10 is
  0xFFFFFFFF.
- **Who takes the indices.** One lock per **Kino animation instance** (Ubisoft's animation runtime,
  `KinoTrack`/`KinoExternalIdMap` code at RVA ~0x6b0000-0x760000): instance creation (virtual method
  RVA 0x71a6d0, slot 122 of the vtable at 0x3582b40) -> 0x71a520 (0x280-byte object from a free
  list) -> ctor 0x708c10 -> 0x7483e0 (0x70-byte object, lock at +0) -> 0x60ebc0. So one TLS index per
  animated character (people, animals, mounts). The other 14 lock owners are global or rare
  (`PhysicStats`, `SoftBodyStats`, `FXStarterComponent`, ...).
- **The limit is the OS's.** A process has 64 TLS slots plus 1024 expansion slots (Windows and
  Wine). In the frozen game: PEB `TlsBitmapBits` (PEB+0x80) 64/64 used, `TlsExpansionBitmapBits`
  (PEB+0x240) 943/1024 (after spawned had dropped from its 1220 peak to 1154). The engine, DXVK,
  NVIDIA and Wine use the first few dozen, so the first freeze lands at ~1000-1100 characters. It
  does not depend on creation rate or spawner size, and nothing throws.
- **No data knob.** The capacity is the process TLS table, not game data. What the game uses
  instead: TlsAlloc/TlsGetValue/TlsSetValue/TlsFree through its private import table
  (VMProtect-filled; the PE import directory's own IAT copies at 0xe164xxx / 0xdb92xxx are unused):
  RVA **0x353a688 / 0x353a690 / 0x353a698 / 0x353a6a0**, holding kernel32's exports (~2100
  TlsGetValue call sites).
- **Fix: `[world] tls_extend=1`** (`src/core/tlsext.cpp`, default on). At the first Present it
  checks that the game's four entries hold kernel32's exports. Swapping those entries does not
  stick: the first try found them reverted to kernel32 after the world load (VMProtect refills
  that table) and froze the same way. Wine's kernel32 exports are stubs (`lea rsp,[rsp]; jmp
  [rip+x]`) that jump through kernel32's own import table to kernelbase. Those four kernel32 import
  entries are swapped instead (`iat_hook`; a data write; no game or Wine code bytes change):
  - Native indices (< 0x10000) take one compare and go straight on to kernelbase.
  - TlsAlloc called from the game image (return address check) gets a virtual index (0x10000 + n,
    up to 65536) once 64 or fewer native indices are free. Every other module only ever gets native
    ones.
  - Virtual values live per thread in a chunked table hung off one native slot. A generation
    counter per index makes a freed and reused index read 0 in every thread, as on Windows.
  - The 800 maxSpawned clamp in npc.cpp (`kMaxSafeSpawned`) applies only when tls_extend is off.
  - The log says "tls: native TLS indices exhausted (...); handing out virtual ones" when it
    engages, then reports native free / virtual in use every 10 s.
- **Validation** (t3 768x480, FG off, story fixture, `npc_units=6 npc_density=40`, `roamer.sh`,
  same setup that froze within 1.5-4.5 min three times, at 994 / ~1220 / 1250 spawned):
  **no freeze in 9.5 minutes**.
  - Spawned 870-1316, peak 1325; 7165 extras.
  - Native TLS free 64-253 (the reserve held). 96-391 virtual indices in use: they follow the
    characters as they spawn and despawn.
  - No exceptions; 46-95 fps with t1 sharing the CPU.
  - Not tested yet: long soaks, fast travel / world change, and where the next limit is (the
    16.5k-units-per-world and 256-units-per-spawner freezes were seen with the old build; they may
    or may not be the same cause).
- Other candidates looked at and ruled out on the way: the task scheduler's queues grow
  dynamically (Anvil arrays; 0x8c8b50); the "ATOM - Loading" thread has a fixed 1024-entry request
  pool (RVA 0xde6050) but sat idle in its normal wait in both frozen games; the
  `BulkWorldComponent` counters were fine at 1137 (k=6) in a run that did not freeze. The older
  "Havok teardown" stacks of these freezes came from scanning stale stack memory (RVA 0x1316a4b is
  an array resize, 0x2dafe30 returns a global). Use `pestack.py`, which starts at the live rsp
  recovered from Wine's syscall frame.

#### npc_units with `tls_extend` (the TLS-exhaustion fix)

- The per-spawner "~256 units" and per-world "~7,500 extras" limits were the same TLS exhaustion:
  with `tls_extend=1`, k=6/8/10 ran 300 s clean with spawners far above 256 units and 12.5k
  extras. The 200-per-spawner cap is gone (16000 sanity limit); snapshots hold 16384 units.
- Scaling (768x480 uncapped, FG off, npc_density 40, Alexandria spawn anchor, spawned civilians
  per band; `ab-bands.sh`):

  | k | 0-40 m | 40-80 m | 80-120 m | total | fps | task threads | native TLS free (min) |
  |---|---|---|---|---|---|---|---|
  | 1 | 58 | 125 | 175 | 369 | 99.2 | 648% | 622 |
  | 4 | 165 | 315 | 364 | 861 | 75.4 | 713% | 81 |
  | 6 | 199 | 402 | 479 | 1098 | 69.7 | 736% | 64 |
  | 8 | 275 | 489 | 543 | 1330 | 63.6 | 736% | 64 |
  | 10 | 303 | 573 | 644 | ~1540 | 56-60 | 732% | 64 |

  Every band rises at every k; spawned ~= units within 120 m. Short of k x because of coverage
  (activated units in view are deferred; only units within 400 m get extras).
- **User settings** (2560x1600, DLSS render_scale 0.6, FG v10 defaults incl. auto_cap, lod 2, cloth
  render): k=1 / 6 / 8 all hold 55 real / 110 presented fps (auto_cap); GPU 64% / 74% / 72%,
  VRAM ~5.6 GB; 0-40 m spawned 62 / 209 / 250 (3.4x / 4.0x), in view 2539 / 6935 / 7286 groups.
- **Soak**: k=8, npc_density 40, 30 min with `roamer.sh` (1,480 m travelled, no teleports): no
  hang, 814-1,957 spawned, 15.4k extras, 0 failed, 58-60 fps at a 60 cap, native TLS min 63.
- **Fast travel** (the game's own, map -> Atlas -> hold right mouse on a region icon;
  `tools/npc-re/fasttravel.sh` / `fttour.sh`), k=8: Alexandria -> Memphis -> Siwa -> Faiyum ->
  Alexandria -> Memphis -> Siwa -> Alexandria with 2 minutes of roaming in each: no hang, 59.8 fps
  at the 60 cap, 395-959 spawned per city, native TLS min 63, up to 146 virtual indices. It
  showed the handled / extra sets growing without bound (52k stale entries); they are now pruned
  to live units once a minute.
- **Combat, kills, quit + reload** (`tools/npc-re/combat-save.sh`), k=8: fighting in the crowd (up to
  1,776 spawned, 850 virtual TLS indices), three extras killed with the engine's kill, then quit to
  title (3.save written) and continue, twice: both reloads clean (1,445 / 1,275 spawned), nothing
  restored from the save (these saves don't rebuild the MetaAI dead-unit records, as found
  earlier), no hang.

#### npc_units v12: the post-fast-travel stall, unit classes, urban / rural, variety

- **Stall after a world change (v11 bug).** After a Sinai -> Alexandria fast travel no extras were
  made: `step()` returned before doing anything. Repro (t1, Alexandria -> Sinai -> Alexandria
  with the game's own fast travel, `tools/npc-re/ft-sinai.sh`), logging what v11's checks would
  have done: the player root was fine in every step, but v11's `readable(list, n * 8)` on the
  MetaAI manager's objective list (49546 entries, 396 KB) failed in every step (96 of 96 per 10 s)
  once the world had changed: the reallocated list spans two allocator reservations, and
  `VirtualQuery` reports only the first. `readable_span` (walks the regions) fixes it; extras came
  back right after the travel (9.3k live extras within two minutes). Within Egypt (the earlier
  fast-travel tour) the list never moved, so that tour was not affected: its 395-959 spawned per
  city came from where the fast-travel points are (city edges).
- **Unit classes.** Objectives holding units (`+0xa0` arrays) and their unit classes, from the
  manager's list in Alexandria (`tools/npc-re/objfactions.py`; class names from TypeInfo via
  `vtname.py`): `MetaAIVirtualObjectiveSpawner` (0x377d218) holds `MetaAIVirtualObjectiveHuman`
  (0x377eab8, ~15k), `...RideableAnimal` (0x3781720, horses/camels), `...SmallWaterVehicle`
  (0x3786208, boats) and `...Vehicle` (0x377ca18); `...AnimalSpawner` (0x3781f20) holds
  `...Animal` (0x377f560); `...Ambush` (0x3781cf0), `...StaticCage`, `...AnimalLocation`,
  `...BountyHunterSpawner` (0x3784400) hold their own. npc_units only ever touched Human units of
  plain spawners.
- **Factions.** A Human unit's archetype (`MetaAIObjectiveHuman`, +0x38 link) links its faction
  at +0x10 (`MetaAIFactionData`, vtable 0x3789a88). Around Alexandria: faction id 0x2dfb71a566 =
  ~11k units in 55 archetypes (civilians); 0x45bafa6a55 = ~2.7k (also the only faction in the
  bounty-hunter spawner: Ptolemaic soldiers / guards); 0x3b79cbd9a8 and 0x157058a1b3 (both in
  `Ambush` objectives: bandits); animal factions (0x3fda49da1f, 0xc637..., 0xbf37...) in animal
  spawners; 0x13e08432389 for rideable animals. **Byte +0x30 of the faction is 0 for the
  civilian and rideable factions and 1 for every combat faction** (soldiers, bandits, wild
  animals, 0x157058a1b7, 0x7ae9ae2e08): v12 uses it as civilian vs hostile. The faction's
  float4 at +0x20 differs per human faction (a UI colour?).
- **Urban vs rural.** The measure is the number of original civilian-spawner Human units within
  ~150 m (7 x 7 cells of 50 m, rebuilt once a minute). At the player (`tools/npc-re/urbanity.py`,
  k=1): Alexandria spawn anchor 802 (units within 400 m: median 877), Memphis 820 (722),
  Krokodilopolis 494 (414), Siwa's centre 352 (439), Faiyum farmland 46. Threshold 300: cities
  and towns urban, farms and Sinai's countryside rural.
- **Variety and spread.** Each extra now copies a random original of the same class from the
  same spawner: its archetype entry (so the spawner's own archetype mix, weighted by use, is the
  palette) and its place plus 1-4 m. Extras of one unit no longer start in a clump on one spot.
  Save safety is unchanged: an extra is still (spawner, an archetype that spawner lists,
  position), which is what the dead-unit records hold and what the loader matches.
- **Hang during a jump; pause after one.** The first k=8 run of `tools/npc-re/v12-places.sh` hung
  (main thread at 99%, log silent) during the debug teleport Memphis -> Faiyum farm (4.3 km), in the
  middle of a burst of 200 extras created within 2 s of the jump, while the old area was streaming
  out. Since then `step()` makes no units for 8 s after the player moves > 150 m between steps
  (fast travel, respawn, teleport; logged as "player jump(s)"). The rerun passed every stage.
  (Not proven to be our fault: instant teleports also trigger the vanilla Havok deadlock below; no
  stack was taken.)
- **Results (t1, 768x480, k=1 vs k=8, rural 2, hostile 1, `npc_density=40`, `tls_extend=1`;
  spawned civilians per band 0-40 / 40-80 / 80-120 / 120-200 m, total):**

  | place | k=1 | k=8 |
  |---|---|---|
  | Alexandria (spawn anchor) | 73/118/170/14, 375 | 295/533/458/18, 1304 |
  | Memphis (fast travel) | 30/76/143/15, 264 | 81/365/561/31, 1038 |
  | Faiyum farm (teleport) | 0/0/4/2, 6 | 0/0/6/0, 6 |
  | Sinai (fast travel; rural) | 8/26/45/0, 79 | 9/36/54/0, 99 |
  | Alexandria after travel | 28/88/171/18, 305 | 99/393/611/20, 1123 |
  | quit to title + reload | 24/80/148/12, 264 | 92/236/542/46, 916 |

  Every band up in the cities (x3-4 total), Sinai +25% at the rural value, the farm is empty
  either way. 55-60 fps (cap 60) everywhere; native TLS slots went down to 64 free, virtual TLS
  peaked at 421 indices (`tls_extend` is required at k=8). No hostile extras at the default 1.
- **Spread and variety (k=8, Alexandria, `tools/npc-re/clump.py 80`):** 536 activated extras
  within 80 m, 24 archetypes among them; nearest sibling (another extra of the same original)
  median 33.9 m, 9% within 3 m (v11: every sibling started on the same spot). Nearest
  same-archetype extra: median 2.8 m, 51% within 3 m: crowds are dense and a spawner's mix is a
  few dozen archetypes. Side-by-side look (`samearch.py`, a same-archetype pair 1.3 m apart,
  screenshot): two women of the same build in different dress colours (green / teal), so one
  archetype is not one fixed look; at 768x480 that is a hint, not proof.
- **Soak (k=8, `tools/npc-re/soak-roam.sh t1 build/mod-uncap 12`):** 12 minutes roaming
  Alexandria (992 m), 1045-1692 spawned, 6.3-8.8k live extras, 0 failed, 53-60 fps, no hang.
  Hook cost 0.16-0.25 ms avg per MetaAI update; one 10 s window at 2.4 ms avg / 40 ms max (that
  window had only 174 MetaAI updates instead of ~600, i.e. the game itself stalled then too).

#### Senu (v13): hitches, the cost of a spawned character, memory, strobing

The user's report (v12, 2560x1600, render_scale 0.333, FG on, npc_units=8, npc_density=40): severe
hitches flying Senu over Alexandria (17:31:48-17:32:13 local: 5-27 fps, single frames of 1.2 / 1.6 /
2.8 s), recovered after ~30 s; then 30-38 fps while hovering with 2800-3110 spawned.

- **Senu and the spawning.** Senu = `EagleMode` (ControlMappings.xml scancode 47 = V); hover =
  `EagleStop` (right mouse, held); boost = LShift. A / D steer her (~80 deg/s); mouse look only turns
  the camera. She flies straight along the heading the camera had when she took off, ~9.7 m/s,
  15-30 m above the ground. The spawning follows her: circling 70 m around the centre (k=8)
  spawned swings 1150 <-> 2090 every lap (+50/s, -30/s; despawns 1 per frame). Flying away, the
  player's surroundings empty within a minute. `tools/npc-re/senu.sh` (resolution independent:
  keys and relative mouse only), `senu-ab.sh`.
- **A/B (t1 at 2560x1600, the user's ini, Senu circling R~70 m over the centre for 160 s; spawned
  range, fps min / mean, worst frame, virtual TLS max, game RSS):**

  | npc_units | spawned | fps | worst | vTLS | RSS |
  |---|---|---|---|---|---|
  | 1 | 292-690 | 79.8 / 81.7 (FG cap) | 19.6 ms | 0 | 5.8-6.1 GB |
  | 8 | 1145-2089 | 49.2 / 61.6 | 32.5 ms | 1159 | 7.0-7.35 GB |
  | 12 | 1709-2560 | 42.7 / 54.9 | 38.4 ms | 1646 | 7.8 GB |

  Also straight flights / hovers at 768x480 and 2560x1600 (k=1, 8, 12; seven runs): no frame over
  90 ms in flight. **The user's hitch did not reproduce.** VRAM 5.4-5.7 GB in all (k=8 +250 MB).
- **Per-character cost.** Flying out of the city at k=8 the frame time falls 20.8 -> 12.6 ms as
  spawned goes 1983 -> 1100 (where the FG auto cap holds it): ~9 us of frame time per spawned
  character, which predicts ~30 ms at 3000 (the user's 30-38 fps hovering). The CPU profile
  (`tools/npc-re/perfhot.py`, perf user-mode samples per thread / module / function start) is flat:
  task threads saturated, top function 1.6%.
- **Memory.** The game's RSS follows the most characters spawned at once and does not come back
  when they despawn (cap lowered live: 1238 -> 553 spawned, RSS 7334 -> 7328 MB): ~0.9 MB per
  character (k=1 6.0 GB, k=8 7.3 GB, k=12 7.8 GB over the same flight). The host journal has
  "systemd-journald: Under memory pressure, flushing caches" at 17:31:59, 17:32:00, 17:32:17,
  17:32:20, 17:32:22, inside the hitch window; none during any of the test runs (which never
  hitched). 15 GB of zram swap were in use before the session started. Best explanation: host
  memory pressure, with the k=8 crowd adding ~1.3-2.2 GB to the game. Not proven (inducing memory
  pressure on the user's desktop was not tried).
- **Reproducible hitch: mass unload after a long jump.** ~23 s after a 470 m debug teleport at
  k=8/12 the old area is unloaded at once: 1760 -> 210 spawned in one step, one frame of 1147 /
  1231 ms (k=8 / 12; virtual TLS 838 -> 16). Teleports only (fast travel hides it behind the
  loading screen); scales with the number spawned.
- **Not the cause:** split-lock traps (`x86/split lock detection` for ACOrigins.exe only at startup),
  VRAM (5.5 of 8 GB), our MetaAI hook (0.3-1.2 ms avg in the user's hitch window), the per-frame
  spawn rate (the SpawningManager's sources add up to ~160/s; despawn 1/frame).
- **Fix (v13, `src/world/npc.cpp`):**
  - `[world] npc_mem_floor` (MB, default 3072): when the host's free memory (Wine's
    GlobalMemoryStatusEx; matched MemAvailable within 2%) drops below it, maxSpawned is held at the
    peak spawned so far / 0.8; released 1 GB above the floor. Whenever the cap binds, the game's
    budget pressure (spawn distance x0.9 from 0.8 x cap, x0.8 from 0.9 x cap) takes a standing crowd
    to about half the cap (cap 1140: 1140 -> 562; 1380: 1240 -> 800; 1574: 1253 -> 770), so while
    held the crowd thins to ~60% of its peak and never sets a new high-water mark; it refills within
    5 s of release (768 -> 1118). An emergency brake: off unless memory is short.
  - `[world] npc_senu_max` (default 0 = off): most characters while the camera is more than 18 m
    from Bayek (Senu, or the camera far away for any reason; logged on each change). Applied as
    maxSpawned = limit / 0.8 so the crowd settles near the number. The game evicts the farthest
    first (cap 800 live: 0-40 m 60-76, 40-80 m 451-473 kept, 80-120 m 515 -> 17-23). Circling at
    k=8 with 1500: 809-1449 spawned after take-off (unlimited: 1145-2089), fps min 50 / mean 63
    (unlimited 49 / 62); a cap of exactly 1500 gave 716-1466, 55 / 69.
  - The 10 s npc log line now has the game's working set and the host's free memory, so the next
    report shows whether memory was short when it hitched.
- **Strobing of actors / shadows ("when flying").** Captures (`tools/npc-re/strobe-ab.sh`:
  framedump of 40 consecutive real frames, `tools/strobe.py`; FG present trace of real and
  generated frames, `tools/fgstrobe.py`, `tools/fgbands.py`):
  - real frames at k=8, render_scale 0.333, FG on: small actors and their long shadows move
    smoothly frame to frame in flight (8-frame crops of walkers ~40-60 m below) and from a hover
    (people region change 5.8-7.9 per frame, no alternation: animation is not stepped);
  - generated frames: a generated frame that interpolates its neighbours cannot be brighter or
    darker than both; on-off-on spikes in G presents were below those of R presents everywhere
    (whole band k=8: R 14437 / G 12131 per MP; a window with walkers and their shadows on a wall:
    R 0.30% / G 0.16% of pixels; k=1: R 3239 / G 1232; render_scale 0.6: R 15465 / G 11752);
  - the flip count of real frames in flight is dominated by parallax (roof tiles) and frame rate,
    so it does not compare across variants.
  Not reproduced. The user's log shows them bisecting it themselves (F9 FG off 17:34:02, F10 game
  TAA / DLSS twice 17:34:11-19, FG on again 17:34:20): what they saw in each state decides between
  FG, DLSS Ultra Performance and the game.

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

**Test harness: the Quiver lab** (`quiver lab`, design/LAB.md in the Quiver repo; replaces the
podman containers of `scripts/aco`, now deprecated). The instance is `origins`, configured for
Pharos by `tools/lab/setup` (d3d11=n,b, build/mod as the mod, `pharos`/`aco` file aliases, the
reload pattern, the `lab` graphics profile, locations). Pharos serves the lab channel
(`src/core/lab.cpp`, inert outside the lab): its log and debug outputs (captures, framedumps,
memtool, `where`/`teleport` trigger files) go to the run folder (`QUIVER_LAB_RUN_DIR`, or
`ACO_DLSS_OUT` if set; the manifest's `log_file=run:pharos.log` names the current run's log), it
reports the state `menu` / `in-world`, and answers the verbs `where`, `teleport`, `cfg`, `ping`.

```sh
tools/npc-re/userplaying.sh && echo "the user is playing: launch nothing"   # before every launch
scripts/package
tools/lab/launch --dt giza-sphinx [section.key=value ...]   # build/mod + overrides, into Discovery Tour, teleport
quiver lab wait origins in-world                    # Pharos's state (menu | in-world)
quiver lab where origins                            # {"player":[x,y,z],"yaw":..,"camera":{"pos":..,"yaw":..,"pitch":..}}
quiver lab teleport origins memphis-temple          # player + camera to the location's heading (named: ... list; --no-turn: player only)
tools/lab/teleport memphis-temple --settle 5 --shot m.png   # the same, plus settle/screenshot
quiver lab turn origins 90 -8                       # camera heading alone (closed XTEST loop on `where`)
quiver lab set origins 'pharos:[reflex]fps_limit=60' --wait   # live ini edit, waits for the reload line
quiver lab call origins cfg stereo.separation [value]          # read / set one key, waits for the reload
quiver lab shot origins out.png --settle 2          # screenshot (full display resolution)
quiver lab logs origins                             # the run folder: pharos.log, game.log, DXVK, bridge
quiver lab gpu --for origins -- CMD ...            # exclusive GPU (other instances throttled via their throttle lines)
quiver lab kill origins && quiver lab down origins
```

- **Defaults:** 1280x720@60, Ultra High, DLSS Balanced (PixelDensity 0.58, in the cloned
  ACO.ini). With a real clock there is a 30 fps cap from Pharos (`[reflex] fps_limit=30`, set by
  the profile `tools/lab/profiles/lab.set`). Measured in the lab (r3): the game's own `MaxFPS`
  doesn't hold (set to 30 in ACO.ini before launch, the game wrote 0 back at startup and ran 85
  fps with Frame Generation off); `fps_limit=30` held at 30.0 fps. With the shipped
  `[fg] mode=d3d12`, Frame Generation paces presents to the display (60 Hz), so the game runs 30
  real + 30 generated. `tools/lab/launch` defaults to the virtual clock (next bullet) and sets
  `fps_limit=0` under it unless told otherwise.
- **Measurements at 1920x1080, uncapped** (stereo cost, performance): `quiver lab edit origins
  display=1920x1080@60 graphics=measure`, `quiver lab down origins && quiver lab up origins`, measure
  under `quiver lab gpu --for origins -- ...`; back with `display=1280x720@60 graphics=lab`.
- **Menus:** `tools/lab/dt` recognises the title and main menu with `tools/lab/screen` (the
  2560x1600 crops in `tools/ref/` scaled by H/1600 and searched near their place, so any 16:9 or
  16:10 size works, and the left eye of hsbs stereo too), clicks Discovery Tour, then waits on
  Pharos's `in-world` state. ~80 s from launch to in the world at 1280x720.
- **Virtual clock** (`clock=virtual:62.5`): works with Pharos now (Quiver's clock hooks chain:
  Pharos -> clock -> DXVK; the earlier bypass, where Pharos never saw the swapchain, is fixed).
  Measured live 2026-11-20 (Quiver 8e053ce; runs r6 with Frame Generation on, r7 with
  `fg.mode=off stereo.mode=sbs`): Denuvo didn't object; Pharos logged its swapchain and
  `stereo: on`, state reached `in-world`, DLSS/stereo ran (L/R difference ~8.7/255 at Giza).
  Speed: ~3.3x real time through boot/menus (r6, first 100 s), 1.13x free-running over r7's whole
  99,464-frame run (1592 s virtual in 1413 s real; the game, not the clock, is the limit in-world
  at 720p Ultra High). No frame was released to real time (`deterministic` in `quiver lab logs`;
  real-time escapes 1 in r6, 0 in r7). Caveat: most frames "creep" (69,350 of 99,464 in r7: a
  thread reads the clock >4096 times in a frame), so those frames' time is not exactly 1/62.5 s.
  Pause/step works as advertised, measured: two screenshots while held were bit-identical,
  `step 30` advanced exactly +30 frames (+0.480000 s), 0 ticks leaked. That makes stills within
  a run exact; bit-identical stills across separate launches are not claimed (menu navigation is
  screenshot-driven in real time, and clouds/NPCs/time of day follow the run's own virtual time).
  `quiver lab turn`/`teleport` camera turning fails while the clock holds the game: resume first.
- **Not ported yet:** the experiment scripts that drive `scripts/aco` or read
  `~/Games/aco-dlss-instances/<inst>/out` (`tools/bench`, `abtest`, `latency*`, `fg-*`, `cloth-*`,
  `npc-re/*.sh`, `lod-re/*.sh`) still use the old harness's paths: port each to `quiver lab` (run
  folder from `quiver lab logs`, `set --wait`, `shot`, `gpu`) when it's next needed. The replaced
  tools (`scripts/aco`, `aco-dt`, `fg-launch`, `ini-set`, `teleport`, `camera`, `where`,
  `gpu-exclusive`) are marked deprecated in their headers.
- `tools/lab/setup` re-applies the instance configuration (and copies `tools/lab/profiles/*.set`
  and `tools/locations.txt` into the lab); `quiver lab show origins` prints it.

**Analysis and RE tools:**

- `tools/npc-re/live.py` (host-side reads of the game's memory, `ACO_INST=t1|t3`), `hist.py`
  (heap objects per class, from `vtnames.json` built by `vtnames.py` from the TypeInfo records),
  `fields.py` (per-offset value distribution across a class's instances), `spawncount.py`.
- `gw.sh` / `gb.sh`: hardware data watchpoints / execution breakpoints via gdb from the host
  (no code bytes modified; the script always deletes the debug registers and detaches; killing gdb
  with the watchpoint armed leaves DR7 set and crashes the game on the next hit). In-process
  guard-page watching (PAGE_GUARD + single step) hangs the game under Wine and was removed.
- `decomp.sh <rva> | @<rva inside a function>`: headless Ghidra decompile of the raw image dump.
- Frozen-game tools (host-side reads; gdb only attaches, reads and detaches):
  - `pestack.py` (gdb -batch -x, `WBASE`, optional `IMG`, `FRAME=1`): live PE call stack of every
    thread. The PE rsp is recovered from Wine's syscall frame on the unix stack; scanning the whole
    PE stack instead mixes in stale frames.
  - `jobq.py`: engine job scheduler queues, task-thread sleep counts, and the busy jobs of the frame
    graph with their prerequisites. `jobwhy.py <job>`: walks a job's unfinished prerequisites down
    to the stuck leaf.
- Senu and crowds in the air: `senu.sh` (fly / circle / hover, samples spawned, virtual TLS,
  fps), `senu-ab.sh`; `perfhot.py <perf.data> <pid>` (perf record -e cpu-clock:u of the game, by
  thread, module and game function start); `strobe-ab.sh` with `tools/strobe.py` (framedumps),
  `tools/fgstrobe.py` / `tools/fgbands.py` (FG present traces, real vs generated).
- Static helpers on the image dump: `rel32.py` (all rel32 / `call [rip+x]` sites of many targets in
  one pass), `callsite.py` (the call before a return address and its function), `fstart.py`,
  `callargs.py` (immediates before each call), `ctorof.py` (TypeInfo records / tables using a
  ctor), `rtti.py` (MSVC RTTI -> vtable).
