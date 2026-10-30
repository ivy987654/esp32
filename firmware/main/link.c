#include "link.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_rgb.h"
#include "board.h"

#define MAX_PAYLOAD 1100
#define N_CHUNKS ((ART_BYTES + ART_CHUNK - 1) / ART_CHUNK)

static link_state_t s_state;
volatile int g_voice_level;
volatile int g_goto_screen = -1;
static SemaphoreHandle_t s_mux;

/* stats reported back to the host */
static uint32_t s_ok, s_bad, s_art_id;
static bool s_snap;                 /* host asked for a piece of the screen */
static uint32_t s_snap_off, s_snap_len;

/* album art: assembled in s_art_rx, copied to s_art when complete and CRC ok */
static uint8_t *s_art, *s_art_rx;
static uint32_t s_rx_id, s_rx_crc;
static uint8_t s_have[(N_CHUNKS + 7) / 8];

static uint16_t crc16(uint16_t c, const uint8_t *p, int n)
{
    while (n--) {
        c ^= (uint16_t)*p++ << 8;
        for (int i = 0; i < 8; i++) c = c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1;
    }
    return c;
}

static uint32_t crc32(const uint8_t *p, int n)
{
    uint32_t c = 0xFFFFFFFF;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++) c = c & 1 ? (c >> 1) ^ 0xEDB88320 : c >> 1;
    }
    return ~c;
}

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* split NUL-terminated payload on 0x1F; missing fields come back as "" */
static int fields(char *p, char **f, int max)
{
    int n = 0;
    f[n++] = p;
    for (; *p && n < max; p++) {
        if (*p == 0x1F) {
            *p = 0;
            f[n++] = p + 1;
        }
    }
    for (int i = n; i < max; i++) f[i] = "";
    return n;
}

#define CP(dst, src) strlcpy((dst), (src), sizeof(dst))

static void on_art_head(char **f)
{
    uint32_t id = strtoul(f[0], NULL, 10);
    if (id == s_rx_id) return;
    s_rx_id = id;
    s_rx_crc = strtoul(f[1], NULL, 10);
    memset(s_have, 0, sizeof(s_have));
}

static void on_art_chunk(const uint8_t *p, int len)
{
    if (len < 8) return;
    uint32_t id = rd32(p), off = rd32(p + 4);
    p += 8;
    len -= 8;
    if (id != s_rx_id || off % ART_CHUNK || off + len > ART_BYTES) return;
    if (len != ART_CHUNK && off + len != ART_BYTES) return;
    memcpy(s_art_rx + off, p, len);
    int k = off / ART_CHUNK;
    s_have[k / 8] |= 1 << (k % 8);
    for (int i = 0; i < N_CHUNKS; i++)
        if (!(s_have[i / 8] & (1 << (i % 8)))) return;
    /* complete */
    memset(s_have, 0, sizeof(s_have));
    if (crc32(s_art_rx, ART_BYTES) != s_rx_crc) {
        s_bad++;
        return;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    memcpy(s_art, s_art_rx, ART_BYTES);
    s_art_id = id;
    s_state.art_ver++;
    s_state.ver++;
    xSemaphoreGive(s_mux);
}

static void on_frame(uint8_t type, uint8_t *p, int len)
{
    if (type == 'A') {
        on_art_chunk(p, len);
        return;
    }
    p[len] = 0;
    char *f[12];
    fields((char *)p, f, 12);
    if (type == 'V') {
        g_voice_level = atoi(f[0]);
        return;
    }
    if (type == 'H') {
        on_art_head(f);
        return;
    }
    if (type == 'G') {             /* debug: show screen n (0 home, 1 keys) */
        g_goto_screen = atoi(f[0]);
        return;
    }
    if (type == 'P') {             /* P <byte offset> <length> of the frame buffer */
        s_snap_off = strtoul(f[0], NULL, 10);
        s_snap_len = strtoul(f[1], NULL, 10);
        s_snap = true;
        return;
    }

    xSemaphoreTake(s_mux, portMAX_DELAY);
    link_state_t *s = &s_state;
    switch (type) {
    case 'M':
        CP(s->title, f[0]); CP(s->artist, f[1]); CP(s->album, f[2]);
        s->playing = atoi(f[3]);
        s->pos_ms = atoi(f[4]);
        s->dur_ms = atoi(f[5]);
        s->pos_at_us = esp_timer_get_time();
        break;
    case 'Y':
        for (int i = 0; i < 5; i++) CP(s->lyr[i], f[i]);
        break;
    case 'C':
        s->epoch = atoll(f[0]);
        s->tz = atoi(f[1]);
        s->sync_us = esp_timer_get_time();
        break;
    case 'W':
        CP(s->w_city, f[0]); CP(s->w_temp, f[1]); CP(s->w_desc, f[2]); CP(s->w_range, f[3]);
        CP(s->w_feels, f[4]); CP(s->w_hum, f[5]); CP(s->w_wind, f[6]);
        for (int i = 0; i < 3; i++) CP(s->w_fc[i], f[7 + i]);
        break;
    case 'K':
        CP(s->key_name, f[0]); CP(s->key_code, f[1]); CP(s->key_stream, f[2]);
        CP(s->key_count, f[3]); CP(s->key_kpm, f[4]);
        break;
    case 'S':
        for (int i = 0; i < 10; i++) CP(s->sys[i], f[i]);
        break;
    case 'R':
        CP(s->bot_state, f[0]); CP(s->bot_emotion, f[1]);
        CP(s->bot_user, f[2]); CP(s->bot_reply, f[3]);
        break;
    default:
        xSemaphoreGive(s_mux);
        return;
    }
    s->ver++;
    s->last_rx_us = esp_timer_get_time();
    xSemaphoreGive(s_mux);
}

static void report(void)
{
    char b[128];
    int n = snprintf(b, sizeof(b), "\n#S up=%lld ok=%lu bad=%lu art=%lu clk=%d hi=%u hp=%u\n",
                     esp_timer_get_time() / 1000000, (unsigned long)s_ok, (unsigned long)s_bad,
                     (unsigned long)s_art_id, s_state.sync_us != 0,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    uart_write_bytes(UART_NUM_0, b, n);
}

/* debug aid: send a block of the frame buffer (RGB565 LE) so the host can save what the panel shows.
 *   "#B <off> <len> <crc32>\n" + raw bytes */
static void snapshot(void)
{
    void *fb = NULL;
    if (esp_lcd_rgb_panel_get_frame_buffer(g_panel, 1, &fb) != ESP_OK || !fb) return;
    uint32_t total = LCD_W * LCD_H * 2, off = s_snap_off, len = s_snap_len;
    if (off >= total) return;
    if (len > 16384) len = 16384;
    if (off + len > total) len = total - off;
    /* offset 0 takes a copy of the whole frame, later blocks come from that copy,
     * so the picture is one instant even though it travels in pieces */
    static uint8_t *shot;
    if (!shot) shot = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    if (!shot) return;
    if (off == 0) memcpy(shot, fb, total);
    static uint8_t blk[16384];
    memcpy(blk, shot + off, len);
    char hdr[64];
    int n = snprintf(hdr, sizeof(hdr), "\n#B %lu %lu %lu\n", (unsigned long)off, (unsigned long)len,
                     (unsigned long)crc32(blk, len));
    uart_write_bytes(UART_NUM_0, hdr, n);
    uart_write_bytes(UART_NUM_0, blk, len);
    uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(2000));
}

static void rx_task(void *arg)
{
    /* installed from core 1 so the UART ISR isn't starved by the RGB bounce-buffer ISR on core 0 */
    uart_driver_install(UART_NUM_0, 32768, 2048, 0, NULL, 0);
    uart_set_baudrate(UART_NUM_0, LINK_BAUD);
    uart_set_rx_full_threshold(UART_NUM_0, 64);
    uart_set_rx_timeout(UART_NUM_0, 4);

    /* everything the parser touches lives in internal RAM */
    static uint8_t frame[MAX_PAYLOAD + 1];
    static uint8_t buf[512];
    enum { S_SYNC0, S_SYNC1, S_TYPE, S_LEN0, S_LEN1, S_DATA, S_CRC0, S_CRC1 } st = S_SYNC0;
    uint8_t type = 0;
    int len = 0, got = 0;
    uint16_t crc = 0;
    int64_t last_report = 0;

    while (1) {
        int r = uart_read_bytes(UART_NUM_0, buf, sizeof(buf), pdMS_TO_TICKS(20));
        for (int i = 0; i < r; i++) {
            uint8_t c = buf[i];
            switch (st) {
            case S_SYNC0: if (c == 0xA5) st = S_SYNC1; break;
            case S_SYNC1: st = c == 0x5A ? S_TYPE : (c == 0xA5 ? S_SYNC1 : S_SYNC0); break;
            case S_TYPE: type = c; st = S_LEN0; break;
            case S_LEN0: len = c; st = S_LEN1; break;
            case S_LEN1:
                len |= c << 8;
                got = 0;
                if (len > MAX_PAYLOAD) { s_bad++; st = S_SYNC0; }
                else st = len ? S_DATA : S_CRC0;
                break;
            case S_DATA:
                frame[got++] = c;
                if (got == len) st = S_CRC0;
                break;
            case S_CRC0: crc = c; st = S_CRC1; break;
            case S_CRC1: {
                crc |= c << 8;
                uint8_t hdr[3] = {type, len & 0xFF, len >> 8};
                uint16_t want = crc16(crc16(0xFFFF, hdr, 3), frame, len);
                if (want == crc) {
                    s_ok++;
                    on_frame(type, frame, len);
                } else {
                    s_bad++;
                }
                st = S_SYNC0;
                break;
            }
            }
        }
        if (s_snap) {
            s_snap = false;
            snapshot();
        }
        int64_t now = esp_timer_get_time();
        if (now - last_report > 1000000) {
            last_report = now;
            report();
        }
    }
}

void link_start(void)
{
    s_mux = xSemaphoreCreateMutex();
    s_art = heap_caps_calloc(1, ART_BYTES, MALLOC_CAP_SPIRAM);
    s_art_rx = heap_caps_calloc(1, ART_BYTES, MALLOC_CAP_SPIRAM);
    strcpy(s_state.title, "等待电脑连接");
    strcpy(s_state.artist, "");
    strcpy(s_state.bot_state, "idle");
    strcpy(s_state.bot_reply, "说「你好耄耋」叫我");
    s_state.ver = 1;
    xTaskCreatePinnedToCore(rx_task, "link", 6144, NULL, 10, NULL, 1);
}

bool link_get(link_state_t *out, uint32_t *ver)
{
    bool changed = false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_state.ver != *ver) {
        *out = s_state;
        *ver = s_state.ver;
        changed = true;
    }
    xSemaphoreGive(s_mux);
    return changed;
}

void link_get_art(uint8_t *dst)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    memcpy(dst, s_art, ART_BYTES);
    xSemaphoreGive(s_mux);
}
