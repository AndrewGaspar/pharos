# Aegis startup artwork

The source is the installed Odyssey `splashscreen.png` (700 × 360, file SHA-256 `e2b0fae46d737dce6adb9bba05ee70f72a66497dcd9441a917100a4dcf502dc3`). Use that PNG and the layers in this directory to reproduce the embedded image from the repository root.

```sh
mkdir -p build
c++ -std=c++17 -O2 tools/splash/bake.cpp -lpng -o build/bake-splash
build/bake-splash /path/to/Odyssey/splashscreen.png tools/splash/odyssey build/odyssey-splash.png
magick build/odyssey-splash.png -type TrueColor -depth 8 BMP3:src/ports/art/odyssey_splash_stereo.bmp
```

The title uses a registered patch from Ubisoft's title-free catalogue illustration for game 5059. The warrior, including his helmet glint, cape, spear and dagger, moves on one foreground plane. A traced silhouette and colour-guided mask separate fine weapon edges and the open space under his arm. The patch underneath uses nearby sky, water and grass at the silhouette and a generated fill inside it. Each boat uses a single depth for the sail, mast and hull, matched to the water at its hull. The colossus and trident share a plane. The center view reproduces the source pixels exactly. See the [shared baker instructions](../README.md) for animated preview output.

The built-in ImageGen tool supplied the initial clean plate and landscape depth map, using the installed PNG as reference for both. The baked images use the registered and corrected layers in this directory.

<details>
<summary>Initial clean plate prompt</summary>

precise-object-edit. Produce a pixel-registered clean background plate for stereoscopic reconstruction. Take the supplied widescreen Assassin's Creed Odyssey original image, and ERASE ONLY the standing central red-plumed Greek warrior in the grass, including both legs, helmet, draped armor, arms, plume, and the two weapons held in his hands extending down-left and down-right. Reconstruct the small missing parts of the water, hill and sunlit grass visible behind him. Also erase the central title text, filling in the original sky/clouds, and rightmost small part of the stone statue. Preserve all remaining original art precisely; maintain identical edge-to-edge aspect ratio, crop, framings, pixel coordinates and vessel positions. Keep the left and right near combatants and ornate shield/blade, columns, eagle silhouette, lands, boats, statue, and buildings. No text, figures added or relighting. Matching the existing picture exactly outside the two removed elements is paramount.

</details>

<details>
<summary>Initial background depth prompt</summary>

precise-object-edit. From the supplied wide Odyssey key art (700:360 aspect, two columns left, temple / statue right), create a perfectly registered edge-to-edge grayscale INVERSE DEPTH MAP of the background, for true stereo parallax. Identical composition and object silhouettes. Ignore the central red warrior, all his held gear, AND the title lettering; they are independent layers, fill their positions with underlying sky/terrain/water. Blackish sky 22 and distant hills 32. Eagle dark gray 67. Monumental stone statue and pedestal on right cliff ~67, its torso and hand/trident at one coherent distance; ridge temple and cliff coherent. Middle-right shore temple ~96; foreground near banks and grasses smoothly advancing 135-235 downward. Far sea coast about 78, water depth graded from 78 on far shore to about 119 near the shore, matching both at contact. Left sailboat mast, sail, deck, hull, rigging at SINGLE medium-gray distance ~103, matching water at hull. Middle tiny distant vessel similarly a single, farther gray. Left large near standing columns about 125–140, front tree around 144. Big cropped helmet/shield/gear at extreme left and arm/sword at right very bright at 244 (same distance along entire blades). Keep exact aspect and shapes, no object should be broken into bright/dark unrelated surfaces. Grayscale depth only, no text, axes, frames, artistic contour outlines, shading from artwork, highlights, or photographic texture.

</details>
