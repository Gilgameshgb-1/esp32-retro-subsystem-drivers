<div align="center">

# ESP32 ILI9341 Display + Audio Driver

A from-scratch SPI display driver and I2S audio driver for the ESP32, written in C directly on top of ESP-IDF primitives - no graphics libraries, no audio middleware. Images and audio are streamed over Wi-Fi from any host device on the same network.

</div>

---

## What this is

This is a from-scratch driver stack built on ESP-IDF and FreeRTOS that drives two peripherals simultaneously:

- **ILI9341 2.8" TFT display** (240×320, RGB565) over SPI with DMA, strip-based rendering, double-buffered
- **MAX98357A I2S mono amplifier** - raw 16-bit PCM audio output at 44100 Hz

Both receive their data over Wi-Fi TCP. The ESP32 connects to your configured network on boot, then listens on two ports: `8080` for images and `8081` for audio. A companion Python tool on the host decodes and streams the content.

**Practical application:** the intended use is as a display/speaker peripheral for an embedded Linux distribution of mine. See: https://github.com/Gilgameshgb-1/yocto-retro-streaming-distro The host device connects over Wi-Fi and pushes images and audio to the ESP32 - no SD card, no USB, no HDMI needed.

---

## Demo
<div>
<video src="https://github.com/user-attachments/assets/64067787-8c87-41f2-bf71-4b4392129b3c" controls width="300"></video>
<video src="https://github.com/user-attachments/assets/56b53659-db5b-4552-b3b8-1fce25be62a6" controls width="300"></video>
</div>
<div>
<video src="https://github.com/user-attachments/assets/beba5ada-2e8c-40bd-83ea-9f654d9aab2f" controls width="300"></video>
<video src="https://github.com/user-attachments/assets/1512e12a-8e1c-4eb8-9877-2e11e7749c9d" controls width="300"></video>
</div>

---

## Wiring

### Display — ILI9341

| Display Pin | ESP32 GPIO |
|-------------|------------|
| MOSI        | 23         |
| CLK         | 18         |
| CS          | 5          |
| DC          | 2          |
| RST         | 4          |
| VCC         | 3.3V       |
| GND         | GND        |

### Audio — MAX98357A

| Amplifier Pin | ESP32 GPIO |
|---------------|------------|
| DIN           | 25         |
| BCLK          | 26         |
| LRC           | 27         |
| VIN           | 3.3V / 5V  |
| GND           | GND        |

---

## Architecture

```
Host device (PC / phone / SBC)
        │
        ├── TCP :8080 ──► on_image_data() ──► strip render ──► ILI9341 (SPI DMA)
        │                                          core 0
        │
        └── TCP :8081 ──► on_audio_data() ──► stream buffer ──► audio_write_task ──► MAX98357A (I2S)
                               core 1                                  core 1
```

- The image TCP task runs on core 0 alongside Wi-Fi / lwIP.
- The audio TCP task and I2S write task both run on core 1, keeping blocking I2S DMA writes away from the Wi-Fi interrupt core.
- TCP flow control naturally paces both senders when the ESP32 is busy the receive window closes and the host pauses automatically.

---

## Building and flashing

```bash
idf.py build
idf.py -p COM5 flash monitor
```

Wi-Fi credentials are set in `sdkconfig.defaults` or via `idf.py menuconfig` under `Component config → Wi-Fi credentials`.

---

## Host tools

All tools live in `tools/`. Install dependencies once:

```bash
pip install Pillow pydub
# also needs ffmpeg on PATH for audio decoding:
# Windows: winget install ffmpeg
# Linux: ?
```

### Send an image

Converts any image to RGB565 240×320 and streams it to the display.

```bash
python tools/send_image.py <host> <image>

# example
python tools/send_image.py 192.168.0.21 tools/images/alert.jpg
```

### Send audio

Decodes any audio format (MP3, WAV, FLAC, …) to raw 16-bit mono PCM and streams it to the amplifier.

```bash
python tools/send_audio.py <host> <file>

# example
python tools/send_audio.py 192.168.0.21 sound.mp3
```

### Send image + audio together

Sends both simultaneously using threads — image to port 8080, audio to port 8081.

```bash
python tools/send_av.py <host> <image> <audio>

# example
python tools/send_av.py 192.168.0.21 tools/images/alert.jpg sound.mp3
```

### Send a video

Extracts frames from a video file, converts each to RGB565, and streams frames + audio simultaneously. (This is highly unstable and I couldn't really get it to work without some extra memory somewhere)

```bash
python tools/send_video.py <host> <video>

# example
python tools/send_video.py 192.168.0.21 tools/videos/BadApple.mp4
```

---

## Component structure

```
components/
├── ili9341/        SPI display driver (init, fill, draw image, RGB565 helpers)
├── framebuffer/    Double-buffered strip framebuffer
├── audio/          I2S init + blocking PCM write (audio_init, audio_write)
└── wifi_server/    Wi-Fi STA connect + multi-port TCP server
tools/
├── send_image.py   Stream a still image
├── send_audio.py   Stream an audio file
├── send_av.py      Stream image + audio together
├── send_video.py   Stream a video file
└── img2c.py        Convert an image to a C header (RGB565 array)
```
