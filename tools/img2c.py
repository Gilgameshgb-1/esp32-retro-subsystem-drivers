"""
img2c.py — Convert an image to an RGB565 C header for the ILI9341 driver.

Usage:
    python3 img2c.py <input_image> <output.h> [width] [height]

    width/height default to 240x320 (full screen).

Example:
    python3 img2c.py photo.jpg main/image.h
    python3 img2c.py photo.jpg main/image.h 120 160

Requires Pillow:
    pip install Pillow
"""

import sys
import os
from PIL import Image


def convert(input_path: str, output_path: str, width: int, height: int) -> None:
    img = Image.open(input_path).convert("RGB")
    img = img.resize((width, height), Image.LANCZOS)

    pixels = list(img.getdata())
    var_name = os.path.splitext(os.path.basename(output_path))[0]
    # Sanitize to a valid C identifier
    var_name = "".join(c if c.isalnum() else "_" for c in var_name)

    with open(output_path, "w") as f:
        f.write(f"#pragma once\n\n")
        f.write(f"#include <stdint.h>\n\n")
        f.write(f"#define {var_name.upper()}_WIDTH   {width}\n")
        f.write(f"#define {var_name.upper()}_HEIGHT  {height}\n\n")
        f.write(f"/* RGB565, big-endian byte order (high byte first) */\n")
        f.write(f"const uint8_t {var_name}[{width * height * 2}] = {{\n")

        for i, (r, g, b) in enumerate(pixels):
            rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            hi = (rgb565 >> 8) & 0xFF
            lo = rgb565 & 0xFF

            if i % width == 0:
                f.write("    ")
            f.write(f"0x{hi:02X}, 0x{lo:02X},")
            if i % width == width - 1:
                f.write("\n")
            else:
                f.write(" ")

        f.write("};\n")

    print(f"Done: {width}x{height} image → {output_path}  ({width * height * 2} bytes)")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    w = int(sys.argv[3]) if len(sys.argv) > 3 else 240
    h = int(sys.argv[4]) if len(sys.argv) > 4 else 320

    convert(sys.argv[1], sys.argv[2], w, h)
