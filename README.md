# Supercon 2026 badge: native ESP-IDF firmware

Replacement firmware for the Hackaday Supercon 2026 "Billboard Badge" (ESP32-S3, 9.7" black/white/red e-paper),
written in plain C on ESP-IDF instead of the stock MicroPython + LVGL build.

- Drives the Pervasive Displays 9.7" Spectra panel (E2969JS0B1, 960x672, two CoGs) directly.
- No app framework, no filesystem. Pins and panel constants are hardcoded in one file, `main/main.c`.
- Wi-Fi SoftAP with a one-page web UI: pick an image, or upload your own. The browser converts and dithers it to
  black/white/red, so the badge only stores raw panel planes.
- Buttons: Menu+/Menu- step through images; A+B together starts Snake.
- Work in progress: Rickroll (15 s at ~1 fps) and Snake use a shortened, windowed refresh. Expect ghosting.

## Build and flash

You need ESP-IDF 5.x or 6.x, Python 3 with Pillow and numpy, and ffmpeg (only for the video frames).

The source images (`images/`) and the rickroll frames (`main/frames_1fps.bin`) are in the repo.
Before the first build, bake the images with your name on the nametag:

```sh
python3 tools/mkimages.py "First" "Last"        # images/*.png -> main/defaults.bin
```

To redo the video frames: `python3 tools/mkframes.py video.mp4 1 43 15 636:476:109:2` (fps, start s, duration s, precrop).

Build and flash:

```sh
idf.py build
idf.py -p /dev/cu.usbmodem101 flash monitor
```

Flashing erases the stock MicroPython firmware and its files. To keep them, back up first with
`esptool.py --chip esp32s3 read_flash 0 ALL backup.bin` (slow, about 8 MB over USB).

## Use

Join the Wi-Fi network `badge-XXXX` (password `supercon`) and open http://192.168.4.1/.

| Endpoint | |
|---|---|
| `GET /` | web UI |
| `GET /list` | `{"cur","ndef","nslot","valid"}` |
| `GET /img?i=N` | raw planes of image N |
| `POST /show?i=N` | show image N (a full refresh takes ~30 s) |
| `POST /upload?s=K` | 161280 raw bytes into upload slot K (0-15), then show it |
| `POST /rick`, `POST /snake` | start video playback / Snake |

## Image format

Black plane, then red plane. Each plane is 960x672 MONO_HLSB: 120 bytes per row, MSB = leftmost pixel, 1 = ink.
161280 bytes per image. The driver splits each row between the left and right CoG and bit-reverses the bytes.

## Hardware

| Signal | GPIO |
|---|---|
| EPD CS left / right | 13 / 3 |
| EPD BUSY left / right | 12 / 8 (low = busy) |
| EPD DC / RST | 10 / 11 |
| EPD SCK / MOSI | 21 / 47 |
| Button A / Menu- / Menu+ / B | 39 / 40 / 15 / 16 (active low; the silkscreen swaps Menu +/-) |

The panel init and refresh sequence follows Pervasive Displays'
[EPD_Driver_GU_large](https://github.com/PervasiveDisplays/EPD_Driver_GU_large). The badge differences (two BUSY lines,
bit reversal, split rows) are in [NOTES.md](NOTES.md) with the other reverse-engineering notes.

Unofficial. Not affiliated with Hackaday or Pervasive Displays.
