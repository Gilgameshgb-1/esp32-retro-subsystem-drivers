"""
mp3toc.py -- Convert an MP3 (or any audio file) to a 16-bit mono PCM C header.

The output is a .h file containing a raw int16_t array that can be passed
directly to audio_play_pcm(). No WAV header is stored -- just samples.

Usage:
    python mp3toc.py <input.mp3> <output.h> [sample_rate]

    sample_rate defaults to 44100.

Example:
    python mp3toc.py sound.mp3 main/sound.h
    python mp3toc.py sound.mp3 main/sound.h 22050

Requires pydub and ffmpeg:
    pip install pydub
    # ffmpeg must be on your PATH: https://ffmpeg.org/download.html
"""

import sys
import os
import struct
from pydub import AudioSegment


def convert(input_path: str, output_path: str, sample_rate: int) -> None:
    print(f"Loading {input_path} ...")
    audio = AudioSegment.from_file(input_path)

    # Normalize to mono, target sample rate, 16-bit
    audio = audio.set_channels(1)
    audio = audio.set_frame_rate(sample_rate)
    audio = audio.set_sample_width(2)  # 16-bit

    raw = audio.raw_data  # bytes, little-endian int16
    num_samples = len(raw) // 2
    duration_ms = (num_samples * 1000) // sample_rate

    var_name = os.path.splitext(os.path.basename(output_path))[0]
    var_name = "".join(c if c.isalnum() else "_" for c in var_name)

    print(f"Duration : {duration_ms} ms")
    print(f"Samples  : {num_samples} @ {sample_rate} Hz mono 16-bit")
    print(f"Size     : {len(raw)} bytes ({len(raw) // 1024} KB)")
    print(f"Writing  : {output_path} ...")

    with open(output_path, "w") as f:
        f.write("#pragma once\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"/* {os.path.basename(input_path)} -- {sample_rate} Hz, mono, 16-bit PCM */\n")
        f.write(f"#define {var_name.upper()}_SAMPLE_RATE  {sample_rate}\n")
        f.write(f"#define {var_name.upper()}_NUM_SAMPLES  {num_samples}\n\n")
        f.write(f"const int16_t {var_name}[{num_samples}] = {{\n")

        samples = struct.unpack(f"<{num_samples}h", raw)
        cols = 16
        for i, s in enumerate(samples):
            if i % cols == 0:
                f.write("    ")
            f.write(f"{s:6d},")
            if i % cols == cols - 1:
                f.write("\n")
            else:
                f.write(" ")

        if num_samples % cols != 0:
            f.write("\n")
        f.write("};\n")

    print("Done.")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    rate = int(sys.argv[3]) if len(sys.argv) > 3 else 44100
    convert(sys.argv[1], sys.argv[2], rate)
