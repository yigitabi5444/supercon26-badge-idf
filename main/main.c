// Billboard badge (Hackaday Supercon 2026): 9.7" 3-colour e-paper + Wi-Fi SoftAP image picker.
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>
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
static void wait_ready(const char *what)
{
    int64_t t0 = esp_timer_get_time(), t = 0;
    int l = -1, r = -1;
    while ((l < 0 || r < 0) && t < 120000000) {
        t = esp_timer_get_time() - t0;
        if (l < 0 && gpio_get_level(BUSY_L)) l = t / 1000;
        if (r < 0 && gpio_get_level(BUSY_R)) r = t / 1000;
        if (l < 0 || r < 0) ms(10);
    }
    ESP_LOGI(TAG, "%s: BUSY left %d ms, right %d ms", what, l, r);
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

// The update steps, same sequence as PDi EPD_Driver_GU_large and the stock epd.py:
// reset, load, cog_init, refresh, sleep.
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

// temp: CoG temperature input. 0x82 = 25 C (PDi). The waveform length follows it.
static void epd_cog_init(int temp)
{
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
        CMD(cog, 0x45, temp);
        CMD(cog, 0xA7, 0x10);
        ms(100);
        CMD(cog, 0xA7, 0x00);
        ms(100);
    }
    CMD(BOTH, 0x60, 0x25); // TCON
    CMD(LEFT, 0x61, 0x01); // STV_DIR, master only
    CMD(BOTH, 0x02, 0x00); // VCOM
}

// DC-DC up, run the waveform, DC-DC down. hold_ms > 0 cuts the waveform off after hold_ms
// (much faster, ghosting). Returns how long the refresh itself took.
static int epd_refresh(int hold_ms)
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
    ms(10);

    wait_ready("dc-dc on");
    int64_t t0 = esp_timer_get_time();
    CMD(BOTH, 0x15, 0x3C); // display refresh
    ms(5);
    if (hold_ms > 0) ms(hold_ms);
    else wait_ready("refresh");
    int t = (esp_timer_get_time() - t0) / 1000;

    CMD(BOTH, 0x09, 0x7F); // DC-DC soft shutdown
    CMD(BOTH, 0x05, 0x7D);
    CMD(BOTH, 0x09, 0x00);
    ms(200);
    wait_ready("dc-dc off");
    return t;
}

static void epd_sleep(void)
{
    gpio_set_level(DC, 0);
    gpio_set_level(CS_L, 0);
    gpio_set_level(RST, 0);
    gpio_set_level(CS_L, 1);
}

// Full 3-colour global update.
static void epd_show(const uint8_t *img)
{
    int64_t t0 = esp_timer_get_time();
    epd_reset();
    epd_load(img, img + PLANE, true);
    epd_cog_init(0x82);
    epd_refresh(0);
    epd_sleep();
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

// ---- Rickroll: mono frames, CoG set up once, then load + refresh per frame.
extern const uint8_t frames_bin[] asm("_binary_frames_1fps_bin_start");
extern const uint8_t frames_end[] asm("_binary_frames_1fps_bin_end");
static int rick_temp = 0x82, rick_hold = 0, rick_frames = 99; // tuning knobs, serial "r <temp> <hold> <frames>"
static volatile bool quit; // serial "q": stop the rickroll / snake

static void rickroll(void)
{
    int n = MIN(rick_frames, (frames_end - frames_bin) / PLANE);
    int64_t t0 = esp_timer_get_time();
    epd_reset();
    quit = false;
    for (int k = 0; k < n && !quit; k++) {
        int64_t t = esp_timer_get_time();
        epd_load(frames_bin + k * PLANE, NULL, k == 0); // red RAM cleared once
        if (k == 0) {
            epd_cog_init(rick_temp);
        } else {
            CMD(BOTH, 0x05, 0x00); // undo the discharge of the last DC-DC shutdown
            ms(10);
        }
        int r = epd_refresh(rick_hold);
        ESP_LOGI(TAG, "rick frame %d: refresh %d ms, frame %d ms", k, r, (int)((esp_timer_get_time() - t) / 1000));
    }
    epd_sleep();
    ESP_LOGI(TAG, "rickroll: %d frames in %d ms (temp 0x%02x, hold %d ms)", n,
             (int)((esp_timer_get_time() - t0) / 1000), rick_temp, rick_hold);
}

// ---- Snake: 48 px cells, black/white, CoG set up once, each move refreshes only the changed rows.
#define CELL 48
#define GW (W / CELL) // 20 x 14 grid
#define GH (H / CELL)
static int snake_temp = 0x82, snake_hold = 0, snake_window = 1; // tuning knobs, serial "n <temp> <hold> <window>"
static volatile bool in_snake;
static volatile int key_dir = -1; // last direction key: 0 left, 1 up, 2 down, 3 right
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
    int hd = 2, tl = 0, dir = 3, food, moves = 0;
    memset(fb, 0, sizeof fb);
    memset(occ, 0, sizeof occ);
    for (int k = 0; k <= hd; k++) {
        q[k] = GH / 2 * GW + 4 + k;
        occ[q[k]] = true;
        fill_cell(q[k], 3, true);
    }
    do food = esp_random() % N; while (occ[food]);
    fill_cell(food, 16, true);
    key_dir = -1;
    quit = false;
    in_snake = true;
    epd_reset();
    epd_load(fb, NULL, true);
    epd_cog_init(snake_temp);
    epd_refresh(snake_hold);
    for (;;) {
        int64_t t0 = esp_timer_get_time();
        int d = key_dir;
        if (d >= 0 && d != 3 - dir) dir = d; // no U-turns: opposite of d is 3 - d
        int x = q[hd] % GW + (dir == 3) - (dir == 0), y = q[hd] / GW + (dir == 2) - (dir == 1);
        if (x < 0 || x >= GW || y < 0 || y >= GH || quit) break;
        int h = y * GW + x, r0 = MIN(y, q[hd] / GW), r1 = MAX(y, q[hd] / GW);
        bool eat = h == food;
        if (!eat) { // the tail moves away first, so following it is allowed
            occ[q[tl]] = false;
            fill_cell(q[tl], 0, false);
            r0 = MIN(r0, q[tl] / GW), r1 = MAX(r1, q[tl] / GW);
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
            r0 = MIN(r0, food / GW), r1 = MAX(r1, food / GW);
        }
        epd_load(fb, NULL, false);
        if (snake_window) // DRFW rows: 4-line units, +2 dummy units, 1 unit margin
            CMD(BOTH, 0x90, 0x00, 0x3B, MAX(0, 12 * r0 + 1), MIN(0xA9, 12 * r1 + 14));
        CMD(BOTH, 0x05, 0x00);
        ms(10);
        int r = epd_refresh(snake_hold);
        ESP_LOGI(TAG, "snake move %d (rows %d-%d): refresh %d ms, move %d ms", ++moves, r0, r1, r,
                 (int)((esp_timer_get_time() - t0) / 1000));
    }
    in_snake = false;
    ESP_LOGI(TAG, "snake over: length %d after %d moves", (hd - tl + N) % N + 1, moves);
    epd_sleep();
    ms(2000);
}

// Buttons, active low. Normal mode: Menu+ / Menu- step through the images, A+B starts Snake.
// Snake: BUTTON_1..4 (A, Menu-, Menu+, B) = left, up, down, right.
static void buttons(void *arg)
{
    static const int pin[4] = {39, 40, 15, 16}; // A, Menu- (silkscreen says +), Menu+ (silkscreen says -), B
    gpio_config(&(gpio_config_t){.pin_bit_mask = BIT64(39) | BIT64(40) | BIT64(15) | BIT64(16),
                                 .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE});
    for (int was = 0;; ms(20)) {
        int now = 0;
        for (int k = 0; k < 4; k++) now |= !gpio_get_level(pin[k]) << k;
        int down = now & ~was;
        was = now;
        if (!down) continue;
        if (in_snake) {
            key_dir = __builtin_ctz(down);
        } else if (now == 0b1001) {
            xTaskNotify(epd_task, SNAKE, eSetValueWithOverwrite);
        } else if (down & 0b0110) {
            int i = cur, total = ndef + NSLOT;
            do i = (i + (down & 0b0100 ? 1 : total - 1)) % total; while (!exists(i));
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

// Serial console, for tuning without Wi-Fi (no redraw after):
// "s <image>", "r <temp> <hold_ms> <frames>" rickroll, "n <temp> <hold_ms> <window 0/1>" snake.
static void console(void *arg)
{
    char line[48];
    size_t n = 0;
    for (;;) {
        char c;
        if (read(0, &c, 1) != 1) { // the USB-JTAG console without driver never blocks
            ms(20);
            continue;
        }
        if (c != '\n') {
            if (n < sizeof line - 1) line[n++] = c;
            continue;
        }
        line[n] = 0;
        n = 0;
        int a = 0, b = 0, d = 99;
        sscanf(line + 1, "%i %i %i", &a, &b, &d);
        if (line[0] == 's' && exists(a)) show(a);
        if (line[0] == 'q') quit = true;
        if (line[0] == 'r') {
            rick_temp = a, rick_hold = b, rick_frames = d;
            xTaskNotify(epd_task, RICK, eSetValueWithOverwrite);
        }
        if (line[0] == 'n') {
            snake_temp = a, snake_hold = b, snake_window = d;
            xTaskNotify(epd_task, SNAKE, eSetValueWithOverwrite);
        }
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
    xTaskNotify(epd_task, SNAKE | REDRAW, eSetValueWithOverwrite);
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

static void web_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap(); // 192.168.4.1, DHCP server on
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    wifi_config_t c = {.ap = {.password = "supercon", .channel = 1, .max_connection = 4, .authmode = WIFI_AUTH_WPA2_PSK}};
    c.ap.ssid_len = snprintf((char *)c.ap.ssid, sizeof c.ap.ssid, "badge-%02X%02X", mac[4], mac[5]);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &c));
    ESP_ERROR_CHECK(esp_wifi_start());
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
    show(exists(c) ? c : 0); // e-paper keeps its image, but redraw so boot always ends in a known state
    xTaskCreate(console, "console", 3072, NULL, 2, NULL);
    xTaskCreate(buttons, "buttons", 3072, NULL, 3, NULL);
    web_start();
}
