"""
send_video.py -- Stream video + audio to the ESP32 simultaneously.

Extracts frames from any video file, converts them to RGB565, and streams
them to port 8080.  Audio is decoded to raw 16-bit mono PCM and streamed
to port 8081.  Both start at the same time via threads.

The ESP32 renders each 240x320 frame as it arrives; TCP flow control
naturally paces the video to match display throughput (~8-10 FPS).

Usage:
    python3 send_video.py <host> <video>  [--fps N]

Examples:
    python3 send_video.py 192.168.1.100 clip.mp4
    python3 send_video.py 192.168.1.100 clip.mp4 --fps 8

Requires:
    pip install opencv-python pydub numpy
    sudo apt install ffmpeg   # Debian/Ubuntu; brew install ffmpeg on macOS
"""

import sys
import socket
import threading
import argparse
import time
import numpy as np
import cv2
from pydub import AudioSegment

DISPLAY_W   = 240
DISPLAY_H   = 320
SAMPLE_RATE = 44100


def frame_to_rgb565(frame_bgr: np.ndarray) -> bytes:
    """Convert a BGR OpenCV frame to big-endian RGB565 bytes."""
    frame = cv2.resize(frame_bgr, (DISPLAY_W, DISPLAY_H))
    r = frame[:, :, 2].astype(np.uint16)
    g = frame[:, :, 1].astype(np.uint16)
    b = frame[:, :, 0].astype(np.uint16)
    rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return rgb565.byteswap().tobytes()   # ESP32 expects big-endian


def send_video(host: str, video_path: str, target_fps: int, ready: threading.Event) -> None:
    cap = cv2.VideoCapture(video_path)
    if not cap.isOpened():
        print(f"[video] ERROR: cannot open {video_path}")
        return

    src_fps      = cap.get(cv2.CAP_PROP_FPS) or 25.0
    total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    step         = max(1, round(src_fps / target_fps))
    n_frames     = total_frames // step

    print(f"[video] {video_path}: {src_fps:.1f} fps source, "
          f"sending every {step} frames -> ~{src_fps/step:.1f} fps")
    print(f"[video] {n_frames} frames × {DISPLAY_W}×{DISPLAY_H} × 2 B = "
          f"{n_frames * DISPLAY_W * DISPLAY_H * 2 / 1024:.0f} KB")

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((host, 8080))
        ready.set()   # signal audio thread to start

        frame_idx = 0
        sent      = 0
        while True:
            ok, frame = cap.read()
            if not ok:
                break
            if frame_idx % step == 0:
                s.sendall(frame_to_rgb565(frame))
                sent += 1
                if sent % 20 == 0:
                    print(f"[video] {sent}/{n_frames} frames sent")
            frame_idx += 1

    cap.release()
    print(f"[video] Done ({sent} frames).")


def send_audio(host: str, video_path: str, ready: threading.Event) -> None:
    print(f"[audio] Decoding audio from {video_path} ...")
    audio = AudioSegment.from_file(video_path)
    audio = audio.set_frame_rate(SAMPLE_RATE).set_channels(1).set_sample_width(2)
    raw   = audio.raw_data

    print(f"[audio] {len(audio) / 1000:.1f} s, {len(raw) / 1024:.0f} KB PCM")

    ready.wait()   # start at the same moment as the first video frame

    # Stream at real-time playback rate (SAMPLE_RATE * 2 bytes/s).
    # Sending the full payload at once floods the ESP32's stream buffer
    # (88 KB/s playback vs. 2-4 MB/s WiFi) causing an immediate reset.
    CHUNK_BYTES = 4410  # 50 ms per send
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
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("host")
    parser.add_argument("video")
    parser.add_argument("--fps", type=int, default=8,
                        help="Target frame rate (default 8). Keep ≤10 for stable WiFi.")
    args = parser.parse_args()

    ready = threading.Event()

    t_video = threading.Thread(target=send_video,
                               args=(args.host, args.video, args.fps, ready))
    t_audio = threading.Thread(target=send_audio,
                               args=(args.host, args.video, ready))

    t_audio.start()   # start decoding early (may take a moment for long files)
    t_video.start()

    t_video.join()
    t_audio.join()
    print("All done.")
