# Aegis and Bifrost

Aegis is the Odyssey D3D11 port. Bifrost is the Valhalla D3D12 port. Both have installable Quiver development builds and have been tested in disposable instances. Pharos remains the Origins release.

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

Installed profiles use the port name for SR and frame generation and append `Stereo` for SR and stereo. For example, launch with `quiver play 5059 --profile "Aegis Stereo"` or `quiver play 13504 --profile "Bifrost Stereo"`.

### Startup artwork

Aegis 0.1.0-dev.5 and Bifrost 0.1.0-dev.4 use the **Stereo rendering** setting
for the startup artwork as well as the game. **Off** shows the game's original
image. **Detect stereo display** checks the artwork window's own display.
**On** packs the two views into the original window, with **Swap left and right
eyes** applied. The splash uses half-width side by side with either gameplay
packing. Quiver's **Skip intro videos** fix controls the movies independently.

Odyssey places the warrior and carried weapons in front of the ships and the
distant landscape. Valhalla places the double axe in front of the carved medallion
and preserves the soft transparency around the original artwork.

On a private HypXRland stereo output, both eye orders matched the expected
Odyssey pixels in the native window. Valhalla's partially transparent edge
pixels also matched in the native window, and the stereo image remained
correct after the compositor resized it.
In the retail Valhalla launch, both the packed and original splash matched the
expected artwork, including translucent edges. The title accepted input in both modes.

### Skip intro videos

Each game has a separate Quiver fix for its startup logos and localized notices. Install it from the repository root with `quiver mods 5059 install dist/ports/odyssey-skip-intros` or `quiver mods 13504 install dist/ports/valhalla-skip-intros`. Add **Skip intro videos** to the profiles where you want it. Quiver hides the listed movies for that launch. Remove the fix from a profile to play the intros again.

In English-language lab launches, Odyssey reached the first visible “Press any key” prompt in 58 seconds with the five startup movies and 22 seconds with them hidden (r27–28). Valhalla reached its prompt in 59 and 18 seconds respectively (r47–48, six movies). Both recipes cover the 17 installed localizations.

## Aegis • Odyssey

Aegis 0.1.0-dev.6 supplies HDR10 input to DLSS Frame Generation when Odyssey outputs HDR. When Odyssey switches to a linear FP16 scRGB backbuffer, Aegis converts the scene to PQ before interpolation and converts each generated frame back to scRGB for presentation. The game’s real frames retain their original color. In an RTX 5090 lab capture of Odyssey’s main menu at 1280×720, the same highlights peaked at 399 nits in the real frame on both builds. The generated peak was 80 nits on dev.5 and 398 nits on dev.6. Among pixels above 100 nits in the real image, median generated brightness rose from 47% to 99.7% of the corresponding real pixel.

Some fine moving edges still double on dev.6. A 3× HDR pan across a coastal scaffold showed faint copies of twigs against the sky and a smear where the character’s shield uncovered the background. The test used DLSS Quality (854×480 to 1280×720) and captured the two consecutive real frames, each generated frame, and the motion and depth submitted to DLSS-G. Of 6,312 tracked corner observations, 99.8% landed within 0.11 of their expected fraction of the interval. A 4× walking test found that 1.8% of 1,380 measured outlines widened by more than two output pixels. An experimental 3×3 nearest-depth selection gave more accurate input vectors along silhouettes, but increased the 90th-percentile normalized image error from 0.263 to 0.282 on 1,540 matched depth-boundary observations in a 4× pan. The released build supplies Odyssey’s undilated motion field.

The main Aegis build runs the Pharos D3D11 rendering path with Odyssey's texture slots, constant-buffer ranges, projection layout, config file and game settings path. Its default `aegis.ini` enables DLSS SR and 2× FG. To run stereo, set `[stereo] mode=sbs` before launch. The compositor and per-eye temporal histories use the same scene twice per simulation step. Aegis uses Odyssey’s icon controllers to read the world positions of tracked targets.

Aegis 0.1.0-dev.7 carries the two fixes below: stereo at about half the mono frame rate, and Discovery Tour icons, aimed-marker brackets and distance labels at their targets' depth.

Stereo in Aegis dev.6 ran at about a fifth of the mono frame rate (26 against 133 fps in the user's laptop benchmark). The depth cursor checked every indirect draw under a contended lock, and the immediate context's per-eye camera constants were GPU copies between draws. Both are fixed in the shared renderer. On an RTX 5090 at 1280×720, stereo now reaches 0.45 of the mono frame rate, up from 0.25. docs/STEREO.md (Performance) has the measurements.

Odyssey's temporal pass binds color at t85, packed motion at t86, linear depth at t87, the responsive mask at t88, and history at t89–90. The depth converter's second R32F target carries device depth. The CPU upload mirror reads the original b6 jitter during command recording. The motion shader uses the current camera in b2 and previous object projection in b4. The full-frame DLSS evaluation runs once at the geometry draw and the game still writes its own geometry, sky and dither histories. The UI compositor then samples the DLSS reconstruction underneath the HUD.

On the RTX 5070 Laptop, the 66.7% benchmark rendered at 854×480 and presented at 1280×720. Run r10 saw eight jitter positions and one SR evaluation and compositor substitution per rendered frame. The engine camera moved through the benchmark and 2× FG sustained about 30 rendered plus 30 generated frames per second. In verification sample 45, moving pixels differed by 14.16/255 between the real neighbors and the generated image differed from each by 9.71 and 9.70. The 4× benchmark in r13 sustained 15 rendered plus 45 generated frames per second at 60 Hz. Its moving-scene verification sample 75 measured differences of 22.03 and 21.79/255 from the neighboring real images. Run r23 sustained 10 rendered plus 50 generated frames per second at 6×, with zero failed or dropped frames in the measured interval. Its generated sample 89 differed from the adjacent real frames by 15.32 and 15.69/255 while the real frames differed by 20.35/255. Stereo run r11 reconstructed each eye independently. On the market approach, 275 of 300 presents used a fresh stereo pair and 25 held the preceding pair. The foreground floor and distant hill differed in horizontal disparity. In r12, preserving the flat HUD camera reduced the measured binocular offset of the main-menu controls and the benchmark hint text to zero pixels.

With `[stereo] cursor=depth`, the mouse follows Odyssey’s Windows client pointer. The left stick uses the position of its separately drawn map ring. Run r17 placed the mouse at x=783 on the map and the reconstructed eye centres at 776.7 and 789.3. Main-menu controls and their cursor had zero disparity. In r18, both projected positions of the gamepad sprite were captured before substitution. Their midpoint was (640,360), and the cursor landed at x=633.7 and 646.3 over terrain approximately 4.93 km from the map camera.

Ikaros’s scanning ring samples the centre ray from the game depth buffer. With marker correction disabled its centre was x=319.75 in both 640-pixel half images. With the correction enabled over a distant ridge, the centres were x=314.5 and x=325.5. The compass and button hints use their original projections. The green tracked-location icon is a separate draw. Run r19 measured an unobstructed target at 182.5 m with terrain at 187.2 m. The icon chose the target. A different target was at 200.0 m behind geometry 2.19 m away and the icon chose 2.18 m. Their native billboard depths were 5.8–5.9 m.

#### Discovery Tour icons, the aimed marker and its label

Odyssey draws tracked icons with two cameras. Frame traces in the rayblaster lab (instance `aegis-markers`, runs r1, r4 and r7) identified the draws by the camera bound to each one. Hiding draws in the right eye only (r3 and r4) identified what each draw shows.

- **The scene camera** draws the tour-station banners (128×128 BC7 atlas), the tracked location icons (64×64 BC7 atlas with a 128×128 BC1 mask), the set marker and the brackets around the aimed icon (64×64 BC3). Their distance labels use a separate font shader pair (VS `ec4421bb`, PS `397fb1a1`). Each label is drawn directly after its icon batch. Odyssey places these billboards about 2.2–2.4 m from the camera (the marker shader's readback of native depth in r6 and r7). Before this change only the location-icon atlas was corrected. The banners, brackets, set marker and labels kept their native depth.
- **The flat HUD camera** draws icons of some nearby tracked things, such as the Discovery Tour horse (sprite VS `c2d0ef4d`), off-screen pointers, their distance labels (font VS `09703824` and `d0a1f529`, PS `6c582fcd`), the compass, the tour panel and the control hints. All of these were at zero disparity.

All scene-camera icons now use the shared target matching (`MarkerRole::Ordinary`, `Bracket` or `Logo`). Labels take the depth chosen for the icon batch before them (`Label`). On the HUD camera, an icon or label moves only when it lines up with a visible tracked target (marker shader modes 4 and 5). It takes that target's depth, or the depth of the first surface in front of the target. The compass band, the top-left tour panel and the bottom control hints are excluded. Odyssey's icons float about 2.2 m above a tour station's position. Its allowance for the height of an icon above its target is therefore 3 m, compared with 2 m for Origins. In busy areas, the icon controllers report more than the shader's 192 positions (r6). The marker pass keeps the nearest 192.

Measurements used side-by-side output at 1280×720 (640-pixel eye images), 32.5 mm per eye and 4 m convergence. Disparities are right-eye minus left-eye positions in half-image pixels, from block matching of the same patch. The before runs used the previous marker code.

| Discovery Tour (rayblaster, RTX 5090) | Before (r8) | After (r9) | Scene at the target |
| --- | --- | --- | --- |
| Tour-station banner, 10 m away on foot | −4.80 | +4.07 | +4.19 (r8), +4.07 (r9) |
| Horse icon on screen | — | +3.96 and +3.80 | +4.09 and +3.73 |
| Ikaros: brackets around the aimed marker | +6 | +11 | +10.8 |
| Ikaros: aimed marker's distance label | +6 | +11 | +10.8 |
| Compass, tour panel text, compass distance | 0 | 0 | — |

In r4, before the change, the label of an aimed location icon 373 m away measured +6 pixels. Its icon measured +10.85 and the terrain +10.84. Setting separation to zero reduced every difference to zero. In r9 the station's golden ground ring measured +3.82, between the adjacent ground at +3.55 and +4.12. It is scene geometry and the marker shader does not draw it.

Off-screen pointers stay on the HUD plane. In r6 and r7, matching pointers by their bearing from the screen centre paired the horse's pointer with an unrelated target 1.4 km away. The horse was 6–8 m from the camera. Where Odyssey places pointers for targets behind the camera is unknown. A pointer needs its own target before it can be placed in depth.

Odyssey’s story bow renders its two spread bars and centre dot separately. In r21, hiding the three draws individually removed the right bar, left bar and dot. In r22 Kassandra aimed at the coastal rocks with the Atlantis shortcut loadout. The centre moved from x=320 in both eyes to x=317.5 and x=322.5, and both bars acquired the same five-pixel disparity. The sight keeps the spread authored by the game as the bow is drawn.

Run r24 rendered the Akropolis Discovery Tour with 1280×720 eyes in a 2560×720 swapchain. Three successive 300-frame samples during gameplay contained 300 fresh eye pairs apiece. The first station’s *Learn More* screen uses an immutable 1920×1080 BC7 photograph followed by the caption and button legend on the same UI camera. In r24 all were at screen depth. In r25 the heading, body and controls measured four pixels of crossed disparity between half-images. The photograph measured zero, including its upper-right edge. The viewer preserves the photograph’s full frame and the native vertical bounds of the scrolling text.

For the original single-frame diagnostic, package `odyssey-probe` and launch with `build/ports/aegis-probe/mod` and lab environment `AEGIS_SR_CAPTURE=1`. Touch `capture-sr` in the current run directory during the benchmark to save the input, the centered-jitter reset frame, and the paired raw device depth. The main renderer uses the game's per-frame jitter and decoded motion.

## Bifrost • Valhalla

Bifrost matches named permutations in the SBIN bank against D3D12 PSOs. It decodes SM5.1 register declarations and root-signature ranges and follows CPU descriptor writes and copies into the active graphics descriptor tables. In the tested bank, 956 matching entries reduced to 586 distinct shader signatures.

In the tiled TAA geometry draw, space 2 holds six inputs at t0–t5 (RGBA8, RGBA8, R32F, R8, RGBA8, R8) and three render targets (RGBA8, RGBA8, R8). The resource bound as t2 was the linear-depth pass's RT0. That pass also writes an R32F device-depth copy to RT1. The geometry draw's RT0 reappeared at t1/space4 in the UI composite. These identifications come from matching the resource objects at actual draws in run r6.

The active camera CBV is b0/space1, and the temporal CBV is b0/space2. Touch `capture-camera` in a Bifrost run directory to write their current contents to `bifrost-camera.tsv`. Run r7 found current/previous eye positions at float4 rows 2/3, current camera-to-world rows 6–9, projection 26–29, view-projection 30–33, previous view-projection 38–41 and inverse projection 69–72. Temporal rows 1/2 held display/render sizes and their reciprocals. Row 3.xy held jitter in pixels. The sampled jitter (−0.125, −0.388889) agreed with the projection's offsets at 1280×720.

Bifrost's native frame generation uses the presenting D3D12 graphics queue. A fullscreen pass at the geometry TAA draw copies scene color, decodes packed motion to previous-minus-current UV coordinates, reprojects sky motion, and inverts the engine's linear depth conversion. The original draw's state and all three game TAA outputs are preserved. DLSS-G consumes those motion and depth images, the engine camera and the finished backbuffer. The swapchain presents the interpolated image followed by the untouched real image. The display's refresh rate sets the cadence. Use `[fg] enabled=0` to disable generation or `verify=1` for GPU pixel readback in SDR.

Bifrost splits the native graphics submission after the tiled TAA list. NGX reads the pre-TAA color, decoded motion, device depth and game jitter. It reconstructs at display resolution. The following UI draw combines the original overlay with that result. To fit the game's descriptor heap, the replacement pixel shader reads the reconstructed RGBA8 image through a root buffer. A GPU validity flag selects the game image whenever that frame has no usable reconstruction. Capture textures live with the game's reusable TAA target. Use `[upscaler] enabled=0` for the native scene. With `compare=1`, the left quarter shows native, the next quarter shows four times the absolute RGB difference, and the right half shows SR. All three keep the original HUD. Run r19 showed scene differences on Eivor and the longhouse wall inside the comparison strip while the HUD continued across it.

Set `[fg] count` to the desired display multiplier. Bifrost clamps the value to the driver's reported limit. Run r16 loaded the Grantebridgescire save and sustained about 30 real and 30 generated frames per second at 60 Hz. While walking, 40.5% of sampled color channels changed by more than 8/255 between real frames. The generated frame differed from those neighbors by 8.18 and 8.07/255. In r18, SR ran at 854×480 to 1280×720 during 4× generation. While moving in the same save, the game delivered 300 rendered and 900 generated frames in 20 seconds. In one sample the midpoint frame differed from its real neighbors by 6.22 and 6.28/255. Run r42 delivered 300 rendered and 1,500 generated frames in 30.0 seconds at 6× in the same region. During movement, the intermediate image differed by 6.91/255 from its preceding real frame and 13.36/255 from its following real frame. These runs used SDR on an RTX 5070 Laptop.

Use `count=auto` with `count_min` and `count_max` to let the multiplier respond to sustained frame times. `target_hz=0` uses the display refresh. F9 toggles FG and F10 toggles SR during play. In r30 both started disabled. Enabling them in Grantebridgescire produced reset histories and enabled the HUD-less input. With a deliberately high 240 Hz target, FG advanced from 2× through 3× to 4×. One walking sample at 4× differed by 7.23 and 7.34/255 from its two real neighbors. Toggling SR off and back on switched the FG HUD-less input off and back on while FG kept running.

The DLSS preset can also change during play. Run r43 recreated both stereo histories independently when switching from K to M. When SR is active at the backbuffer format, Bifrost also supplies its display-resolution scene as the HUD-less input to DLSS-G. Set `[fg] hudless=0` to disable this input.

Set `[stereo] mode=sbs` before launch for synchronized half-side-by-side output. Bifrost records the engine's draws and dispatches for both eyes from each simulation step. The right eye owns its render targets, mutable buffers, descriptor heaps, command lists and camera uploads. Its placed resources preserve the game's heap offsets and aliasing. Each eye has its own DLSS history. The presenter checks that the two temporal draws and the right-eye UI composite belong to this flip. Flat screens are duplicated. The earlier alternating-frame experiment remains available through `preview_pair=1` with `mode=off`.

`separation` sets millimetres per eye from the camera centre (default 32.5). `convergence` sets the zero-parallax distance in metres (default 4). Both settings update during play. At zero separation in run r27, the mean absolute difference between the eyes was 0.34/255 at rest and 0.55/255 while Eivor ran through Ravensthorpe. At the default 65 mm total separation in r28, distant foliage shifted 7–8 pixels between the 640-pixel-wide eye images, part of Eivor shifted one pixel in the opposite direction, and the quest heading and compass stayed aligned. Changing separation back to zero in the same session reduced the difference to 0.38/255. Each eye reconstructed 854×480 to 1280×720 before packing.

Use `mode=auto` for stereo-display detection at startup. `output=sbs` requests a swapchain twice the logical game width. Run r34 presented 1280×720 eyes in a 2560×720 swapchain, advertised full SBS through the Quiver SDK and reached Grantebridgescire. The compositor can scale that image to its display. `swap=1` exchanges the packed eyes. `show=left`, `right` or `diff` selects a diagnostic view.

Bifrost reads projected tracked locations from Valhalla’s UI icon controllers. Its world-diamond shader chooses the closer of the tracked position and the first visible surface from the primary eye. It keeps three frames of reversed depth with decreasing weights to damp movement at silhouette edges. The location-diamond draw was identified in r39 by removing just its 160×80 atlas. In r40 Synin’s scanning ring moved from x=320 in both half images to x=314.5 and x=325.5 over the distant landscape. Both eyes use the same centre ray.

The depth cursor tracks the actual Windows pointer in menus and on the world map. It compares small patches from the two finished eye images to find the disparity of the hovered control or terrain. The left stick drives Valhalla’s own map ring and dot. In r40 the mouse was at x=426.25 and x=432.25 over map terrain. The controller ring moved from x=319.75 in both eyes to x=316.5 and x=323.0. Removing the 256×256 ring and 64×64 dot individually identified their separate draws. In r43 the corrected centre dot was at x=316.0 and x=322.6, in line with the ring. The same run captured 85 world targets. One lay 880.82 m along the camera axis behind terrain at 224.27 m. Applying the shader’s foreground margin selected 223.16 m.

Valhalla’s hunter bow uses a 16×16 dot and two 32×16 spread bars. Run r43 isolated the dot and bars by removing their respective draws. In r44 all three moved together by about two half-image pixels across the eyes at the shop wall.

For a first-person Predator Bow, `[stereo] aim_eye=left` places the native camera at the left eye and puts the full binocular separation on the opposite eye. Choose `right` or `center` in the live Quiver options. The matching eye supplies the target depth. The observed Needler camera had an 18° vertical field of view and a 0.10 m near plane. Synin’s scanning camera had a 30° field of view and a 0.80 m near plane. The renderer checks the projection and the presence of the aiming-dot draw before shifting the pair. In r44 the two sight rings at centred aim were 23 and 25 half-image pixels to either side of the zero-separation reference. With left-eye aiming the left ring returned to its native horizontal position and the opposite ring moved 48 pixels. With right-eye aiming the right ring was within two pixels of its reference and the opposite ring moved 46 pixels.

The lab has also seen an intermittent game exit while checking add-ons or loading a save in FG and stereo runs, including stereo with SR disabled. The fault was a write to address 0x2 at ACValhalla.exe+0x65218d. Stereo runs r27, r28, r34, r38–40 and r43–44 reached gameplay. Clean controls r22, r32 and r33 passed the add-on check. Two subsequent unmodified controls, r45 and r46, loaded the same Fornburg save used in r44. The cause remains open.

For a command inventory, set `[stereo] trace=1` before launch and create `capture-stereo` in the run directory. The next captured frame logs its queue submissions, camera ranges, query operations, aliasing barriers and resource-state changes. The renderer consumes the trigger after one capture.
