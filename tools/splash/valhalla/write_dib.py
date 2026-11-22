"""Write a bottom-up 32-bit BI_RGB bitmap with the native splash's straight alpha."""

import argparse
from pathlib import Path
import struct

from PIL import Image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("native", type=Path)
    parser.add_argument("stereo", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--preview-png", type=Path,
                        help="also save the full-size stereo pair with its native alpha")
    args = parser.parse_args()
    with Image.open(args.native) as original, Image.open(args.stereo) as pair:
        width, height = original.size
        if pair.size != (2 * width, height):
            parser.error("expected the complete native dimensions for each eye")
        rgba = pair.convert("RGBA")
        alpha = original.convert("RGBA").getchannel("A")
        pair_alpha = Image.new("L", rgba.size)
        pair_alpha.paste(alpha, (0, 0))
        pair_alpha.paste(alpha, (width, 0))
        rgba.putalpha(pair_alpha)
        if args.preview_png:
            rgba.save(args.preview_png)
        pixels = rgba.transpose(Image.Transpose.FLIP_TOP_BOTTOM).tobytes("raw", "BGRA")
        with args.output.open("wb") as output:
            output.write(struct.pack("<2sIHHI", b"BM", 54 + len(pixels), 0, 0, 54))
            output.write(struct.pack("<IiiHHIIiiII", 40, rgba.width, height, 1, 32, 0,
                                     len(pixels), 0, 0, 0, 0))
            output.write(pixels)


if __name__ == "__main__":
    main()
