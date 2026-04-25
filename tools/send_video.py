"""
send_video.py -- Stream video + audio to the ESP32 simultaneously.

Extracts frames from any video file, converts them to RGB565, and streams
them to port 8080.  Audio is decoded to raw 16-bit mono PCM and streamed
to port 8081.  Both start at the same time via threads.

Usage:
    python3 send_video.py <host> <video>  [--fps N] [--stats]

Examples:
    python3 send_video.py 192.168.1.100 clip.mp4
    python3 send_video.py 192.168.1.100 clip.mp4 --fps 8 --stats

Requires:
    pip install opencv-python pydub numpy
    sudo apt install ffmpeg   # Debian/Ubuntu; brew install ffmpeg on macOS
"""

import sys
import socket
import struct
import threading
import argparse
import time
import numpy as np
import cv2
from pydub import AudioSegment

DISPLAY_W   = 240
DISPLAY_H   = 320
SAMPLE_RATE = 44100
FULL_FRAME_BYTES = DISPLAY_W * DISPLAY_H * 2  # 153,600

# Tile size for dirty-region detection. 8×8 gives 30×40 = 1200 tiles.
TILE_W = 8
TILE_H = 8
TILES_X = DISPLAY_W // TILE_W   # 30
TILES_Y = DISPLAY_H // TILE_H   # 40

# Delta frame limits. Patches taller than MAX_PATCH_TILE_ROWS×TILE_H would
# exceed the ESP32's static patch buffer (240 × 40 × 2 = 19 200 B).
MAX_PATCH_TILE_ROWS = 5           # → 40 px max patch height
MAX_PATCHES         = 200         # uint8_t wire field; per-patch header is only 8 B
DELTA_THRESHOLD     = 0.75        # send delta only when it saves >25 % vs full


# ---------------------------------------------------------------------------
# Frame conversion
# ---------------------------------------------------------------------------

def frame_to_rgb565_array(frame_bgr: np.ndarray) -> np.ndarray:
    """Resize and convert a BGR OpenCV frame to a (H, W) uint16 RGB565 array."""
    frame = cv2.resize(frame_bgr, (DISPLAY_W, DISPLAY_H))
    r = frame[:, :, 2].astype(np.uint16)
    g = frame[:, :, 1].astype(np.uint16)
    b = frame[:, :, 0].astype(np.uint16)
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def rgb565_to_bytes(arr: np.ndarray) -> bytes:
    """Convert a (H, W) uint16 RGB565 array to big-endian bytes for the ESP32."""
    return arr.byteswap().tobytes()


# ---------------------------------------------------------------------------
# Dirty-tile detection
# ---------------------------------------------------------------------------

def compute_dirty_mask(curr: np.ndarray, prev: np.ndarray) -> np.ndarray:
    """Return a (TILES_Y, TILES_X) bool array: True where any pixel changed."""
    diff = curr != prev                                         # (H, W) bool
    tiled = diff.reshape(TILES_Y, TILE_H, TILES_X, TILE_W)
    return tiled.any(axis=(1, 3))                              # (TILES_Y, TILES_X)


def merge_dirty_tiles(dirty_mask: np.ndarray) -> list[tuple[int, int, int, int]]:
    """Merge dirty tile runs into a minimal list of pixel-coordinate rectangles.

    Scanline algorithm: walk left-to-right, top-to-bottom. Each time a dirty
    tile is found, extend right then down as far as the full column run is
    dirty. Consumed tiles are cleared so they are not visited again.

    Returns list of (x0, y0, x1, y1) in pixel coordinates (x1/y1 exclusive).
    """
    mask = dirty_mask.copy()
    rects = []

    for ty in range(TILES_Y):
        tx = 0
        while tx < TILES_X:
            if not mask[ty, tx]:
                tx += 1
                continue

            # Extend right
            tx_end = tx + 1
            while tx_end < TILES_X and mask[ty, tx_end]:
                tx_end += 1

            # Extend down while every tile in [tx, tx_end) is dirty,
            # capped so the patch height never exceeds the ESP32 patch buffer.
            ty_end = ty + 1
            while (ty_end < TILES_Y
                   and (ty_end - ty) < MAX_PATCH_TILE_ROWS
                   and mask[ty_end, tx:tx_end].all()):
                ty_end += 1

            mask[ty:ty_end, tx:tx_end] = False

            rects.append((tx  * TILE_W, ty  * TILE_H,
                          tx_end * TILE_W, ty_end * TILE_H))
            tx = tx_end

    return rects


def patch_bytes(rects: list) -> int:
    """Byte count of a delta frame: 2-byte frame header + per-patch header + pixels."""
    total = 2
    for x0, y0, x1, y1 in rects:
        total += 8 + (x1 - x0) * (y1 - y0) * 2
    return total


def build_delta_frame(rects: list, curr: np.ndarray) -> bytes:
    """Serialise a delta frame: [0xDF, n] then per-patch [x0 y0 x1 y1 pixels…]."""
    buf = bytearray([0xDF, len(rects)])
    for x0, y0, x1, y1 in rects:
        buf += struct.pack('<HHHH', x0, y0, x1, y1)
        buf += curr[y0:y1, x0:x1].byteswap().tobytes()
    return bytes(buf)


# ---------------------------------------------------------------------------
# Video sender
# ---------------------------------------------------------------------------

def send_video(host: str, video_path: str, target_fps: int,
               ready: threading.Event, show_stats: bool) -> None:
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

    prev_rgb565: np.ndarray | None = None

    total_sent_bytes = 0
    total_full_bytes = 0   # what we would have sent without delta encoding

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((host, 8080))
        ready.set()

        frame_idx = 0
        sent      = 0
        while True:
            ok, frame = cap.read()
            if not ok:
                break

            if frame_idx % step != 0:
                frame_idx += 1
                continue

            curr_rgb565 = frame_to_rgb565_array(frame)

            # --- Choose frame type ---
            if prev_rgb565 is None:
                # First frame: always full so ESP32 has a complete baseline.
                payload = bytes([0xFF, 0x00]) + rgb565_to_bytes(curr_rgb565)
                mode    = 'first'
                rects   = []
            else:
                dirty_mask = compute_dirty_mask(curr_rgb565, prev_rgb565)
                rects      = merge_dirty_tiles(dirty_mask)
                n          = len(rects)
                delta_b    = patch_bytes(rects)

                if n == 0:
                    payload = bytes([0xDF, 0x00])   # 2 bytes — skip frame
                    mode    = 'skip '
                elif n <= MAX_PATCHES and delta_b < FULL_FRAME_BYTES * DELTA_THRESHOLD:
                    payload = build_delta_frame(rects, curr_rgb565)
                    mode    = 'delta'
                else:
                    payload = bytes([0xFF, 0x00]) + rgb565_to_bytes(curr_rgb565)
                    mode    = 'full '

            total_sent_bytes += len(payload)
            total_full_bytes += FULL_FRAME_BYTES + 2

            if show_stats:
                dirty_px  = int((curr_rgb565 != prev_rgb565).sum()) if prev_rgb565 is not None else DISPLAY_W * DISPLAY_H
                pct_dirty = dirty_px / (DISPLAY_W * DISPLAY_H) * 100
                print(f"[stats] f{sent:04d} [{mode}]: {len(rects):2d} patches | "
                      f"{pct_dirty:5.1f}% dirty | {len(payload)/1024:6.1f} KB sent")
            elif sent % 20 == 0 and sent > 0:
                saving = (1 - total_sent_bytes / total_full_bytes) * 100
                print(f"[video] {sent}/{n_frames} frames | bandwidth saving: {saving:.0f}%")

            s.sendall(payload)
            prev_rgb565 = curr_rgb565
            sent += 1
            frame_idx += 1

    cap.release()
    saving = (1 - total_sent_bytes / total_full_bytes) * 100 if total_full_bytes else 0
    print(f"[video] Done ({sent} frames). Bandwidth saving vs full frames: {saving:.0f}%")


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
    parser.add_argument("--stats", action="store_true",
                        help="Print per-frame dirty-region stats.")
    args = parser.parse_args()

    ready = threading.Event()

    t_video = threading.Thread(target=send_video,
                               args=(args.host, args.video, args.fps, ready, args.stats))
    t_audio = threading.Thread(target=send_audio,
                               args=(args.host, args.video, ready))

    t_audio.start()   # start decoding early (may take a moment for long files)
    t_video.start()

    t_video.join()
    t_audio.join()
    print("All done.")
