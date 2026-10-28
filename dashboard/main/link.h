#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Data pushed from the Mac companion script over UART0 (tab separated lines):
 *   M  title artist album playing(0/1) pos_s dur_s
 *   Y  lyric-2 lyric-1 lyric lyric+1 lyric+2
 *   C  unix_epoch tz_offset_s
 *   W  temp desc city feels humidity wind range fc1 fc2 fc3
 *   K  last_key recent_text total_count kpm
 *   R  state(idle/listen/think/speak) user_text reply_text emotion(happy/sad/angry/surprised/calm/playful)
 *   V  voice_level(0-100)   ~10 Hz mic level for the robot line
 *   A  base64(RGB565 little endian, ART_N x ART_N)   album art
 */
#define ART_N 96
typedef struct {
    char title[128], artist[128], album[128];
    bool playing;
    int pos, dur;
    char lyr[5][160];

    int64_t epoch;          /* epoch at sync */
    int tz;                 /* seconds east of UTC */
    int64_t sync_us;        /* esp_timer at sync, 0 = never */

    char w_temp[16], w_desc[64], w_city[64], w_feels[16], w_hum[16], w_wind[48], w_range[32];
    char w_fc[3][64];

    char key_last[32], key_stream[96];
    uint32_t key_count;
    int kpm;

    char bot_state[16], bot_user[256], bot_reply[1024], bot_emotion[16];

    uint8_t art[ART_N * ART_N * 2];
    uint32_t art_ver;

    uint32_t ver;
    int64_t last_rx_us;
} link_state_t;

extern volatile int g_voice_level;

void link_start(void);
/* copy state if it changed since *ver; returns true when copied */
bool link_get(link_state_t *out, uint32_t *ver);
