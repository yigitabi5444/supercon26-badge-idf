// Billboard badge (Hackaday Supercon 2026): two-badge Pong over ESP-NOW on the 9.7" e-paper.
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_event.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "badge";

// ---- Panel: Pervasive Displays 9.7" black/white/red, 960x672, two CoGs.
// The left CoG (master) drives columns 0-479, the right CoG (slave) columns 480-959.
#define W 960
#define H 672
#define PLANE (W * H / 8) // one colour, MONO_HLSB (MSB = leftmost pixel), 1 = ink
#define IMG (2 * PLANE)   // an image is the black plane, then the red plane
#define HALF (PLANE / 2)  // what one CoG gets of one plane
#define REVERSE_BITS 1    // stock driver: reverse unless /data/hwconf display_pn == "84"

enum { CS_L = 13, CS_R = 3, BUSY_L = 12, BUSY_R = 8, DC = 10, RST = 11, SCK = 21, MOSI = 47 };
enum { LEFT = 1, RIGHT = 2, BOTH = 3 };

static spi_device_handle_t spi;

static void ms(int t) { vTaskDelay(pdMS_TO_TICKS(t) + 1); } // at least t ms (1 kHz tick)

// One S3 SPI transaction carries at most 32 KB, so long data goes out in 4 KB pieces (CS stays low).
static void tx(const uint8_t *d, size_t n)
{
    for (size_t k; n; d += k, n -= k) {
        k = MIN(n, 4096);
        spi_transaction_t t = {.length = 8 * k, .tx_buffer = d};
        ESP_ERROR_CHECK(spi_device_polling_transmit(spi, &t));
    }
}

// Index byte with DC low, then data with DC high, to the CoGs in `cog`.
static void cmd(int cog, uint8_t index, const uint8_t *data, size_t n)
{
    gpio_set_level(DC, 0);
    if (cog & LEFT) gpio_set_level(CS_L, 0);
    if (cog & RIGHT) gpio_set_level(CS_R, 0);
    tx(&index, 1);
    gpio_set_level(CS_L, 1);
    gpio_set_level(CS_R, 1);
    gpio_set_level(DC, 1);
    if (cog & LEFT) gpio_set_level(CS_L, 0);
    if (cog & RIGHT) gpio_set_level(CS_R, 0);
    tx(data, n);
    gpio_set_level(CS_L, 1);
    gpio_set_level(CS_R, 1);
}
#define CMD(cog, index, ...) \
    cmd(cog, index, (const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}))

// Each CoG pulls its BUSY low while it works. Logs how long each side stayed busy (-1: timeout).
// Returns false on timeout.
static bool wait_ready(const char *what, int timeout_ms)
{
    int64_t t0 = esp_timer_get_time(), t = 0;
    int l = -1, r = -1;
    while ((l < 0 || r < 0) && t < timeout_ms * 1000LL) {
        t = esp_timer_get_time() - t0;
        if (l < 0 && gpio_get_level(BUSY_L)) l = t / 1000;
        if (r < 0 && gpio_get_level(BUSY_R)) r = t / 1000;
        if (l < 0 || r < 0) ms(10);
    }
    ESP_LOGI(TAG, "%s: BUSY left %d ms, right %d ms", what, l, r);
    return l >= 0 && r >= 0;
}

static uint8_t rev(uint8_t b)
{
    b = b >> 4 | b << 4;
    b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
    return (b & 0xAA) >> 1 | (b & 0x55) << 1;
}

// A CoG takes its 60 bytes of every row of one plane in one burst. plane NULL = all white.
static void send_half(int cog, uint8_t index, const uint8_t *plane)
{
    static DMA_ATTR uint8_t buf[HALF];
    const uint8_t *src = plane ? plane + (cog == RIGHT ? W / 16 : 0) : NULL;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W / 16; x++) {
            uint8_t b = src ? src[y * (W / 8) + x] : 0;
            buf[y * (W / 16) + x] = REVERSE_BITS ? rev(b) : b;
        }
    CMD(cog, 0x12, 0x3B, 0x00, 0x14); // RAM_RW
    cmd(cog, index, buf, HALF);
}

// A panel session: epd_open (reset, frame data, register sequence, DC-DC up), any number of
// epd_pull, epd_close. Same sequence as PDi EPD_Driver_GU_large and the stock epd.py, plus what the
// badge team documented for this B1 CoG (2026-Billboard_Badge/documentation/itc97-driver-reference.pdf):
// the 0x70 booster setting and the 0xB0 short waveform.

// Short waveform for the B1 CoG: data/rendered_luts.py "lutb_base" from the badge team.
// 5 tables (paper, ink, red, unused, VCOM) x 10 groups of {level, on, off, ph2, on, off, ph2, repeat};
// a level byte is 2 bits per phase: 0 GND, 1 VDH, 2 VDL, 3 VDHR. A pull runs a fixed ~1070 ms
// whatever the table asks for, so one pull is a light dose and three make a clean image.
static const uint8_t lutb[400] = {
    // table 0: paper
    0x99, 0x0A, 0x0A, 0x01, 0x0A, 0x0A, 0x01, 0x03,
    0xAA, 0x2D, 0x03, 0x01, 0x2D, 0x03, 0x01, 0x03,
    0xAA, 0x2D, 0x03, 0x01, 0x2D, 0x03, 0x01, 0x03,
    0xAA, 0x2D, 0x03, 0x01, 0x2D, 0x03, 0x01, 0x03,
    0xAA, 0x2D, 0x03, 0x01, 0x2D, 0x03, 0x01, 0x03,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    // table 1: ink
    0x66, 0x0A, 0x0A, 0x01, 0x0A, 0x0A, 0x01, 0x03,
    0x44, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0x44, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0x44, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0x44, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    // table 2: red
    0xEE, 0x0A, 0x0A, 0x01, 0x0A, 0x0A, 0x01, 0x03,
    0xAA, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0xFF, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0xFF, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0xFF, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0xFF, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0xFF, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0xFF, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0xFF, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0xFF, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    // table 3: unused
    0x00, 0x0A, 0x0A, 0x01, 0x0A, 0x0A, 0x01, 0x03,
    0x00, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0x00, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0x00, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0x00, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    // table 4: VCOM
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
};

static void epd_reset(void)
{
    ms(200);
    gpio_set_level(RST, 1);
    ms(20);
    gpio_set_level(RST, 0);
    ms(200);
    gpio_set_level(RST, 1);
    ms(200);
    gpio_set_level(CS_L, 1);
    gpio_set_level(CS_R, 1);
    ms(5);
    CMD(BOTH, 0x70, 0x71, 0x36); // B1 booster setting. Without it a refresh draws ~2 A (badge team errata 9)
}

// Frame into CoG RAM. red NULL = no red; with_red false leaves the red RAM as it is (saves 80 ms).
static void epd_load(const uint8_t *black, const uint8_t *red, bool with_red)
{
    CMD(BOTH, 0x13, 0x00, 0x3B, 0x00, 0x00, 0x9F, 0x02); // DUW: 60 bytes x 672 lines
    CMD(BOTH, 0x90, 0x00, 0x3B, 0x00, 0xA9);             // DRFW: whole panel
    for (int cog = LEFT; cog <= RIGHT; cog++) {
        send_half(cog, 0x10, black);
        if (with_red) send_half(cog, 0x11, red);
    }
}

static void dcdc_on(void)
{
    CMD(BOTH, 0x51, 0x50, 0x01); // DC-DC soft start
    for (int v = 1; v <= 4; v++) {
        CMD(BOTH, 0x09, 0x1F);
        CMD(BOTH, 0x51, 0x50, v);
        CMD(BOTH, 0x09, 0x9F);
        ms(2);
    }
    for (int v = 1; v <= 10; v++) {
        CMD(BOTH, 0x09, 0x1F);
        CMD(BOTH, 0x51, 0x0A, v);
        CMD(BOTH, 0x09, 0x9F);
        ms(2);
    }
    for (int v = 3; v <= 10; v++) {
        CMD(BOTH, 0x09, 0x7F);
        CMD(BOTH, 0x51, 0x0A, v);
        CMD(BOTH, 0x09, 0xFF);
        ms(2);
    }
    for (int v = 9; v >= 2; v--) {
        CMD(BOTH, 0x09, 0x7F);
        CMD(BOTH, 0x51, v, 0x0A);
        CMD(BOTH, 0x09, 0xFF);
        ms(2);
    }
    CMD(BOTH, 0x09, 0xFF);
    ms(100);
    wait_ready("dc-dc on", 10000); // never trigger into a booster that is still ramping
}

// fast: load the short waveform instead of the panel's own 33 s one (black/white only, red goes pale).
static void epd_open(const uint8_t *black, const uint8_t *red, bool fast)
{
    epd_reset();
    epd_load(black, red, true);
    CMD(BOTH, 0x05, 0x7D);
    ms(200);
    CMD(BOTH, 0x05, 0x00);
    ms(10);
    CMD(BOTH, 0xD8, 0x80); // MS_SYNC
    CMD(BOTH, 0xD6, 0x00); // BVSS
    CMD(BOTH, 0xA7, 0x10);
    ms(100);
    CMD(BOTH, 0xA7, 0x00);
    ms(100);
    CMD(BOTH, 0x03, 0x00, 0x11); // OSC
    for (int cog = LEFT; cog <= RIGHT; cog++) {
        CMD(cog, 0x44, 0x00);
        CMD(cog, 0x45, 0x80);
        CMD(cog, 0xA7, 0x10);
        ms(100);
        CMD(cog, 0xA7, 0x00);
        ms(100);
        CMD(cog, 0x44, 0x06);
        CMD(cog, 0x45, fast ? 0x84 : 0x82); // temperature 0x50 + 2 * C: 26 C as the badge team uses, 25 C stock
        CMD(cog, 0xA7, 0x10);
        ms(100);
        CMD(cog, 0xA7, 0x00);
        ms(fast ? 500 : 100); // this strobe commits the OTP waveform; 0xB0 must come after it settles
    }
    CMD(BOTH, 0x60, 0x25); // TCON
    CMD(LEFT, 0x61, 0x01); // STV_DIR, master only
    CMD(BOTH, 0x02, 0x00); // VCOM
    if (fast) cmd(BOTH, 0xB0, lutb, sizeof lutb); // one chance per session: rewriting it re-latches the slow one
    dcdc_on();
}

// Run the latched waveform n times on what is in RAM. Returns ms spent.
static int epd_pull(int n, int timeout_ms)
{
    int64_t t0 = esp_timer_get_time();
    while (n--) {
        CMD(BOTH, 0x15, 0x3C);
        ms(5);
        wait_ready("pull", timeout_ms);
    }
    return (esp_timer_get_time() - t0) / 1000;
}

static void epd_close(void)
{
    CMD(BOTH, 0x09, 0x7F); // DC-DC soft shutdown
    CMD(BOTH, 0x05, 0x7D);
    CMD(BOTH, 0x09, 0x00);
    ms(200);
    wait_ready("dc-dc off", 2000);
    gpio_set_level(DC, 0);
    gpio_set_level(CS_L, 0);
    gpio_set_level(RST, 0);
    gpio_set_level(CS_L, 1);
}

static void epd_init(void)
{
    gpio_set_level(DC, 1);
    gpio_set_level(RST, 1);
    gpio_set_level(CS_L, 1);
    gpio_set_level(CS_R, 1);
    gpio_config(&(gpio_config_t){.pin_bit_mask = BIT64(DC) | BIT64(RST) | BIT64(CS_L) | BIT64(CS_R),
                                 .mode = GPIO_MODE_OUTPUT});
    gpio_config(&(gpio_config_t){.pin_bit_mask = BIT64(BUSY_L) | BIT64(BUSY_R), .mode = GPIO_MODE_INPUT});
    spi_bus_config_t bus = {.mosi_io_num = MOSI, .sclk_io_num = SCK, .miso_io_num = -1,
                            .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4096};
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t dev = {.clock_speed_hz = 8000000, .mode = 0, .spics_io_num = -1, .queue_size = 1};
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &spi));
}

// ---- Pong between two badges. The game runs on a 10 x 7 grid of 96 px cells, one step per panel frame.
// The badge with the lower MAC is the host: it owns the game and steps it; the guest shows the host's state and
// sends its paddle. Both screens show their own player on the left. Pairing and heartbeat: link.h, NOTES.md.
#include "link.h"
#define CELL 96
#define PW (W / CELL) // 10 x 7
#define PH (H / CELL)
#define PAD 2         // paddle height in cells
#define WIN 4         // first to 4: the LED bar has 4 LEDs per player
#define CHANNEL 1
#define LED_GPIO 38
#define LED_FLIP 0    // 1 if LED 0 turns out to be on the right as you look at the badge
#define LED_ON 12     // WS2812 brightness, 0-255

static uint8_t fb[PLANE]; // black plane
static TaskHandle_t epd_task;
static link_t lk;
static game_t g;     // host: the game; guest: the host's latest, but g.py[1] is its own paddle
static uint8_t seen; // last tick the guest has shown (host: as the guest reported it)
static bool host;
static volatile bool in_pong, a_pressed;
static portMUX_TYPE gmux = portMUX_INITIALIZER_UNLOCKED;

static void box(int x0, int y0, int w, int h)
{
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) fb[y * (W / 8) + x / 8] |= 0x80 >> (x & 7);
}

// mirror: the guest flips the court so its own paddle (player 1) is on the left.
static void draw(const game_t *s, bool mirror)
{
    memset(fb, 0, sizeof fb);
    for (int y = 0; y < H; y += 48) box(W / 2 - 4, y + 12, 8, 24); // centre line
    for (int p = 0; p < 2; p++) {
        bool left = (p == 0) != mirror;
        box((left ? 0 : PW - 1) * CELL + 32, s->py[p] * CELL + 8, 32, PAD * CELL - 16);
        for (int k = 0; k < s->score[p]; k++) box(left ? W / 2 - 60 - 48 * k : W / 2 + 36 + 48 * k, 12, 24, 24);
    }
    if (!s->tick) return; // not served yet
    box((mirror ? PW - 1 - s->bx : s->bx) * CELL + 12, s->by * CELL + 12, CELL - 24, CELL - 24);
    if (s->bx == 0 || s->bx == PW - 1) return; // a point: no next position
    int nx = s->bx + s->dx, ny = s->by + s->dy;
    if (ny < 0 || ny >= PH) ny = s->by - s->dy;
    int x = (mirror ? PW - 1 - nx : nx) * CELL + 18, y = ny * CELL + 18, n = CELL - 36; // where the ball goes next:
    box(x, y, n, 6), box(x, y + n - 6, n, 6), box(x, y, 6, n), box(x + n - 6, y, 6, n); // an outline
}

static void serve(game_t *g, int dir)
{
    g->bx = dir > 0 ? PW / 2 - 1 : PW / 2;
    g->by = esp_random() % PH;
    g->dx = dir;
    g->dy = esp_random() & 1 ? 1 : -1;
}

// Host only: one game step.
static void pong_tick(game_t *g)
{
    if (!++g->tick) g->tick = 1; // 0 means "not started"
    if (g->bx == 0 || g->bx == PW - 1) { // last step was a point: serve towards whoever lost it
        if (g->score[0] == WIN || g->score[1] == WIN) g->score[0] = g->score[1] = 0;
        serve(g, g->bx ? 1 : -1);
        return;
    }
    int y = g->by + g->dy;
    if (y < 0 || y >= PH) g->dy = -g->dy, y = g->by + g->dy; // wall
    int x = g->bx + g->dx;
    if (x == 0 || x == PW - 1) {
        int p = x != 0;
        if (y >= g->py[p] && y < g->py[p] + PAD) g->dx = -g->dx, x = g->bx + g->dx; // paddle: back again
        else g->score[!p]++; // missed: the ball goes into the gutter
    }
    g->bx = x;
    g->by = y;
}

// Lockstep at the panel's ~1 frame/s: the host steps only after the guest has shown the last step, so both
// players see every step for a whole frame. A frame is plane data + 1 pull, a fresh session every 20 frames.
static void pong(void)
{
    ESP_LOGI(TAG, "pong: %s", host ? "host, left paddle" : "guest, right paddle");
    in_pong = true;
    int frames = 0;
    for (uint8_t drawn = 0; lk.state == L_PAIRED;) {
        taskENTER_CRITICAL(&gmux);
        bool go = host ? seen == g.tick : g.tick != drawn;
        if (go && host) pong_tick(&g);
        game_t s = g;
        taskEXIT_CRITICAL(&gmux);
        if (!go) {
            ms(20);
            continue;
        }
        draw(&s, !host);
        if (frames % 20 == 0) {
            if (frames) epd_close();
            epd_open(fb, NULL, true);
            epd_pull(frames ? 1 : 3, 5000);
        } else {
            epd_load(fb, NULL, false);
            epd_pull(1, 5000);
        }
        frames++;
        drawn = s.tick;
        if (!host) seen = drawn;
    }
    if (frames) epd_close();
    in_pong = false;
    ESP_LOGI(TAG, "pong over after %d frames: peer gone", frames);
}

// Unpaired: an empty court; then wait for the link task to pair.
static void epd_loop(void *arg)
{
    for (;;) {
        draw(&(game_t){.py = {2, 2}}, false);
        epd_open(fb, NULL, true); // red RAM cleared once
        epd_pull(3, 5000);
        epd_close();
        xTaskNotifyWait(0, 0, NULL, portMAX_DELAY);
        pong();
    }
}

// ---- LED bar: 8x WS2812, RMT at 10 MHz (0: 0.3 us high, 0.9 us low; 1: 0.9 us high, 0.3 us low).
static rmt_channel_handle_t led_ch;
static rmt_encoder_handle_t led_enc;

static void led_init(void)
{
    ESP_ERROR_CHECK(rmt_new_tx_channel(&(rmt_tx_channel_config_t){.gpio_num = LED_GPIO, .clk_src = RMT_CLK_SRC_DEFAULT,
                                                                   .resolution_hz = 10000000, .mem_block_symbols = 64,
                                                                   .trans_queue_depth = 1}, &led_ch));
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&(rmt_bytes_encoder_config_t){.bit0 = {.level0 = 1, .duration0 = 3, .duration1 = 9},
                                                                         .bit1 = {.level0 = 1, .duration0 = 9, .duration1 = 3},
                                                                         .flags.msb_first = 1}, &led_enc));
    ESP_ERROR_CHECK(rmt_enable(led_ch));
}

// LED 0 is the left end, as the court is seen on this badge.
static void leds(const uint8_t rgb[8][3])
{
    static uint8_t grb[24];
    uint8_t out[24];
    for (int i = 0; i < 8; i++) {
        uint8_t *o = out + 3 * (LED_FLIP ? 7 - i : i);
        o[0] = rgb[i][1], o[1] = rgb[i][0], o[2] = rgb[i][2];
    }
    if (!memcmp(out, grb, sizeof grb)) return;
    memcpy(grb, out, sizeof grb);
    rmt_transmit(led_ch, led_enc, grb, sizeof grb, &(rmt_transmit_config_t){0});
    rmt_tx_wait_all_done(led_ch, 10);
}

// Free: off (red for 0.5 s after a failed pairing or a lost peer). Seeking: blue. Playing: my points green from
// the left, the peer's red from the right, the ball's column white; all green / all red when someone has won.
static void led_update(int64_t now, int64_t red_until)
{
    uint8_t c[8][3] = {0};
    int fill = now < red_until ? 0 : lk.state == L_SEEK ? 2 : -1;
    if (lk.state == L_PAIRED) {
        taskENTER_CRITICAL(&gmux);
        game_t s = g;
        taskEXIT_CRITICAL(&gmux);
        int me = !host;
        fill = s.score[me] == WIN ? 1 : s.score[!me] == WIN ? 0 : -1;
        for (int k = 0; k < s.score[me]; k++) c[k][1] = LED_ON;
        for (int k = 0; k < s.score[!me]; k++) c[7 - k][0] = LED_ON;
        int col = host ? s.bx : PW - 1 - s.bx;
        if (s.tick) memset(c[(col * 7 + 4) / 9], LED_ON, 3);
    }
    if (fill >= 0)
        for (int i = 0; i < 8; i++) memset(c[i], 0, 3), c[i][fill] = LED_ON;
    leds(c);
}

// ---- Radio: ESP-NOW only (no AP), packets go through a queue to the link task.
typedef struct { uint8_t src[6]; pkt_t p; } rx_t;
static QueueHandle_t rxq;

static void radio_rx(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    rx_t r;
    if (len != sizeof r.p) return;
    memcpy(r.src, info->src_addr, 6);
    memcpy(&r.p, data, len);
    xQueueSend(rxq, &r, 0);
}

static void radio_start(void)
{
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE)); // modem sleep would drop packets
    ESP_ERROR_CHECK(esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(radio_rx));
    esp_now_peer_info_t bc = {.ifidx = WIFI_IF_STA};
    memcpy(bc.peer_addr, link_bcast, 6);
    ESP_ERROR_CHECK(esp_now_add_peer(&bc));
    esp_read_mac(lk.me, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "ESP-NOW on channel %d, me " MACSTR, CHANNEL, MAC2STR(lk.me));
}

// Runs link.h every 10 ms or per packet, sends, applies game packets, drives the LEDs.
static void link_task(void *arg)
{
    int was = L_FREE;
    int64_t next_tx = 0, red_until = 0;
    for (;;) {
        rx_t r;
        bool got = xQueueReceive(rxq, &r, pdMS_TO_TICKS(10));
        int64_t now = esp_timer_get_time() / 1000;
        if (__atomic_exchange_n(&a_pressed, false, __ATOMIC_RELAXED)) link_press(&lk, now);
        if (got) link_rx(&lk, r.src, &r.p, now);
        link_tick(&lk, now);
        if (lk.state != was) {
            ESP_LOGI(TAG, "link %d -> %d, peer " MACSTR, was, lk.state, MAC2STR(lk.peer));
            if (lk.state == L_PAIRED) {
                esp_now_peer_info_t pi = {.ifidx = WIFI_IF_STA};
                memcpy(pi.peer_addr, lk.peer, 6);
                esp_now_add_peer(&pi);
                taskENTER_CRITICAL(&gmux);
                host = memcmp(lk.me, lk.peer, 6) < 0;
                g = (game_t){.py = {2, 2}};
                seen = 0;
                taskEXIT_CRITICAL(&gmux);
                xTaskNotifyGive(epd_task);
            }
            if (was == L_PAIRED) esp_now_del_peer(lk.peer);
            if (lk.state == L_FREE) red_until = now + 500;
            was = lk.state;
        }
        if (got && lk.state == L_PAIRED && r.p.type == P_GAME && !memcmp(r.src, lk.peer, 6)) {
            taskENTER_CRITICAL(&gmux);
            if (host) {
                g.py[1] = MIN(r.p.g.py[1], PH - PAD);
                seen = r.p.seen;
            } else {
                uint8_t mine = g.py[1];
                g = r.p.g;
                g.py[1] = mine;
            }
            taskEXIT_CRITICAL(&gmux);
        }
        pkt_t p;
        const uint8_t *to = link_pkt(&lk, now, &p);
        if (to && now >= next_tx) {
            taskENTER_CRITICAL(&gmux);
            p.g = g;
            p.seen = seen;
            taskEXIT_CRITICAL(&gmux);
            esp_now_send(to, (const uint8_t *)&p, sizeof p);
            next_tx = now + (lk.state == L_SEEK ? LINK_PAIR_MS : LINK_GAME_MS);
        }
        led_update(now, red_until);
    }
}

// Buttons, active low, named as printed: A (GPIO39) pairs; in a game MENU + (GPIO40) moves the paddle up and
// MENU - (GPIO15) down, one cell per press.
static void buttons(void *arg)
{
    enum { A = 39, PLUS = 40, MINUS = 15 };
    gpio_config(&(gpio_config_t){.pin_bit_mask = BIT64(A) | BIT64(PLUS) | BIT64(MINUS), .mode = GPIO_MODE_INPUT,
                                 .pull_up_en = GPIO_PULLUP_ENABLE});
    for (int was = 0;; ms(20)) {
        int now = (!gpio_get_level(A)) | (!gpio_get_level(PLUS)) << 1 | (!gpio_get_level(MINUS)) << 2;
        int down = now & ~was;
        was = now;
        if (down & 1) __atomic_store_n(&a_pressed, true, __ATOMIC_RELAXED);
        if (down & 0b110 && in_pong) {
            taskENTER_CRITICAL(&gmux);
            uint8_t *y = &g.py[!host];
            *y = down & 0b010 ? MAX(*y - 1, 0) : MIN(*y + 1, PH - PAD);
            taskEXIT_CRITICAL(&gmux);
        }
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init(); // Wi-Fi keeps its calibration here
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    epd_init();
    led_init();
    rxq = xQueueCreate(16, sizeof(rx_t));
    radio_start();
    xTaskCreate(epd_loop, "epd", 4096, NULL, 5, &epd_task);
    xTaskCreate(buttons, "buttons", 3072, NULL, 3, NULL);
    xTaskCreate(link_task, "link", 4096, NULL, 6, NULL);
}
