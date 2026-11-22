# Valhalla startup artwork

The shipped artwork is a 900×506 RGBA image of a turquoise double axe on a carved
stone disc. Both blades, their interwoven handles and the lower crescent share
one foreground layer. The ravens and beasts rise from the outer slab in shallow
relief. Eight foreground sparks have their own layer. The margins retain the
game's original alpha, including its partially transparent edge pixels.

To regenerate the embedded bitmap and a full-size preview, run these commands
from the repository root with the installed game's `splashscreen.png`.

```sh
c++ -O2 -std=c++17 tools/splash/bake.cpp -lpng -o build/bake-splash
build/bake-splash splashscreen.png tools/splash/valhalla build/valhalla-eyes.png
python3 tools/splash/valhalla/write_dib.py splashscreen.png build/valhalla-eyes.png src/ports/art/valhalla_splash_stereo.bmp --preview-png build/valhalla-sbs.png
```

The writer uses Pillow. It stores unpremultiplied BGRA in a bottom-up 32-bit
Windows bitmap. The runtime premultiplies the two eyes for the layered window.

Built-in ImageGen supplied a plate of unobstructed stone and an initial carving
height field. The final inputs register the carving to the native image contours,
fit the disc rim and preserve the original turquoise silhouettes and openings.
With 900 pixels per eye, the two blades, handle and lower crescent each measure
+6 pixels of horizontal disparity. The disc measures −5 pixels, outer stone −4,
animal details −1 to −2, and a foreground ember +7. The center camera reproduces
the original RGB values exactly.

<details>
<summary>Plate and relief prompts</summary>

Use case: precise-object-edit. The attached image is the edit target. Create a registered clean background plate for a binocular image of this particular 900 by 506 stone artwork. Remove the bright turquoise double-axe Assassin's Creed emblem at center, including its thin black outlines and cast shadow. Reconstruct ONLY the missing gray-blue weathered stone in its footprint. Exactly preserve all external stone texture, the large stone circle, the crack paths around and through the original emblem, carved knotwork animals at outer edges, tiny orange sparks at the bottom, lighting, scale, coordinates, and the 16:9 crop. No new designs, lettering or subjects. The only image difference should be natural continuation of the stone behind the removed central emblem.

Use case: precise-object-edit. The supplied 16:9 game startup artwork contains a slab with a circular inset plus FOUR elaborate carved Nordic beasts around the edges. Convert this image into a precision grayscale height field registered to the EXACT same artwork coordinates, for rendering bas-relief on the actual stone texture. All visible guilloche and protruding parts of the four carved animal figures and interwoven ribbons around the perimeter must be raised medium gray from the much darker surrounding stone. The carved animal heads, feet, feathers and knotted coils around the left and right edges should remain recognisable as a somewhat raised cohesive bas-relief, with slightly recessed channels in the carved lines. Model the central circular medallion as a shallow inset stone disc with a defined slightly raised lip around its circular circumference. Fill the center as smooth stone; exclude the central turquoise axe insignia and any sparks, those are separate image layers. Smooth, low-frequency depth ONLY, no diffuse lighting, shadows or photographic texture, no title or letters. Preserve the exact positions and proportions of the original silhouettes to within one pixel after resizing. Every surface should have plausible bas-relief on one stone slab, not a landscape. Output a full frame depth map, no border or writing.

</details>
