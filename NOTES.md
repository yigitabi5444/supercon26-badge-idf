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
| Button (stock code name) | Printed on the badge | GPIO | Use |
|---|---|---|---|
| SW4 BUTTON_1 "User A" | A | 39 | pair (press on both badges together) |
| SW5 BUTTON_2 "Menu -" | MENU + | 40 | Pong: paddle up |
| SW6 BUTTON_3 "Menu +" | MENU - | 15 | Pong: paddle down |
| SW7 BUTTON_4 "User B" | B | 16 | unused |
| Boot | BOOT | 0 | unused |

LED bar: 8x WS2812 on GPIO38 (`lib/led_bar.py`), GRB, driven by RMT (`led_init`/`leds` in main.c).

SPI mode 0, MSB first, 8 MHz (same as PDi). CS and DC are driven by hand, so one command can span several SPI transfers.

## Toolchain
ESP-IDF **v6.0.2** was already installed with EIM (`~/.espressif/v6.0.2/esp-idf`, activate with
`source ~/.espressif/tools/activate_idf_v6.0.2.sh`). I used it instead of installing a v5.x under `~/esp`.
The code uses only APIs that are the same in v5.x.

## Gotchas
- FreeRTOS default tick is 100 Hz. `pdMS_TO_TICKS(2)` is then 0 and the 2 ms DC-DC soft start steps would not wait.
  `sdkconfig.defaults` sets 1000 Hz and `ms()` adds one tick.
- MicroPython: a soft reset breaks LVGL (MemoryError in display_create). Use `mpremote reset`.
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

## Firmware (this directory, branch `pong`)
Pong only: no SoftAP, no web UI, no images, no Snake/rickroll (all on `master`).
- `main/main.c`: panel driver, Pong, LED bar, ESP-NOW radio, buttons. One file, pins and panel constants hardcoded.
- `main/link.h`: the pairing/heartbeat state machine, plain C so it also builds on a PC.
- `tools/test_link.c`: simulation of 4 badges on one channel with 20% packet loss:
  `cc -I main tools/test_link.c -o /tmp/test_link && /tmp/test_link`.

### Plane format
960x672, MONO_HLSB (row-major, 120 bytes per row, MSB = leftmost pixel), 1 = ink, 80640 bytes. The driver splits
each row into the left CoG half (bytes 0-59) and right half (60-119) and bit-reverses every byte on the way out
(panel `"B1"`, `REVERSE_BITS` in main.c).

### Flash layout (8 MB)
| Partition | Offset | Size | |
|---|---|---|---|
| nvs | 0x9000 | 0x6000 | Wi-Fi calibration |
| factory | 0x10000 | 0x400000 | app (~0.75 MB) |

## Pong over ESP-NOW
Wi-Fi in STA mode, not connected, power save off, fixed on channel 1. Every packet is one 24-byte `pkt_t`
(`main/link.h`, packed, little-endian):

| Bytes | Field | |
|---|---|---|
| 0-3 | magic | `"PONG"` |
| 4 | type | 1 = PAIR (broadcast), 2 = GAME (unicast) |
| 5-6 | age | PAIR: ms since the sender pressed A |
| 7-12 | peer | PAIR: the sender's choice (zero while undecided); GAME: the sender's peer |
| 13 | seen | guest: last game tick it has shown |
| 14-23 | game | tick, ball x/y/dx/dy, paddle row [host, guest], score [host, guest] |

States: FREE, SEEK, PAIRED. Times from `link.h`:
- **FREE.** Nothing sent. PAIR packets heard are remembered with the sender's press time (`now - age`), so a
  partner who pressed first still counts. LEDs off (red for 0.5 s after a failed pairing or a lost peer).
- **A pressed -> SEEK** (ignored while PAIRED). Broadcast PAIR every 50 ms. LEDs blue.
  600 ms after the press (500 ms window + air time) the choice is made: the badges heard whose press was within
  500 ms of mine. Exactly one -> that is my choice, it goes into `peer`. None or two+ -> FREE, press again.
  So two pairs pressing at the same moment do not cross-bind: everybody sees two or three candidates and gives
  up. Pairs that press a second apart do not see each other.
- **SEEK -> PAIRED** when a packet from my choice names me as its choice/peer (mutual). No answer 1.5 s after the
  press -> FREE.
- **PAIRED.** Unicast GAME every 100 ms; it is the heartbeat. A press does nothing. No packet from the peer for
  1 s -> FREE (the panel goes back to the empty court). So rebooting one badge is the only way to unpair.

Game: 10 x 7 grid of 96 px cells, paddles 2 cells, first to 4. The lower MAC is the host (left paddle) and the
only one that steps the game; the guest copies the host's state and sends its own paddle row. Each badge draws its
own paddle on the left (the guest mirrors the court). Lockstep: the host steps only when the guest reports
(`seen`) that it has shown the last step, so each step is on both screens for a whole frame, about 1.2-1.3 s per
step (a fresh panel session every 20 frames adds ~3 s). The screen shows the ball solid and its next cell as an
outline, so you move the paddle onto the outline. LEDs: my points green from the left end, the peer's red from
the right end, the ball's column white; all green/red for one step when someone reaches 4. If LED 0 turns out to
be on the right, set `LED_FLIP` in main.c.

### Build and flash
```
cd ~/Documents/hackaday26/pong
source ~/.espressif/tools/activate_idf_v6.0.2.sh
idf.py build
idf.py -p /dev/cu.usbmodem101 flash monitor
```
- `oldfs_0x1FC000-0x800000.bin` is the only copy left of the old MicroPython filesystem (LittleFS starts at
  0x310000, i.e. offset 0x114000 in the file). `badge_fs/lib/button.py` and `led_bar.py` were cut out of it.

## Fast refresh (short waveform at 0xB0)
Source: the badge team's `documentation/itc97-driver-reference.pdf` in
https://github.com/Hack-a-Day/2026-Billboard_Badge (driver for both panel variants, ported from the OpenEPaperLink
`pervasive_itc_medium` driver) and the tables in the stock filesystem's `data/rendered_luts.py`, which the stock
MicroPython never uses.
- This badge has the **B1** CoG (E2969JS0B1, "driver B"): the stock waveform runs 32.96 s here; the doc gives
  ~34 s for B1 and ~51 s for the 084 variant. B1 takes the 400-byte table `lutb_base` (5 tables x 10 groups x 8 B).
- `0x70 71 36` (booster setting) right after reset, to both CoGs. Without it a B1 refresh draws about 2 A
  (doc errata 9). The stock MicroPython and PDi drivers never send it; added for every session here.
- `0xB0` has one window: after the last `0xA7 00` load strobe has settled (500 ms) and before the DC-DC soft start.
  Earlier, the OTP waveform overwrites it; inside an open session, rewriting it falls back to the slow waveform.
- One `0x15 3C` ("pull") runs a fixed ~1070 ms on B1 whatever the table holds. Three pulls give a clean image
  (doc: ink L* 15.1, paper L* 86.9 in 3460 ms). Within an open session a frame is just plane data + pulls:
  no reset, no register sequence, no soft start. The badge team reopens the session every 20 frames.
- The short waveform is black/white only. Red goes pale and red already on the film survives it; only the slow
  waveform clears it. So the rickroll/Snake start with leftover red from a nametag until the next slow refresh.
- `0x45` after `0x44 06` is the temperature, `0x50 + 2 * degrees C` (PDi docs): 0x82 = 25 C. The sweep above
  (0x90-0xC0) just told the CoG it was 32-56 C, which barely shortens the stock waveform.
- The doc's VCC_EN (panel supply switch) is not driven here: GPIO48 (`EPD_PWR_EN` in the stock `epd.py`) is
  left alone, as in the stock firmware. The doc warns that a wrong supply polarity or driving the bus while the
  panel is unpowered can break the panel.

### Measured on this badge (current driver, 8 MHz SPI)
| What | Time |
|---|---|
| Full 3-colour refresh, stock OTP waveform | 35.3 s total, BUSY low 32 956 ms |
| Short-waveform session open (reset, data, registers, 0xB0, DC-DC) + 3 pulls | 5 920 ms |
| One pull | BUSY low 945-946 ms |
| Rickroll frame (80 KB black plane + 1 pull) | 1 065-1 066 ms -> **0.94 fps**, 15 frames in 21.0 s |
| Snake move (plane + 1 pull) | 1 062-1 063 ms |

The pull is fixed by the CoG, so ~1.06 fps is the ceiling on this waveform; the remaining ~120 ms per frame is
the SPI transfer (80 KB at 8 MHz) and bit reversal. Every short refresh shakes the whole panel, so the screen
flashes on each frame/move. Partial / no-flash updates are being worked on separately (`panel_lab`).
