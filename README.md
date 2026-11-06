# Supercon 2026 badge: two-badge Pong over ESP-NOW

Pong for two Hackaday Supercon 2026 "Billboard Badges" (ESP32-S3, 9.7" black/white/red e-paper), peer to peer
over ESP-NOW, in plain C on ESP-IDF. This branch (`pong`) is Pong only; the image viewer, web UI, Snake and the
rickroll are on `master`.

## Play
1. Both badges show an empty court. Press **A** on both badges at the same time (within 0.5 s).
   The LED bar goes blue while it looks for the other badge, and red for a moment if pairing failed (nobody else
   pressed, or more than one other badge pressed at the same moment). Press again.
2. Once paired, each badge shows its own paddle on the left. **MENU +** moves it up, **MENU -** down, one cell per
   press. The solid square is the ball; the outline is where it will be next step, so put your paddle on it.
3. The screen updates about once a second and flashes each time (fast e-paper waveform). The LED bar reacts at
   once: your points green from the left, the other badge's red from the right, the ball's column white.
   First to 4 wins; the bar goes all green or all red, then a new game starts.
4. The pairing stays until a badge reboots. The other badge goes back to the empty court 1 s later.

## Build and flash
ESP-IDF 5.x or 6.x (tested with v6.0.2):

```sh
idf.py build
idf.py -p /dev/cu.usbmodem101 flash monitor
```

Flashing erases the stock MicroPython firmware and its files. To keep them, back up first with
`esptool.py --chip esp32s3 read_flash 0 ALL backup.bin` (slow, about 8 MB over USB).

Check the pairing logic on a PC: `cc -I main tools/test_link.c -o /tmp/test_link && /tmp/test_link`.

## Hardware

| Signal | GPIO |
|---|---|
| EPD CS left / right | 13 / 3 |
| EPD BUSY left / right | 12 / 8 (low = busy) |
| EPD DC / RST | 10 / 11 |
| EPD SCK / MOSI | 21 / 47 |
| Button A / MENU + / MENU - | 39 / 40 / 15 (active low, named as printed) |
| LED bar (8x WS2812) | 38 |

The panel init and refresh sequence follows Pervasive Displays'
[EPD_Driver_GU_large](https://github.com/PervasiveDisplays/EPD_Driver_GU_large) plus the badge team's short
waveform. Driver details, timings and the ESP-NOW protocol are in [NOTES.md](NOTES.md).

Unofficial. Not affiliated with Hackaday or Pervasive Displays.
