"""
send_image.py — Send an image to the ESP32 display driver over TCP.

Converts the image to raw RGB565 (240x320) and streams it over a socket.

Usage:
    python send_image.py <host> <image_path> [width] [height] [port]

Examples:
    python send_image.py 192.168.1.100 photo.jpg
    python send_image.py 192.168.1.100 photo.jpg 240 320 8080

Requires Pillow:
    pip install Pillow
"""

import sys
import socket
import struct
from PIL import Image


def send_image(host: str, port: int, image_path: str, width: int, height: int) -> None:
    img = Image.open(image_path).convert("RGB")
    img = img.resize((width, height), Image.LANCZOS)
    pixels = list(img.getdata())

    # Convert to RGB565, big-endian byte order
    raw = bytearray()
    for r, g, b in pixels:
        rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        raw.append((rgb565 >> 8) & 0xFF)
        raw.append(rgb565 & 0xFF)

    print(f"Image: {width}x{height}, {len(raw)} bytes")
    print(f"Connecting to {host}:{port} ...")

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((host, port))
        s.sendall(raw)

    print("Sent!")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    host  = sys.argv[1]
    image = sys.argv[2]
    w     = int(sys.argv[3]) if len(sys.argv) > 3 else 240
    h     = int(sys.argv[4]) if len(sys.argv) > 4 else 320
    port  = int(sys.argv[5]) if len(sys.argv) > 5 else 8080

    send_image(host, port, image, w, h)
