"""
send_audio.py -- Send audio to the ESP32 as raw 16-bit mono PCM over TCP.

Decodes any format supported by pydub (MP3, WAV, FLAC, …) on the PC,
resamples to 44100 Hz mono, and streams the raw int16 bytes at real-time
playback speed so the ESP32's stream buffer is never flooded.

Usage:
    python3 send_audio.py <host> <file> [port]

Example:
    python3 send_audio.py 192.168.1.100 sound.mp3
    python3 send_audio.py 192.168.1.100 sound.wav 8081

Requires pydub + ffmpeg:
    pip install pydub
    sudo apt install ffmpeg   # Debian/Ubuntu; brew install ffmpeg on macOS
"""

import sys
import socket
import time
from pydub import AudioSegment


SAMPLE_RATE  = 44100
CHUNK_BYTES  = 4410   # 50 ms of audio per send (44100 * 0.050 * 2 bytes)


def send_audio(host: str, port: int, path: str) -> None:
    print(f"Loading {path} ...")
    audio = AudioSegment.from_file(path)
    audio = audio.set_frame_rate(SAMPLE_RATE).set_channels(1).set_sample_width(2)
    raw = audio.raw_data  # raw 16-bit little-endian mono PCM

    duration_s = len(audio) / 1000.0
    print(f"Duration : {duration_s:.1f} s")
    print(f"PCM size : {len(raw)} bytes ({len(raw)/1024:.1f} KB)")
    print(f"Target   : {host}:{port}")
    print(f"Streaming at real-time rate ({SAMPLE_RATE * 2 / 1024:.1f} KB/s) ...")

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((host, port))
        offset = 0
        t0 = time.monotonic()
        while offset < len(raw):
            chunk = raw[offset:offset + CHUNK_BYTES]
            s.sendall(chunk)
            offset += len(chunk)

            # Sleep until the ESP32 should have consumed this chunk
            # i.e. pace sender to real-time playback speed
            expected_t = offset / (SAMPLE_RATE * 2)
            sleep_for = (t0 + expected_t) - time.monotonic()
            if sleep_for > 0:
                time.sleep(sleep_for)

    elapsed = time.monotonic() - t0
    print(f"Done. ({elapsed:.1f} s sent, {duration_s:.1f} s of audio)")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    host = sys.argv[1]
    path = sys.argv[2]
    port = int(sys.argv[3]) if len(sys.argv) > 3 else 8081

    send_audio(host, port, path)
