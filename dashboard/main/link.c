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

static link_state_t s_state;
volatile int g_voice_level;
static bool s_debug = false;
static uint32_t s_bad;
static SemaphoreHandle_t s_mux;

static int b64v(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int b64_decode(const char *in, uint8_t *out, int max)
{
    int n = 0, bits = 0, acc = 0;
    for (; *in; in++) {
        int v = b64v(*in);
        if (v < 0) continue;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n < max) out[n++] = (acc >> bits) & 0xFF;
        }
    }
    return n;
}

static int split(char *line, char **f, int max)
{
    int n = 0;
    f[n++] = line;
    for (char *p = line; *p && n < max; p++) {
        if (*p == '\t') {
            *p = 0;
            f[n++] = p + 1;
        }
    }
    return n;
}

#define CP(dst, src) strlcpy((dst), (src), sizeof(dst))

/* every line ends with "\t#<payload byte length>"; anything else got mangled on the wire */
static bool frame_ok(char *line)
{
    char *h = strrchr(line, '#');
    if (!h || h == line || h[-1] != '\t') return false;
    int want = atoi(h + 1);
    h[-1] = 0;
    return want == (int)(h - 1 - line);
}

static void handle(char *line)
{
    if (!frame_ok(line)) {
        s_bad++;
        return;
    }
    char *f[12];
    int n = split(line, f, 12);
    if (s_debug && line[0] != 'V') printf("[rx] '%s' n=%d len1=%d\n", f[0], n, n > 1 ? (int)strlen(f[1]) : -1);
    if (n < 2 || f[0][1] != 0) return;
    if (f[0][0] == 'V') {               /* high rate, kept out of the state copy */
        g_voice_level = atoi(f[1]);
        return;
    }

    xSemaphoreTake(s_mux, portMAX_DELAY);
    link_state_t *s = &s_state;
    switch (f[0][0]) {
    case 'M':
        if (n < 7) break;
        CP(s->title, f[1]); CP(s->artist, f[2]); CP(s->album, f[3]);
        s->playing = atoi(f[4]); s->pos = atoi(f[5]); s->dur = atoi(f[6]);
        break;
    case 'Y':
        for (int i = 0; i < 5; i++) CP(s->lyr[i], i + 1 < n ? f[i + 1] : "");
        break;
    case 'C':
        if (n < 3) break;
        s->epoch = atoll(f[1]); s->tz = atoi(f[2]); s->sync_us = esp_timer_get_time();
        break;
    case 'W':
        if (n < 11) break;
        CP(s->w_temp, f[1]); CP(s->w_desc, f[2]); CP(s->w_city, f[3]); CP(s->w_feels, f[4]);
        CP(s->w_hum, f[5]); CP(s->w_wind, f[6]); CP(s->w_range, f[7]);
        for (int i = 0; i < 3; i++) CP(s->w_fc[i], f[8 + i]);
        break;
    case 'R':
        if (n < 4) break;
        CP(s->bot_state, f[1]); CP(s->bot_user, f[2]); CP(s->bot_reply, f[3]);
        CP(s->bot_emotion, n > 4 ? f[4] : "");
        break;
    case 'A':
        if (n < 2) break;
        if (b64_decode(f[1], s->art, sizeof(s->art)) == sizeof(s->art)) s->art_ver++;
        break;
    case 'K':
        if (n < 5) break;
        CP(s->key_last, f[1]); CP(s->key_stream, f[2]);
        s->key_count = strtoul(f[3], NULL, 10); s->kpm = atoi(f[4]);
        break;
    default:
        xSemaphoreGive(s_mux);
        return;
    }
    s->ver++;
    s->last_rx_us = esp_timer_get_time();
    xSemaphoreGive(s_mux);
}

static void rx_task(void *arg)
{
    /* install here (core 1) so the UART ISR isn't starved by the RGB bounce-buffer ISR on core 0 */
    uart_driver_install(UART_NUM_0, 16384, 0, 0, NULL, 0);
    uart_set_rx_full_threshold(UART_NUM_0, 32);
    uart_set_rx_timeout(UART_NUM_0, 4);
    const int LINE_CAP = 40000;
    char *line = heap_caps_malloc(LINE_CAP, MALLOC_CAP_SPIRAM);
    int len = 0;
    uint8_t buf[256];
    while (1) {
        int r = uart_read_bytes(UART_NUM_0, buf, sizeof(buf), pdMS_TO_TICKS(50));
        for (int i = 0; i < r; i++) {
            char c = buf[i];
            if (c == '\n') {
                line[len] = 0;
                if (len && line[len - 1] == '\r') line[len - 1] = 0;
                handle(line);
                len = 0;
            } else if (len < LINE_CAP - 1) {
                line[len++] = c;
            }
        }
    }
}

void link_start(void)
{
    s_mux = xSemaphoreCreateMutex();
    strcpy(s_state.title, "等待电脑连接…");
    strcpy(s_state.lyr[2], "运行电脑上的 dashboard.py");
    strcpy(s_state.w_desc, "--");
    strcpy(s_state.key_last, "-");
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
