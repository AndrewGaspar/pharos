# Aegis and Bifrost

Aegis is the Odyssey D3D11 port. Bifrost is the Valhalla D3D12 port. Both currently run as development builds in disposable Quiver instances. Pharos remains the Origins release.

## Build and run

Use the Pharos build prerequisites and pass the directory containing the installed game's shader bank. The packager reads that bank and writes a signature table beside the proxy. Regenerate the table after a game update. The package contains no game shader bytecode.

```sh
scripts/package-port odyssey "/path/to/Assassin's Creed Odyssey"
scripts/package-port valhalla "/path/to/Assassin's Creed Valhalla"

quiver lab edit aegis dll.d3d11=n,b env.PROTON_ENABLE_NVAPI=1 env.DXVK_ENABLE_NVAPI=1
quiver lab edit bifrost dll.d3d12=n,b env.PROTON_ENABLE_NVAPI=1 env.VKD3D_CONFIG=descriptor_heap
quiver lab up aegis
quiver lab launch aegis --mod build/ports/aegis/mod --wait frame
quiver lab up bifrost
quiver lab launch bifrost --mod build/ports/bifrost/mod --wait frame
```

For Quiver’s normal launcher, the packager also writes `build/ports/aegis/package` and `build/ports/bifrost/package`. Install the desired folder with `quiver mods 5059 install build/ports/aegis/package` or `quiver mods 13504 install build/ports/bifrost/package`. Each package declares its Wine DLL overrides, NVAPI settings and Options page. Select the mod in a profile to play with it.

These commands assume Odyssey and Valhalla instances named `aegis` and `bifrost`. Each run writes `aegis.log` or `bifrost.log` in the Quiver run directory. Use separate launches for frame generation and stereo, as with Pharos.

## Aegis • Odyssey

The main Aegis build runs the Pharos D3D11 rendering path with Odyssey's texture slots, constant-buffer ranges, projection layout, config file and game settings path. Its default `aegis.ini` enables DLSS SR and 2× FG. To run stereo, set `[stereo] mode=sbs` before launch. The compositor and per-eye temporal histories use the same scene twice per simulation step. Aegis uses Odyssey’s icon controllers to read the world positions of tracked targets.

Odyssey's temporal pass binds color at t85, packed motion at t86, linear depth at t87, the responsive mask at t88, and history at t89–90. The depth converter's second R32F target carries device depth. The CPU upload mirror reads the original b6 jitter during command recording. The motion shader uses the current camera in b2 and previous object projection in b4. The full-frame DLSS evaluation runs once at the geometry draw and the game still writes its own geometry, sky and dither histories. The UI compositor then samples the DLSS reconstruction underneath the HUD.

On the RTX 5070 Laptop, the 66.7% benchmark rendered at 854×480 and presented at 1280×720. Run r10 saw eight jitter positions and one SR evaluation and compositor substitution per rendered frame. The engine camera moved through the benchmark and 2× FG sustained about 30 rendered plus 30 generated frames per second. In verification sample 45, moving pixels differed by 14.16/255 between the real neighbors and the generated image differed from each by 9.71 and 9.70. The 4× benchmark in r13 sustained 15 rendered plus 45 generated frames per second at 60 Hz. Its moving-scene verification sample 75 measured differences of 22.03 and 21.79/255 from the neighboring real images. Stereo run r11 reconstructed each eye independently. On the market approach, 275 of 300 presents used a fresh stereo pair and 25 held the preceding pair. The foreground floor and distant hill differed in horizontal disparity. In r12, preserving the flat HUD camera reduced the measured binocular offset of the main-menu controls and the benchmark hint text to zero pixels.

With `[stereo] cursor=depth`, the mouse follows Odyssey’s Windows client pointer. The left stick uses the position of its separately drawn map ring. Run r17 placed the mouse at x=783 on the map and the reconstructed eye centres at 776.7 and 789.3. Main-menu controls and their cursor had zero disparity. In r18, both projected positions of the gamepad sprite were captured before substitution. Their midpoint was (640,360), and the cursor landed at x=633.7 and 646.3 over terrain approximately 4.93 km from the map camera.

Ikaros’s scanning ring samples the centre ray from the game depth buffer. With marker correction disabled its centre was x=319.75 in both 640-pixel half images. With the correction enabled over a distant ridge, the centres were x=314.5 and x=325.5. The compass and button hints use their original projections. The green tracked-location icon is a separate draw. Run r19 measured an unobstructed target at 182.5 m with terrain at 187.2 m. The icon chose the target. A different target was at 200.0 m behind geometry 2.19 m away and the icon chose 2.18 m. Their native billboard depths were 5.8–5.9 m.

Odyssey’s story bow renders its two spread bars and centre dot separately. In r21, hiding the three draws individually removed the right bar, left bar and dot. In r22 Kassandra aimed at the coastal rocks with the Atlantis shortcut loadout. The centre moved from x=320 in both eyes to x=317.5 and x=322.5, and both bars acquired the same five-pixel disparity. The sight keeps the spread authored by the game as the bow is drawn.

For the original single-frame diagnostic, package `odyssey-probe` and launch with `build/ports/aegis-probe/mod` and lab environment `AEGIS_SR_CAPTURE=1`. Touch `capture-sr` in the current run directory during the benchmark to save the input, the centered-jitter reset frame, and the paired raw device depth. The main renderer uses the game's per-frame jitter and decoded motion.

## Bifrost • Valhalla

Bifrost matches named permutations in the SBIN bank against D3D12 PSOs. It decodes SM5.1 register declarations and root-signature ranges and follows CPU descriptor writes and copies into the active graphics descriptor tables. In the tested bank, 956 matching entries reduced to 586 distinct shader signatures.

In the tiled TAA geometry draw, space 2 holds six inputs at t0–t5 (RGBA8, RGBA8, R32F, R8, RGBA8, R8) and three render targets (RGBA8, RGBA8, R8). The resource bound as t2 was the linear-depth pass's RT0. That pass also writes an R32F device-depth copy to RT1. The geometry draw's RT0 reappeared at t1/space4 in the UI composite. These identifications come from matching the resource objects at actual draws in run r6.

The active camera CBV is b0/space1, and the temporal CBV is b0/space2. Touch `capture-camera` in a Bifrost run directory to write their current contents to `bifrost-camera.tsv`. Run r7 found current/previous eye positions at float4 rows 2/3, current camera-to-world rows 6–9, projection 26–29, view-projection 30–33, previous view-projection 38–41 and inverse projection 69–72. Temporal rows 1/2 held display/render sizes and their reciprocals. Row 3.xy held jitter in pixels. The sampled jitter (−0.125, −0.388889) agreed with the projection's offsets at 1280×720.

Bifrost's native frame generation uses the presenting D3D12 graphics queue. A fullscreen pass at the geometry TAA draw copies scene color, decodes packed motion to previous-minus-current UV coordinates, reprojects sky motion, and inverts the engine's linear depth conversion. The original draw's state and all three game TAA outputs are preserved. DLSS-G consumes those motion and depth images, the engine camera and the finished backbuffer. The swapchain presents the interpolated image followed by the untouched real image. The display's refresh rate sets the cadence. Use `[fg] enabled=0` to disable generation or `verify=1` for GPU pixel readback in SDR.

Bifrost splits the native graphics submission after the tiled TAA list. NGX reads the pre-TAA color, decoded motion, device depth and game jitter. It reconstructs at display resolution. The following UI draw combines the original overlay with that result. To fit the game's descriptor heap, the replacement pixel shader reads the reconstructed RGBA8 image through a root buffer. A GPU validity flag selects the game image whenever that frame has no usable reconstruction. Capture textures live with the game's reusable TAA target. Use `[upscaler] enabled=0` for the native scene. With `compare=1`, the left quarter shows native, the next quarter shows four times the absolute RGB difference, and the right half shows SR. All three keep the original HUD. Run r19 showed scene differences on Eivor and the longhouse wall inside the comparison strip while the HUD continued across it.

Set `[fg] count` to the desired display multiplier. Bifrost clamps the value to the driver's reported limit. Run r16 loaded the Grantebridgescire save and sustained about 30 real and 30 generated frames per second at 60 Hz. While walking, 40.5% of sampled color channels changed by more than 8/255 between real frames. The generated frame differed from those neighbors by 8.18 and 8.07/255. In r18, SR ran at 854×480 to 1280×720 during 4× generation. While moving in the same save, the game delivered 300 rendered and 900 generated frames in 20 seconds. In one sample the midpoint frame differed from its real neighbors by 6.22 and 6.28/255. These runs used SDR on an RTX 5070 Laptop.

Use `count=auto` with `count_min` and `count_max` to let the multiplier respond to sustained frame times. `target_hz=0` uses the display refresh. F9 toggles FG and F10 toggles SR during play. In r30 both started disabled. Enabling them in Grantebridgescire produced reset histories and enabled the HUD-less input. With a deliberately high 240 Hz target, FG advanced from 2× through 3× to 4×. One walking sample at 4× differed by 7.23 and 7.34/255 from its two real neighbors. Toggling SR off and back on switched the FG HUD-less input off and back on while FG kept running.

When SR is active at the backbuffer format, Bifrost also supplies its display-resolution scene as the HUD-less input to DLSS-G. Set `[fg] hudless=0` to disable this input.

Set `[stereo] mode=sbs` before launch for synchronized half-side-by-side output. Bifrost records the engine's draws and dispatches for both eyes from each simulation step. The right eye owns its render targets, mutable buffers, descriptor heaps, command lists and camera uploads. Its placed resources preserve the game's heap offsets and aliasing. Each eye has its own DLSS history. The presenter checks that the two temporal draws and the right-eye UI composite belong to this flip. Flat screens are duplicated. The earlier alternating-frame experiment remains available through `preview_pair=1` with `mode=off`.

`separation` sets millimetres per eye from the camera centre (default 32.5). `convergence` sets the zero-parallax distance in metres (default 4). Both settings update during play. At zero separation in run r27, the mean absolute difference between the eyes was 0.34/255 at rest and 0.55/255 while Eivor ran through Ravensthorpe. At the default 65 mm total separation in r28, distant foliage shifted 7–8 pixels between the 640-pixel-wide eye images, part of Eivor shifted one pixel in the opposite direction, and the quest heading and compass stayed aligned. Changing separation back to zero in the same session reduced the difference to 0.38/255. Each eye reconstructed 854×480 to 1280×720 before packing.

Use `mode=auto` for stereo-display detection at startup. `output=sbs` requests a swapchain twice the logical game width. Run r34 presented 1280×720 eyes in a 2560×720 swapchain, advertised full SBS through the Quiver SDK and reached Grantebridgescire. The compositor can scale that image to its display. `swap=1` exchanges the packed eyes. `show=left`, `right` or `diff` selects a diagnostic view.

The lab has also seen an intermittent game exit during the add-on check, before loading a save, in both FG and stereo runs. The game wrote to address 0x2 at ACValhalla.exe+0x65218d. Stereo runs r27, r28 and r34 reached gameplay. Clean controls r22, r32 and r33 passed the check. Its cause remains open. Title-specific world-marker and targeting depth remain under development.

For a command inventory, set `[stereo] trace=1` before launch and create `capture-stereo` in the run directory. The next captured frame logs its queue submissions, camera ranges, query operations, aliasing barriers and resource-state changes. The renderer consumes the trigger after one capture.
