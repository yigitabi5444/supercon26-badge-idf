# Billboard badge: ESP-IDF port notes

## Panel
- Pervasive Displays 9.7" (9.69") Spectra black/white/red, 960x672 (PDi says 672x960, portrait), two CoGs.
- `epd.py` hardcodes `pdi_cp = 0x9608`, `pdi_size = 0x96`. In PDi `EPD_Configuration.h` that is
  `eScreen_EPD_969 = 0x9608 // reference xE2969CS08x, previous type`.
- The 9.7" BWR part with the newer "0B" CoG is **E2969JS0B1** (DigiKey: "Black, Red, White, 9.7", 672 x 960").
  `epd.py` reads `display_pn` from `/data/hwconf` and defaults to `"B1"`; only `"84"` turns the bit reversal off.
  So `"B1"` = E2969JS0B1 and `"84"` = probably the older E2969?S084 ("08" CoG type). This badge: see below.
- Left CoG = PDi "master" (CS1), right CoG = "slave" (CS2). Each takes 60 bytes x 672 rows per colour.

## Does the PDi driver match?
Yes. `epd.py` is a line-by-line port of
`EPD_Driver_GU_large@e408154/src/EPD_Driver.cpp` (the docstring says so too). Same reset timing (200/20/200/200/5 ms),
same DUW/DRFW/RAM_RW values, same COG_initial, DC-DC soft start/shutdown and refresh (0x15 0x3C).
Badge-specific differences:
- Two BUSY lines (one per CoG). The PDi code reads only one. We wait for both.
- Bytes are bit-reversed before sending (panel `"B1"`). PDi's `reverse()` exists but is not used in the PDi code.
- The 960-wide framebuffer is split into left/right 480-column halves, row by row.
- The 0x09 0x7F 0x7D 0x00 `COG_powerOff()` is never called; the DC-DC soft shutdown ends the update.

## Pin map (ESP32-S3)
| Signal | GPIO | Note |
|---|---|---|
| CS1 (left CoG, master) | 13 | |
| CS2 (right CoG, slave) | 3 | strapping pin, fine as output after boot |
| BUSY1 (left) | 12 | low = busy |
| BUSY2 (right) | 8 | low = busy |
| DC | 10 | low = command index, high = data |
| RST | 11 | |
| SCK | 21 | |
| MOSI | 47 | |
| MISO | 45 | not connected; strapping pin (VDD_SPI), leave alone |
| PWR_EN | 48 | defined in `epd.py` but never used |

Buttons (from the old `lib/button.py`, active low, internal pull-ups; the silkscreen swaps Menu +/-):
| Button | GPIO | Normal mode | Snake |
|---|---|---|---|
| SW4 BUTTON_1 "User A" | 39 | (A+B together: start Snake) | left |
| SW5 BUTTON_2 "Menu -" | 40 | previous image | up |
| SW6 BUTTON_3 "Menu +" | 15 | next image | down |
| SW7 BUTTON_4 "User B" | 16 | (A+B together: start Snake) | right |
| Boot | 0 | unused | unused |

LED bar: 8x WS2812 on GPIO38 (`lib/led_bar.py`), unused here.

SPI mode 0, MSB first, 8 MHz (same as PDi). CS and DC are driven by hand, so one command can span several SPI transfers.

## Toolchain
ESP-IDF **v6.0.2** was already installed with EIM (`~/.espressif/v6.0.2/esp-idf`, activate with
`source ~/.espressif/tools/activate_idf_v6.0.2.sh`). I used it instead of installing a v5.x under `~/esp`.
The code uses only APIs that are the same in v5.x.

## Gotchas
- FreeRTOS default tick is 100 Hz. `pdMS_TO_TICKS(2)` is then 0 and the 2 ms DC-DC soft start steps would not wait.
  `sdkconfig.defaults` sets 1000 Hz and `ms()` adds one tick.
- MicroPython: a soft reset breaks LVGL (MemoryError in display_create). Use `mpremote reset`.
- Gallery PNGs are 960x670 (2 rows short); `mkimages.py` centres them.
- One S3 SPI transaction carries at most 32 KB (2^18 bits). The first build sent a 40320-byte half-plane in one
  transaction, `ESP_ERROR_CHECK` aborted and the badge boot-looped every second (the SoftAP showed up but nobody
  could join). `tx()` now sends 4 KB pieces.
- Opening the USB-Serial-JTAG port from macOS raises DTR+RTS; pyserial then drops DTR first, which is the
  download-mode reset combo. To read the log without resetting: open with `dtr=True, rts=False`, then `dtr=False`.
- No full-flash backup exists: the user stopped `read_flash 0 ALL` ("too slow") and said to flash without it.
  The old MicroPython LittleFS (at 0x310000, the stock build had a bigger app partition) was still intact after
  the first flash; `lib/button.py`, `lib/led_bar.py` and `epd.py` were recovered from a 6 MB dump of
  0x1FC000-0x800000. The current partition table overwrites that region.
- IDF v6 uses picolibc by default: logs use `%d` with casts, not `%lld`.

## Firmware (this directory)
- `main/main.c`: panel driver, image store, SoftAP + web server. One file, pins and panel constants hardcoded.
- `main/index.html`: the web UI, embedded in the binary. Converts in the browser (canvas, nearest colour or
  Floyd-Steinberg to black/white/red) and uploads raw planes only.
- `tools/mkimages.py`: host script, PNG -> `main/defaults.bin` (the baked-in images). Name lines are optional
  and go on the nametag: `python3 tools/mkimages.py "First" "Last"`. Needs Pillow (system python3 has it).
- `tools/check_pack.js`: `node tools/check_pack.js`, checks the browser packer against the plane format.

### Image format
Black plane then red plane, 960x672 each, MONO_HLSB (row-major, 120 bytes per row, MSB = leftmost pixel),
1 = ink. 80640 bytes per plane, 161280 bytes per image. White = neither bit set.
The driver splits each row into the left CoG half (bytes 0-59) and right half (60-119) and bit-reverses
every byte on the way out (panel `"B1"`, `REVERSE_BITS` in main.c).

### Flash layout (8 MB)
| Partition | Offset | Size | |
|---|---|---|---|
| nvs | 0x9000 | 0x6000 | Wi-Fi calibration, `badge/cur` (image on screen), `badge/valid` (slot bitmask) |
| factory | 0x10000 | 0x300000 | app + 7 baked-in images (1.1 MB) |
| images | 0x310000 | 0x280000 | 16 upload slots x 0x28000 |

Image index: 0-6 = built-in (0 = nametag), 7-22 = slots 1-16. A slot only counts after its upload finished
(bit set in NVS after the last byte is written), so leftover MicroPython filesystem data there is ignored.
At boot the firmware redraws the image from NVS `cur` (first boot: the nametag).

### Web API (http://192.168.4.1/)
- `GET /` the page; `GET /list` `{"cur","ndef","nslot","valid"}`; `GET /img?i=N` raw planes of image N
- `POST /show?i=N` queue a refresh (returns at once; the panel takes ~30 s)
- `POST /upload?s=K` body = 161280 raw bytes -> slot K (0-15), then shows it

### Build and flash
```
cd ~/Documents/hackaday26/idf_port
source ~/.espressif/tools/activate_idf_v6.0.2.sh
idf.py build
idf.py -p /dev/cu.usbmodem101 flash monitor
```
- `oldfs_0x1FC000-0x800000.bin` is the only copy left of the old MicroPython filesystem (LittleFS starts at
  0x310000, i.e. offset 0x114000 in the file). `badge_fs/lib/button.py` and `led_bar.py` were cut out of it.
