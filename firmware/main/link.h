#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Binary link from the Mac (host/dashboard.py), see PROTOCOL.md.
 *
 *   frame = A5 5A | type | len lo | len hi | payload[len] | crc16 lo | crc16 hi
 *   crc16 = CRC-16/CCITT-FALSE over type, len and payload.
 *
 * Text frames carry UTF-8 fields separated by 0x1F. A frame that fails the CRC
 * is dropped whole, so a field can never land in the wrong place; the host
 * resends every state frame every few seconds, so a drop only delays a refresh.
 */
#define LINK_BAUD 460800

#define ART_N 240                 /* album art is ART_N x ART_N RGB565 little endian */
#define ART_BYTES (ART_N * ART_N * 2)
#define ART_CHUNK 1024

typedef struct {
    /* music */
    char title[192], artist[192], album[192];
    bool playing;
    int32_t pos_ms, dur_ms;
    int64_t pos_at_us;            /* esp_timer when pos_ms was received */
    char lyr[5][256];             /* prev2 prev cur next next2 */

    /* clock */
    int64_t epoch;
    int32_t tz;
    int64_t sync_us;              /* 0 = never synced */

    /* weather */
    char w_city[48], w_temp[16], w_desc[48], w_range[32];
    char w_feels[16], w_hum[16], w_wind[48];
    char w_fc[3][64];

    /* robot */
    char bot_state[12], bot_emotion[12];
    char bot_user[384], bot_reply[1024];

    /* keyboard page */
    char key_name[48], key_code[32], key_stream[192], key_count[16], key_kpm[16];

    /* system page: cpu gpu mem mem_detail down up codex_left codex_reset claude_left claude_reset */
    char sys[10][40];

    uint32_t art_ver;             /* bumps when a complete, CRC-checked cover arrived */
    uint32_t ver;                 /* bumps on any change */
    int64_t last_rx_us;
} link_state_t;

extern volatile int g_voice_level;
extern volatile int g_goto_screen;      /* debug request from the host, -1 = none */

void link_start(void);
/* copy state if it changed since *ver; returns true when copied */
bool link_get(link_state_t *out, uint32_t *ver);
/* copy the newest complete album art (ART_BYTES) into dst */
void link_get_art(uint8_t *dst);
