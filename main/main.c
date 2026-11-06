// Billboard badge (Hackaday Supercon 2026): 9.7" 3-colour e-paper + Wi-Fi SoftAP image picker.
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
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

// Full 3-colour update on the panel's own waveform.
static void epd_show(const uint8_t *img)
{
    int64_t t0 = esp_timer_get_time();
    epd_open(img, img + PLANE, false);
    epd_pull(1, 120000);
    epd_close();
    ESP_LOGI(TAG, "update done in %d ms", (int)((esp_timer_get_time() - t0) / 1000));
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

// ---- Images: baked-in defaults (in the app), then NSLOT upload slots in the "images" partition.
#define NSLOT 16
#define SLOT 0x28000 // IMG rounded up to whole 4 KB flash sectors

extern const uint8_t defaults_bin[] asm("_binary_defaults_bin_start");
extern const uint8_t defaults_end[] asm("_binary_defaults_bin_end");
extern const char index_html[] asm("_binary_index_html_start");
extern const char index_end[] asm("_binary_index_html_end");

static const esp_partition_t *part;
static const uint8_t *slots; // the whole images partition, memory-mapped
static nvs_handle_t nvs;
static TaskHandle_t epd_task;
enum { RICK = 1 << 16, REDRAW = 1 << 17, SNAKE = 1 << 18 }; // epd_task requests besides image numbers
static int ndef, cur;
static uint32_t valid; // bit k: slot k holds a complete upload

static const uint8_t *image(int i) { return i < ndef ? defaults_bin + i * IMG : slots + (i - ndef) * SLOT; }
static bool exists(int i) { return i >= 0 && (i < ndef || (i < ndef + NSLOT && (valid >> (i - ndef) & 1))); }

static void show(int i)
{
    cur = i;
    nvs_set_i32(nvs, "cur", i);
    nvs_commit(nvs);
    xTaskNotify(epd_task, i, eSetValueWithOverwrite); // a request during a refresh runs right after it
}

// ---- Rickroll: mono frames in one short-waveform session: a frame is its plane data + pulls.
extern const uint8_t frames_bin[] asm("_binary_frames_1fps_bin_start");
extern const uint8_t frames_end[] asm("_binary_frames_1fps_bin_end");
#define RICK_PULLS 1 // per frame after the first: 1 = ~0.94 fps with ghosting, 2-3 = cleaner and slower

static void rickroll(void)
{
    int n = (frames_end - frames_bin) / PLANE;
    int64_t t0 = esp_timer_get_time();
    epd_open(frames_bin, NULL, true); // red RAM cleared once
    epd_pull(3, 5000);                // first frame at full dose
    char log[160];                    // frame times, logged once at the end
    int len = snprintf(log, sizeof log, "%d", (int)((esp_timer_get_time() - t0) / 1000));
    for (int k = 1; k < n; k++) {
        int64_t t = esp_timer_get_time();
        epd_load(frames_bin + k * PLANE, NULL, false);
        epd_pull(RICK_PULLS, 5000);
        len += snprintf(log + len, sizeof log - MIN(len, sizeof log), " %d", (int)((esp_timer_get_time() - t) / 1000));
    }
    epd_close();
    ESP_LOGI(TAG, "rickroll: %d frames in %d ms, %d pull(s)/frame, frame ms: %s", n,
             (int)((esp_timer_get_time() - t0) / 1000), RICK_PULLS, log);
}

// ---- Snake: 48 px cells, black/white, short-waveform session, a move is plane data + pulls.
#define CELL 48
#define GW (W / CELL) // 20 x 14 grid
#define GH (H / CELL)
#define SNAKE_PULLS 1
static volatile bool in_snake;
static int key_turn; // set by the buttons: +1 clockwise, -1 counter-clockwise; one turn per move
static uint8_t fb[PLANE];         // snake screen, black plane

static void fill_cell(int c, int inset, bool ink)
{
    int x0 = c % GW * CELL + inset, y0 = c / GW * CELL + inset, n = CELL - 2 * inset;
    for (int y = y0; y < y0 + n; y++)
        for (int x = x0; x < x0 + n; x++)
            if (ink) fb[y * (W / 8) + x / 8] |= 0x80 >> (x & 7);
            else fb[y * (W / 8) + x / 8] &= ~(0x80 >> (x & 7));
}

static void snake(void)
{
    enum { N = GW * GH };
    static int q[N]; // ring buffer of body cells, q[tl] = tail .. q[hd] = head
    static bool occ[N];
    int hd = 2, tl = 0, dir = 1, food, moves = 0; // dir clockwise from up: 0 up, 1 right, 2 down, 3 left
    memset(fb, 0, sizeof fb);
    memset(occ, 0, sizeof occ);
    for (int k = 0; k <= hd; k++) {
        q[k] = GH / 2 * GW + 4 + k;
        occ[q[k]] = true;
        fill_cell(q[k], 3, true);
    }
    do food = esp_random() % N; while (occ[food]);
    fill_cell(food, 16, true);
    key_turn = 0;
    in_snake = true;
    epd_open(fb, NULL, true);
    epd_pull(3, 5000);
    char log[200]; // move times, logged once at the end
    int len = 0;
    for (;;) {
        int64_t t0 = esp_timer_get_time();
        dir = (dir + __atomic_exchange_n(&key_turn, 0, __ATOMIC_RELAXED) + 4) % 4;
        int x = q[hd] % GW + (dir == 1) - (dir == 3), y = q[hd] / GW + (dir == 2) - (dir == 0);
        if (x < 0 || x >= GW || y < 0 || y >= GH) break;
        int h = y * GW + x;
        bool eat = h == food;
        if (!eat) { // the tail moves away first, so following it is allowed
            occ[q[tl]] = false;
            fill_cell(q[tl], 0, false);
            tl = (tl + 1) % N;
        }
        if (occ[h]) break;
        hd = (hd + 1) % N;
        q[hd] = h;
        occ[h] = true;
        fill_cell(h, 3, true);
        if (eat) {
            if ((hd - tl + N) % N == N - 1) break; // board full: you win
            do food = esp_random() % N; while (occ[food]);
            fill_cell(food, 16, true);
        }
        if (++moves % 20 == 0) { // badge team: a fresh session every 20 short refreshes
            epd_close();
            epd_open(fb, NULL, true);
        } else {
            epd_load(fb, NULL, false);
        }
        epd_pull(SNAKE_PULLS, 5000);
        len += snprintf(log + len, sizeof log - MIN(len, sizeof log), " %d", (int)((esp_timer_get_time() - t0) / 1000));
    }
    in_snake = false;
    ESP_LOGI(TAG, "snake over: length %d after %d moves, move ms:%s", (hd - tl + N) % N + 1, moves, len ? log : "");
    epd_close();
    ms(2000);
}

// Buttons, active low. Snake is the default mode and uses two buttons, named as printed on the badge
// (the silkscreen swaps the stock code's Menu +/-): "MENU -" (GPIO15) turns clockwise, "MENU +" (GPIO40)
// counter-clockwise; between games either one starts a new game. A / B step back / forward through the images.
static void buttons(void *arg)
{
    enum { A = 39, PLUS = 40, MINUS = 15, B = 16 }; // printed labels
    gpio_config(&(gpio_config_t){.pin_bit_mask = BIT64(A) | BIT64(PLUS) | BIT64(MINUS) | BIT64(B),
                                 .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE});
    for (int was = 0;; ms(20)) {
        int now = (!gpio_get_level(A)) | (!gpio_get_level(PLUS)) << 1 | (!gpio_get_level(MINUS)) << 2 |
                  (!gpio_get_level(B)) << 3;
        int down = now & ~was;
        was = now;
        if (down & 0b0110) {
            if (in_snake) __atomic_store_n(&key_turn, down & 0b0100 ? 1 : -1, __ATOMIC_RELAXED);
            else xTaskNotify(epd_task, SNAKE, eSetValueWithOverwrite);
        } else if (down & 0b1001 && !in_snake) {
            int i = cur, total = ndef + NSLOT;
            do i = (i + (down & 0b1000 ? 1 : total - 1)) % total; while (!exists(i));
            show(i);
        }
    }
}

static void epd_loop(void *arg)
{
    for (;;) {
        uint32_t i;
        xTaskNotifyWait(0, 0, &i, portMAX_DELAY);
        if (i & (RICK | SNAKE)) {
            if (i & RICK) rickroll();
            else snake();
            if (!(i & REDRAW)) continue;
            i = cur;
        }
        ESP_LOGI(TAG, "showing image %d", (int)i);
        epd_show(image(i));
    }
}

// ---- Web UI
static int query(httpd_req_t *r, const char *key)
{
    char q[32], v[8];
    if (httpd_req_get_url_query_str(r, q, sizeof q) != ESP_OK || httpd_query_key_value(q, key, v, sizeof v) != ESP_OK)
        return -1;
    return atoi(v);
}

static esp_err_t get_page(httpd_req_t *r) { return httpd_resp_send(r, index_html, index_end - index_html); }

static esp_err_t get_list(httpd_req_t *r)
{
    char s[80];
    snprintf(s, sizeof s, "{\"cur\":%d,\"ndef\":%d,\"nslot\":%d,\"valid\":%u}", cur, ndef, NSLOT, (unsigned)valid);
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, s);
}

static esp_err_t get_image(httpd_req_t *r)
{
    int i = query(r, "i");
    if (!exists(i)) return httpd_resp_send_404(r);
    httpd_resp_set_type(r, "application/octet-stream");
    return httpd_resp_send(r, (const char *)image(i), IMG);
}

static esp_err_t post_rick(httpd_req_t *r)
{
    xTaskNotify(epd_task, RICK | REDRAW, eSetValueWithOverwrite);
    return httpd_resp_sendstr(r, "ok");
}

static esp_err_t post_snake(httpd_req_t *r)
{
    xTaskNotify(epd_task, SNAKE, eSetValueWithOverwrite);
    return httpd_resp_sendstr(r, "ok");
}

static esp_err_t post_show(httpd_req_t *r)
{
    int i = query(r, "i");
    if (!exists(i)) return httpd_resp_send_404(r);
    show(i);
    return httpd_resp_sendstr(r, "ok");
}

// Body: one image (black plane, red plane), streamed straight into the slot.
static esp_err_t post_upload(httpd_req_t *r)
{
    static uint8_t buf[4096];
    int s = query(r, "s");
    if (s < 0 || s >= NSLOT || r->content_len != IMG)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "want ?s=<slot> and a 161280 byte body");
    valid &= ~(1u << s); // a half-written slot must not count after a power cut
    nvs_set_u32(nvs, "valid", valid);
    nvs_commit(nvs);
    ESP_ERROR_CHECK(esp_partition_erase_range(part, s * SLOT, SLOT));
    for (size_t got = 0; got < IMG;) {
        int n = httpd_req_recv(r, (char *)buf, MIN(sizeof buf, IMG - got));
        if (n <= 0) return ESP_FAIL; // client gone or timed out: slot stays invalid
        ESP_ERROR_CHECK(esp_partition_write(part, s * SLOT + got, buf, n));
        got += n;
    }
    valid |= 1u << s;
    nvs_set_u32(nvs, "valid", valid); // committed by show()
    ESP_LOGI(TAG, "slot %d uploaded", s);
    show(ndef + s);
    return httpd_resp_sendstr(r, "ok");
}

// Captive portal, part 1: every DNS A query gets 192.168.4.1, so the phone's connectivity check lands here.
static void dns_task(void *arg)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    bind(s, (struct sockaddr *)&a, sizeof a);
    static const uint8_t answer[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1}; // name ptr, A, IN, TTL 60
    for (;;) {
        uint8_t b[512];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(s, b, sizeof b - sizeof answer, 0, (struct sockaddr *)&from, &fl);
        int p = 12; // skip the header, then the first question's name
        while (p < n && b[p]) p += b[p] + 1;
        p += 5; // name end, QTYPE, QCLASS
        if (n < 12 || p > n) continue;
        bool a_query = b[p - 4] == 0 && b[p - 3] == 1;
        b[2] = 0x81, b[3] = 0x80;                     // response, no error
        b[4] = 0, b[5] = 1, b[6] = 0, b[7] = a_query; // 1 question, 1 answer for A (others: none)
        memset(b + 8, 0, 4);                          // drop authority/additional (EDNS)
        if (a_query) memcpy(b + p, answer, sizeof answer), p += sizeof answer;
        sendto(s, b, p, 0, (struct sockaddr *)&from, fl);
    }
}

// Captive portal, part 2: anything unknown (generate_204, hotspot-detect.html, ...) goes to the page.
// iOS wants a body, not only the redirect.
static esp_err_t to_portal(httpd_req_t *r, httpd_err_code_t err)
{
    httpd_resp_set_status(r, "303 See Other");
    httpd_resp_set_hdr(r, "Location", "http://192.168.4.1/");
    return httpd_resp_sendstr(r, "badge config: http://192.168.4.1/");
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_AP_STACONNECTED) ESP_LOGI(TAG, "station joined, free heap %u", (unsigned)esp_get_free_heap_size());
    if (id == WIFI_EVENT_AP_STADISCONNECTED)
        ESP_LOGI(TAG, "station left, reason %d", ((wifi_event_ap_stadisconnected_t *)data)->reason);
}

static void web_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap(); // 192.168.4.1, DHCP server on
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    wifi_config_t c = {.ap = {.password = "supercon", .channel = 1, .max_connection = 4, .authmode = WIFI_AUTH_WPA2_PSK}};
    c.ap.ssid_len = snprintf((char *)c.ap.ssid, sizeof c.ap.ssid, "badge-%02X%02X", mac[4], mac[5]);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &c));
    ESP_ERROR_CHECK(esp_wifi_start());
    xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "SoftAP \"%s\", password \"supercon\", http://192.168.4.1/", c.ap.ssid);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;
    httpd_handle_t srv;
    ESP_ERROR_CHECK(httpd_start(&srv, &cfg));
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = get_page},
        {.uri = "/list", .method = HTTP_GET, .handler = get_list},
        {.uri = "/img", .method = HTTP_GET, .handler = get_image},
        {.uri = "/show", .method = HTTP_POST, .handler = post_show},
        {.uri = "/upload", .method = HTTP_POST, .handler = post_upload},
        {.uri = "/rick", .method = HTTP_POST, .handler = post_rick},
        {.uri = "/snake", .method = HTTP_POST, .handler = post_snake},
    };
    for (int i = 0; i < sizeof uris / sizeof *uris; i++) httpd_register_uri_handler(srv, &uris[i]);
    httpd_register_err_handler(srv, HTTPD_404_NOT_FOUND, to_portal);
    ESP_LOGI(TAG, "web up, free heap %u (min %u)", (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(nvs_open("badge", NVS_READWRITE, &nvs));
    nvs_get_u32(nvs, "valid", &valid);
    int32_t c = 0;
    nvs_get_i32(nvs, "cur", &c);

    part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "images");
    assert(part && part->size >= NSLOT * SLOT);
    esp_partition_mmap_handle_t map;
    ESP_ERROR_CHECK(esp_partition_mmap(part, 0, NSLOT * SLOT, ESP_PARTITION_MMAP_DATA, (const void **)&slots, &map));
    ndef = (defaults_end - defaults_bin) / IMG;
    ESP_LOGI(TAG, "%d built-in images, slots in use 0x%04x", ndef, (unsigned)valid);

    epd_init();
    xTaskCreate(epd_loop, "epd", 4096, NULL, 5, &epd_task);
    cur = exists(c) ? c : 0;
    xTaskNotify(epd_task, SNAKE, eSetValueWithOverwrite); // Snake is the default mode
    xTaskCreate(buttons, "buttons", 3072, NULL, 3, NULL);
    web_start();
}
