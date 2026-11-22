# Origins stereo startup artwork

Pharos embeds the finished 1920×540 side-by-side bitmap. To regenerate it, capture
the game's original 960×540 splash window at native size as `splash.png`.
The reference capture's top-down RGB8 samples have SHA-256
`295c122610ad7741cdb42373e5842c578579614ae486d7f7fe7409cfcbe72349`.
From the repository root, run

```sh
c++ -std=c++17 -O2 tools/splash/bake.cpp -lpng -o build/bake-splash
build/bake-splash splash.png tools/splash/art build/origins-splash.png
magick build/origins-splash.png -type TrueColor -depth 8 BMP3:src/core/art/origins_splash_stereo.bmp
```

The grayscale background map encodes inverse depth. Bayek and the title each have a
separate antialiased matte. The two colour patches provide the underlying scene that
becomes visible as those layers move. The patch behind Bayek comes from the registered
hero-free Origins catalogue artwork. The patch under the lettering reconstructs the
pyramid, ridge and palm. At the centre camera position the layers reassemble to the
original pixels. All camera shifts are horizontal.

The built-in ImageGen tool supplied the initial background depth map from the registered
hero-free plate. The final map follows colour boundaries in the original image and models
the ridge of the main pyramid. The colour shown to each eye comes from the game artwork.

<details>
<summary>Background depth-map prompt</summary>

Use case: precise-object-edit. Edit the single reference photograph into a precisely pixel-registered grayscale inverse depth field for a stereo renderer. SAME CROP AND EXACT SILHOUETTES as input, panoramic 16:9 edge-to-edge. Every pixel indicates only relative distance: bright means near the viewer, dark means distant. Retain the EXACT subject placement and shapes. Omit all lettering from depth, allow the shape of underlying pyramid to continue through its lettering. Preserve this empty landscape: NO PERSON OR HERO in the foreground. Sky very dark (value 16); pyramids dark gray 35, distant skyline 50, sphinx 65, central temples 80, distant shore and statues 95, boats ~115, large right palm 142 gray following its actual trunk and fronds, nearest right-hand stone walls 160–182, river smooth inverse-depth gradient ~95 at its far bank to ~148 at its near bank; sand shore follows actual silhouette with continuous depth at wet shoreline then a smooth depth ramp to foreground bottom ~225. Do not make the sand into a uniformly white sheet or a surface at a discontinuity from adjacent water. Trace distinct silhouettes precisely at their original positions. Smooth solid object interiors, no photographic highlights, no texture, no contour outlines, no shadows or lighting. No visible writing, scales, legends, frame or labels. This is a single clean depth-only map, completely filling image, for the actual art at exact registration.

</details>

To make a two-second animated preview, supply a directory for the intermediate views
and encode them at 20 frames per second.

```sh
build/bake-splash splash.png tools/splash/art build/origins-splash.png build/splash-views
ffmpeg -framerate 20 -i build/splash-views/%02d.png -c:v libwebp_anim -quality 84 -loop 0 build/origins-splash.webp
```
