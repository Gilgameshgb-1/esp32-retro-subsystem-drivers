"""
send_av.py -- Send an image and audio to the ESP32 simultaneously.

Sends RGB565 image data to port 8080 and raw 16-bit mono PCM to port 8081
at the same time using threads.  Audio is decoded/resampled on the PC
(any format pydub supports: MP3, WAV, FLAC, …).

Usage:
    python3 send_av.py <host> <image> <audio>

Example:
    python3 send_av.py 192.168.1.100 photo.jpg sound.mp3

Requires Pillow + pydub + ffmpeg:
    pip install Pillow pydub
    sudo apt install ffmpeg   # Debian/Ubuntu; brew install ffmpeg on macOS
"""

import sys
import socket
import time
import threading
from PIL import Image
from pydub import AudioSegment

SAMPLE_RATE  = 44100
CHUNK_BYTES  = 4410   # 50 ms of audio per send


def send_image(host: str, image_path: str) -> None:
    img = Image.open(image_path).convert("RGB")
    img = img.resize((240, 320), Image.LANCZOS)
    pixels = list(img.getdata())

    raw = bytearray()
    for r, g, b in pixels:
        rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        raw.append((rgb565 >> 8) & 0xFF)
        raw.append(rgb565 & 0xFF)

    print(f"[image] Connecting to {host}:8080 ({len(raw)} bytes)")
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((host, 8080))
        s.sendall(raw)
    print("[image] Done.")


def send_audio(host: str, audio_path: str) -> None:
    print(f"[audio] Loading {audio_path} ...")
    audio = AudioSegment.from_file(audio_path)
    audio = audio.set_frame_rate(SAMPLE_RATE).set_channels(1).set_sample_width(2)
    raw = audio.raw_data

    print(f"[audio] Connecting to {host}:8081 ({len(raw)} bytes)")
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((host, 8081))
        offset = 0
        t0 = time.monotonic()
        while offset < len(raw):
            chunk = raw[offset:offset + CHUNK_BYTES]
            s.sendall(chunk)
            offset += len(chunk)
            expected_t = offset / (SAMPLE_RATE * 2)
            sleep_for = (t0 + expected_t) - time.monotonic()
            if sleep_for > 0:
                time.sleep(sleep_for)
    print("[audio] Done.")


if __name__ == "__main__":
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(1)

    host       = sys.argv[1]
    image_path = sys.argv[2]
    audio_path = sys.argv[3]

    t_image = threading.Thread(target=send_image, args=(host, image_path))
    t_audio = threading.Thread(target=send_audio, args=(host, audio_path))

    t_image.start()
    t_audio.start()

    t_image.join()
    t_audio.join()

    print("All done.")
