/* "Ice" glass dashboard: big vinyl now-playing + lyrics, clock, weather, 小耄 wave line.
 * Tap a card to open its full-screen view, tap anywhere to go back. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_timer.h"
#include "lvgl.h"
#include "ui.h"
#include "link.h"

extern const uint8_t font_start[] asm("_binary_font_ttf_start");
extern const uint8_t font_end[]   asm("_binary_font_ttf_end");

#define W 800
#define H 480

#define C_WHITE   lv_color_hex(0xFFFFFF)
#define C_TEXT    lv_color_hex(0xF4FAFF)
#define C_SUB     lv_color_hex(0xB9CDE3)
#define C_DIM     lv_color_hex(0x7D93AE)
#define C_ICE     lv_color_hex(0x9BEBFF)
#define C_FROST   lv_color_hex(0xD8F6FF)
#define C_MINT    lv_color_hex(0x7DFFD1)
#define C_GOLD    lv_color_hex(0xFFD98A)
#define C_ROSE    lv_color_hex(0xFF9EC4)

enum { F16, F20, F24, F28, F36, F48, F72, F120, F_COUNT };
static const int s_font_px[F_COUNT] = {16, 20, 24, 28, 36, 48, 72, 120};
static lv_font_t *F[F_COUNT];

enum { V_MUSIC, V_TIME, V_WEATHER, V_BOT, V_COUNT };
static lv_obj_t *s_home, *s_detail[V_COUNT];
static lv_draw_buf_t *s_bg;

static link_state_t S;
static uint32_t s_ver, s_art_ver;
static int s_angle;

/* home */
static lv_obj_t *s_mcard, *s_mpanel, *s_mtint;
static lv_obj_t *h_vinyl, *h_art, *h_state, *h_title, *h_artist, *h_album, *h_bar, *h_t0, *h_t1;
static lv_obj_t *h_lprev, *h_lcur, *h_lnext;
static lv_obj_t *h_clock, *h_date;
static lv_obj_t *h_wtemp, *h_wdesc, *h_wrange, *h_wcity;
static lv_obj_t *h_botstate, *h_bottxt;
/* details */
static lv_obj_t *d_vinyl, *d_art, *d_title, *d_artist, *d_bar, *d_t0, *d_t1, *d_lyr[5];
static lv_obj_t *d_clock, *d_sec, *d_date, *d_daybar, *d_daytxt;
static lv_obj_t *d_wcity, *d_wtemp, *d_wdesc, *d_wfeels, *d_whum, *d_wwind, *d_wrange, *d_wfc[3];
static lv_obj_t *d_botstate, *d_bot_user, *d_bot_reply;

/* wave line */
#define WAVE_N 48
typedef struct {
    lv_obj_t *glow, *line;
    lv_point_precise_t pts[WAVE_N];
    int w, h;
    float amp;
} wave_t;
static wave_t h_wave, d_wave;

/* ---------- helpers ---------- */
static void set_text(lv_obj_t *l, const char *t)
{
    if (strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t);
}

static void plain(lv_obj_t *o)
{
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
}

static lv_obj_t *label(lv_obj_t *p, int font, lv_color_t c, int x, int y, int w)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, F[font], 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_pos(l, x, y);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    }
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *bar(lv_obj_t *p, int x, int y, int w, int h)
{
    lv_obj_t *b = lv_bar_create(p);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_bar_set_range(b, 0, 1000);
    lv_obj_set_style_bg_color(b, C_WHITE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, 46, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, C_ICE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(b, C_WHITE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    return b;
}

/* frosted ice panel */
static void glass(lv_obj_t *o, int radius)
{
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, C_WHITE, 0);
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(0xBFE9FF), 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_opa(o, 44, 0);
    lv_obj_set_style_bg_grad_opa(o, 14, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, C_WHITE, 0);
    lv_obj_set_style_border_opa(o, 70, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_bg_main_opa(o, 70, LV_STATE_PRESSED);
    lv_obj_set_style_border_opa(o, 160, LV_STATE_PRESSED);
}

/* ---------- background: deep glacier gradient with soft light blobs ---------- */
static void make_bg(void)
{
    s_bg = lv_draw_buf_create(W, H, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    const struct { float x, y, r, cr, cg, cb, a; } blob[] = {
        {120, 90, 330, 80, 190, 255, 0.55f},
        {700, 430, 360, 120, 110, 255, 0.45f},
        {640, 40, 230, 70, 230, 215, 0.30f},
        {300, 470, 260, 40, 140, 220, 0.35f},
    };
    for (int y = 0; y < H; y++) {
        uint16_t *row = (uint16_t *)(s_bg->data + y * s_bg->header.stride);
        float t = y / (float)H;
        for (int x = 0; x < W; x++) {
            float r = 8 + 10 * t, g = 18 + 22 * t, b = 38 + 34 * t;
            for (int k = 0; k < 4; k++) {
                float dx = x - blob[k].x, dy = y - blob[k].y;
                float d = (dx * dx + dy * dy) / (blob[k].r * blob[k].r);
                if (d < 1.0f) {
                    float a = blob[k].a * (1 - d) * (1 - d);
                    r += (blob[k].cr - r) * a;
                    g += (blob[k].cg - g) * a;
                    b += (blob[k].cb - b) * a;
                }
            }
            /* tiny ordered dither so gradients don't band in RGB565 */
            float n = (((x * 7 + y * 13) & 7) - 3.5f) * 0.6f;
            int R = r + n, G = g + n, B = b + n;
            R = R < 0 ? 0 : R > 255 ? 255 : R;
            G = G < 0 ? 0 : G > 255 ? 255 : G;
            B = B < 0 ? 0 : B > 255 ? 255 : B;
            row[x] = ((R & 0xF8) << 8) | ((G & 0xFC) << 3) | (B >> 3);
        }
    }
}

static lv_obj_t *screen(void)
{
    lv_obj_t *s = lv_obj_create(NULL);
    plain(s);
    lv_obj_set_style_bg_color(s, lv_color_hex(0x0A1626), 0);
    lv_obj_t *bg = lv_image_create(s);
    lv_image_set_src(bg, s_bg);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_remove_flag(bg, LV_OBJ_FLAG_CLICKABLE);
    return s;
}

/* ---------- vinyl ---------- */
static lv_draw_buf_t *make_vinyl(int D)
{
    lv_draw_buf_t *db = lv_draw_buf_create(D, D, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
    float R = D / 2.0f;
    for (int y = 0; y < D; y++) {
        uint8_t *row = db->data + y * db->header.stride;
        for (int x = 0; x < D; x++) {
            float dx = x + 0.5f - R, dy = y + 0.5f - R;
            float r = sqrtf(dx * dx + dy * dy) / R;
            float th = atan2f(dy, dx);
            float rr, gg, bb, a = 1.0f;
            if (r > 1.0f) {
                a = 1.0f - (r - 1.0f) * R;
                if (a < 0) a = 0;
            }
            if (r < 0.37f) {
                float t = r / 0.37f;
                rr = 150 + 80 * (1 - t); gg = 215 + 30 * (1 - t); bb = 245;
                if (r > 0.355f) { rr *= 0.5f; gg *= 0.5f; bb *= 0.5f; }
            } else {
                float g = 16 + 6 * sinf(r * 260.0f) + (fmodf(r * 37.0f, 1.0f) < 0.07f ? 9 : 0);
                if (r > 0.975f) g = 40;
                float sheen = powf(fabsf(cosf(th - 0.8f)), 14.0f) * 70.0f * r;
                rr = g + sheen * 0.85f; gg = g + sheen * 0.95f; bb = g + 6 + sheen;
            }
            uint8_t *p = row + x * 4;
            p[0] = bb > 255 ? 255 : bb;
            p[1] = gg > 255 ? 255 : gg;
            p[2] = rr > 255 ? 255 : rr;
            p[3] = (uint8_t)(a * 255);
        }
    }
    return db;
}

static lv_obj_t *vinyl(lv_obj_t *p, int D, int x, int y)
{
    lv_obj_t *img = lv_image_create(p);
    lv_image_set_src(img, make_vinyl(D));
    lv_obj_set_pos(img, x, y);
    lv_image_set_pivot(img, D / 2, D / 2);
    lv_image_set_antialias(img, true);
    return img;
}

static lv_obj_t *art_img(lv_obj_t *p, int D, int vx, int vy, int vd)
{
    lv_obj_t *img = lv_image_create(p);
    lv_draw_buf_t *db = lv_draw_buf_create(D, D, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
    lv_draw_buf_clear(db, NULL);
    lv_image_set_src(img, db);
    lv_obj_set_pos(img, vx + (vd - D) / 2, vy + (vd - D) / 2);
    lv_image_set_pivot(img, D / 2, D / 2);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    return img;
}

static void art_fill(lv_obj_t *img, const uint8_t *rgb565)
{
    lv_draw_buf_t *db = (lv_draw_buf_t *)lv_image_get_src(img);
    int D = db->header.w;
    float R = D / 2.0f;
    for (int y = 0; y < D; y++) {
        uint8_t *row = db->data + y * db->header.stride;
        for (int x = 0; x < D; x++) {
            float dx = x + 0.5f - R, dy = y + 0.5f - R;
            float r = sqrtf(dx * dx + dy * dy);
            int sx = x * ART_N / D, sy = y * ART_N / D;
            uint16_t c = rgb565[(sy * ART_N + sx) * 2] | (rgb565[(sy * ART_N + sx) * 2 + 1] << 8);
            uint8_t *px = row + x * 4;
            px[0] = (c << 3) & 0xF8;
            px[1] = (c >> 3) & 0xFC;
            px[2] = (c >> 8) & 0xF8;
            float a = R - r, h = r - R * 0.08f;
            if (h < a) a = h;
            a = a < 0 ? 0 : (a > 1 ? 1 : a);
            px[3] = (uint8_t)(a * 255);
        }
    }
    lv_image_cache_drop(db);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(img);
}

static void tonearm(lv_obj_t *p, int x, int y, int len)
{
    static lv_point_precise_t pts[2][3];
    static int n;
    lv_point_precise_t *pt = pts[n++ & 1];
    pt[0].x = 0; pt[0].y = 0;
    pt[1].x = -len * 0.12f; pt[1].y = len * 0.62f;
    pt[2].x = -len * 0.38f; pt[2].y = len * 0.98f;
    lv_obj_t *l = lv_line_create(p);
    lv_line_set_points(l, pt, 3);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_line_width(l, len / 30 + 3, 0);
    lv_obj_set_style_line_color(l, lv_color_hex(0xF2F5F8), 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    int bs = len / 4;
    lv_obj_t *base = lv_obj_create(p);
    plain(base);
    lv_obj_set_size(base, bs, bs);
    lv_obj_set_pos(base, x - bs / 2, y - bs / 2);
    lv_obj_set_style_radius(base, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(base, lv_color_hex(0xE8ECF0), 0);
    lv_obj_set_style_bg_grad_color(base, lv_color_hex(0x9AA3AD), 0);
    lv_obj_set_style_bg_grad_dir(base, LV_GRAD_DIR_VER, 0);
    lv_obj_remove_flag(base, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *head = lv_obj_create(p);
    plain(head);
    lv_obj_set_size(head, len / 9 + 4, len / 6);
    lv_obj_set_pos(head, x + pt[2].x - len / 18 - 2, y + pt[2].y - len / 14);
    lv_obj_set_style_radius(head, 3, 0);
    lv_obj_set_style_bg_color(head, lv_color_hex(0xDDE3E9), 0);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_CLICKABLE);
}

/* tint music card from the cover's average colour (warm cover -> warm card) */
static void tint_from_art(const uint8_t *rgb565)
{
    uint32_t r = 0, g = 0, b = 0, n = ART_N * ART_N;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t c = rgb565[i * 2] | (rgb565[i * 2 + 1] << 8);
        r += (c >> 8) & 0xF8; g += (c >> 3) & 0xFC; b += (c << 3) & 0xF8;
    }
    lv_color_t avg = lv_color_make(r / n, g / n, b / n);
    lv_color_t top = lv_color_mix(avg, lv_color_white(), 200);
    lv_color_t bot = lv_color_mix(avg, lv_color_black(), 150);
    lv_obj_t *objs[] = {s_mcard, s_mtint};
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_bg_color(objs[i], top, 0);
        lv_obj_set_style_bg_grad_color(objs[i], bot, 0);
        lv_obj_set_style_bg_main_opa(objs[i], i ? 150 : 120, 0);
        lv_obj_set_style_bg_grad_opa(objs[i], i ? 220 : 170, 0);
    }
}

/* ---------- wave line ---------- */
static lv_obj_t *wave_line(lv_obj_t *p, wave_t *wv, int width, int opa)
{
    lv_obj_t *l = lv_line_create(p);
    lv_line_set_points(l, wv->pts, WAVE_N);
    lv_obj_set_style_line_width(l, width, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_obj_set_style_line_color(l, C_ICE, 0);
    lv_obj_set_style_line_opa(l, opa, 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static void wave_create(wave_t *wv, lv_obj_t *p, int x, int y, int w, int h, int thick)
{
    wv->w = w;
    wv->h = h;
    for (int i = 0; i < WAVE_N; i++) {
        wv->pts[i].x = i * (w - 1) / (WAVE_N - 1);
        wv->pts[i].y = h / 2;
    }
    lv_obj_t *box = lv_obj_create(p);
    plain(box);
    lv_obj_set_style_bg_opa(box, 0, 0);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    wv->glow = wave_line(box, wv, thick * 3, 50);
    wv->line = wave_line(box, wv, thick, LV_OPA_COVER);
}

static int bot_state(void)
{
    if (!strcmp(S.bot_state, "listen")) return 1;
    if (!strcmp(S.bot_state, "think")) return 2;
    if (!strcmp(S.bot_state, "speak")) return 3;
    return 0;
}

/* idle: slow breathing; listen: follows your voice; think: fast ripple; speak: lively bursts */
static void wave_update(wave_t *wv, float t)
{
    int st = bot_state();
    float lvl = g_voice_level / 100.0f;
    float half = wv->h / 2.0f - 4;
    float target, f1, f2, sp;
    lv_color_t c;
    switch (st) {
    case 1:  target = 0.10f + lvl * 0.9f; f1 = 2.2f; f2 = 5.1f; sp = 6; c = C_MINT; break;
    case 2:  target = 0.28f; f1 = 4.0f; f2 = 7.0f; sp = 11; c = C_GOLD; break;
    case 3:  target = 0.25f + 0.55f * fabsf(sinf(t * 7.3f) * sinf(t * 2.9f + 1)); f1 = 3.0f; f2 = 6.2f; sp = 8; c = C_ROSE; break;
    default: target = 0.05f + 0.03f * sinf(t * 1.3f) + lvl * 0.5f; f1 = 1.4f; f2 = 3.3f; sp = 1.6f; c = C_ICE; break;
    }
    /* mood colours/shapes the line while idle and while talking */
    if (st == 0 || st == 3) {
        const char *e = S.bot_emotion;
        float k = st == 3 ? 1.0f : 0.5f;
        if (!strcmp(e, "happy"))          { c = C_ROSE; f1 += 0.8f; sp *= 1.4f; target += 0.06f * k; }
        else if (!strcmp(e, "playful"))   { c = C_MINT; f2 += 2; sp *= 1.8f; target += 0.08f * k; }
        else if (!strcmp(e, "sad"))       { c = lv_color_hex(0x6F8CFF); f1 = 0.8f; sp *= 0.5f; target *= 0.6f; }
        else if (!strcmp(e, "angry"))     { c = lv_color_hex(0xFF6A5C); f1 = 6; f2 = 11; sp *= 2.2f; target += 0.15f * k; }
        else if (!strcmp(e, "surprised")) { c = C_GOLD; f1 = 2; f2 = 9; target += 0.2f * k * fabsf(sinf(t * 3)); }
    }
    wv->amp += (target - wv->amp) * 0.25f;
    for (int i = 0; i < WAVE_N; i++) {
        float u = i / (float)(WAVE_N - 1);
        float env = sinf(u * 3.14159f);
        float v = 0.7f * sinf(6.2832f * f1 * u + t * sp) + 0.3f * sinf(6.2832f * f2 * u - t * sp * 1.4f);
        wv->pts[i].y = wv->h / 2.0f + v * env * wv->amp * half;
    }
    lv_obj_set_style_line_color(wv->line, c, 0);
    lv_obj_set_style_line_color(wv->glow, c, 0);
    lv_line_set_points(wv->line, wv->pts, WAVE_N);
    lv_line_set_points(wv->glow, wv->pts, WAVE_N);
}

/* ---------- navigation ---------- */
static void open_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    lv_screen_load_anim(s_detail[v], LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
}

static void back_cb(lv_event_t *e)
{
    lv_screen_load_anim(s_home, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
}

static lv_obj_t *card(int v, int x, int y, int w, int h)
{
    lv_obj_t *c = lv_obj_create(s_home);
    plain(c);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    glass(c, 28);
    lv_obj_add_event_cb(c, open_cb, LV_EVENT_CLICKED, (void *)(intptr_t)v);
    return c;
}

static lv_obj_t *detail(int v, const char *title)
{
    lv_obj_t *s = screen();
    s_detail[v] = s;
    lv_obj_add_event_cb(s, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *b = label(s, F20, C_SUB, 28, 22, 0);
    lv_label_set_text(b, "← 返回");
    lv_obj_t *t = label(s, F24, C_TEXT, 0, 20, 0);
    lv_label_set_text(t, title);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 20);
    return s;
}

static lv_obj_t *panel(lv_obj_t *p, int x, int y, int w, int h)
{
    lv_obj_t *o = lv_obj_create(p);
    plain(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    glass(o, 28);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

/* ---------- build ---------- */
static void build_home(void)
{
    s_home = screen();
    lv_obj_t *c;

    /* music: whole left side */
    c = card(V_MUSIC, 16, 16, 500, 448);
    s_mcard = c;
    h_vinyl = vinyl(c, 210, 18, 22);
    h_art = art_img(c, 150, 18, 22, 210);
    tonearm(c, 214, 20, 120);
    h_state = label(c, F16, C_MINT, 252, 30, 226);
    h_title = label(c, F28, C_TEXT, 252, 62, 226);
    lv_label_set_long_mode(h_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    h_artist = label(c, F20, C_SUB, 252, 104, 226);
    h_album = label(c, F16, C_DIM, 252, 136, 226);
    h_bar = bar(c, 252, 186, 226, 6);
    h_t0 = label(c, F16, C_DIM, 252, 200, 100);
    h_t1 = label(c, F16, C_DIM, 378, 200, 100);
    lv_obj_set_style_text_align(h_t1, LV_TEXT_ALIGN_RIGHT, 0);
    h_lprev = label(c, F20, C_DIM, 24, 262, 452);
    h_lcur = label(c, F36, C_WHITE, 24, 300, 452);
    lv_label_set_long_mode(h_lcur, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(h_lcur, 96);
    h_lnext = label(c, F20, C_SUB, 24, 400, 452);

    /* clock */
    c = card(V_TIME, 532, 16, 252, 156);
    h_clock = label(c, F72, C_WHITE, 22, 14, 0);
    h_date = label(c, F20, C_SUB, 26, 110, 210);

    /* weather */
    c = card(V_WEATHER, 532, 188, 252, 140);
    h_wcity = label(c, F16, C_SUB, 24, 16, 200);
    h_wtemp = label(c, F48, C_WHITE, 22, 46, 120);
    h_wdesc = label(c, F20, C_FROST, 132, 54, 110);
    h_wrange = label(c, F16, C_SUB, 132, 86, 110);

    /* robot: just the line */
    c = card(V_BOT, 532, 344, 252, 120);
    h_botstate = label(c, F16, C_SUB, 24, 14, 200);
    wave_create(&h_wave, c, 16, 34, 220, 40, 3);
    h_bottxt = label(c, F16, C_TEXT, 24, 76, 206);
    lv_obj_set_height(h_bottxt, 40);
}

static void build_details(void)
{
    lv_obj_t *s, *p;

    /* music */
    s = detail(V_MUSIC, "正在播放");
    s_mtint = lv_obj_create(s);
    plain(s_mtint);
    lv_obj_set_size(s_mtint, W, H);
    lv_obj_set_style_bg_opa(s_mtint, 0, 0);
    lv_obj_remove_flag(s_mtint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_to_index(s_mtint, 1);
    d_vinyl = vinyl(s, 320, 30, 110);
    d_art = art_img(s, 230, 30, 110, 320);
    tonearm(s, 352, 96, 190);
    p = panel(s, 384, 70, 392, 394);
    d_title = label(p, F36, C_WHITE, 24, 20, 344);
    lv_label_set_long_mode(d_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    d_artist = label(p, F20, C_SUB, 24, 70, 344);
    d_bar = bar(p, 24, 110, 344, 6);
    d_t0 = label(p, F16, C_DIM, 24, 124, 120);
    d_t1 = label(p, F16, C_DIM, 248, 124, 120);
    lv_obj_set_style_text_align(d_t1, LV_TEXT_ALIGN_RIGHT, 0);
    static const int ly[5] = {160, 194, 232, 314, 350};
    static const int lf[5] = {F16, F20, F28, F20, F16};
    for (int i = 0; i < 5; i++) {
        d_lyr[i] = label(p, lf[i], i == 2 ? C_WHITE : (i == 1 || i == 3 ? C_SUB : C_DIM), 24, ly[i], 344);
    }
    lv_label_set_long_mode(d_lyr[2], LV_LABEL_LONG_WRAP);
    lv_obj_set_height(d_lyr[2], 76);

    /* clock */
    s = detail(V_TIME, "时间");
    p = panel(s, 60, 80, 680, 370);
    d_clock = label(p, F120, C_WHITE, 0, 30, 0);
    lv_obj_align(d_clock, LV_ALIGN_TOP_MID, -36, 30);
    d_sec = label(p, F48, C_ICE, 0, 0, 0);
    d_date = label(p, F36, C_SUB, 0, 190, 0);
    d_daybar = bar(p, 80, 270, 520, 8);
    d_daytxt = label(p, F20, C_DIM, 80, 292, 520);
    lv_obj_set_style_text_align(d_daytxt, LV_TEXT_ALIGN_CENTER, 0);

    /* weather */
    s = detail(V_WEATHER, "天气");
    p = panel(s, 40, 80, 720, 230);
    d_wcity = label(p, F24, C_SUB, 32, 22, 300);
    d_wtemp = label(p, F120, C_WHITE, 26, 50, 320);
    d_wdesc = label(p, F36, C_FROST, 360, 30, 330);
    d_wfeels = label(p, F24, C_SUB, 360, 90, 330);
    d_whum = label(p, F24, C_SUB, 360, 126, 330);
    d_wwind = label(p, F24, C_SUB, 360, 162, 330);
    d_wrange = label(p, F24, C_SUB, 32, 186, 300);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = panel(s, 40 + i * 245, 326, 230, 130);
        d_wfc[i] = label(b, F24, C_TEXT, 14, 0, 202);
        lv_label_set_long_mode(d_wfc[i], LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(d_wfc[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(d_wfc[i], LV_ALIGN_LEFT_MID, 14, 0);
    }

    /* robot */
    s = detail(V_BOT, "小耄");
    p = panel(s, 40, 80, 720, 150);
    wave_create(&d_wave, p, 30, 20, 660, 110, 6);
    d_botstate = label(s, F20, C_SUB, 40, 244, 720);
    lv_obj_set_style_text_align(d_botstate, LV_TEXT_ALIGN_CENTER, 0);
    p = panel(s, 40, 284, 720, 180);
    d_bot_user = label(p, F20, C_SUB, 26, 18, 668);
    d_bot_reply = label(p, F28, C_WHITE, 26, 56, 668);
    lv_label_set_long_mode(d_bot_reply, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(d_bot_reply, 112);
}

/* ---------- refresh ---------- */
static void mmss(char *o, int n, int s)
{
    if (s < 0) s = 0;
    snprintf(o, n, "%d:%02d", s / 60, s % 60);
}

static void refresh_data(void)
{
    char buf[320];

    set_text(h_title, S.title);
    set_text(d_title, S.title);
    set_text(h_artist, S.artist);
    set_text(d_artist, S.artist);
    set_text(h_album, S.album);
    const char *st = S.dur ? (S.playing ? "● 播放中" : "Ⅱ 已暂停") : "";
    set_text(h_state, st);
    lv_obj_set_style_text_color(h_state, S.playing ? C_MINT : C_SUB, 0);
    int prog = S.dur > 0 ? S.pos * 1000 / S.dur : 0;
    lv_bar_set_value(h_bar, prog, LV_ANIM_OFF);
    lv_bar_set_value(d_bar, prog, LV_ANIM_OFF);
    mmss(buf, sizeof(buf), S.pos);
    set_text(h_t0, S.dur ? buf : "");
    set_text(d_t0, S.dur ? buf : "");
    mmss(buf, sizeof(buf), S.dur);
    set_text(h_t1, S.dur ? buf : "");
    set_text(d_t1, S.dur ? buf : "");
    set_text(h_lprev, S.lyr[1]);
    set_text(h_lcur, S.lyr[2][0] ? S.lyr[2] : "♪");
    set_text(h_lnext, S.lyr[3]);
    for (int i = 0; i < 5; i++) set_text(d_lyr[i], i == 2 && !S.lyr[2][0] ? "♪" : S.lyr[i]);

    set_text(h_wcity, S.w_city[0] ? S.w_city : "天气");
    set_text(d_wcity, S.w_city);
    set_text(h_wtemp, S.w_temp[0] ? S.w_temp : "--");
    set_text(d_wtemp, S.w_temp[0] ? S.w_temp : "--");
    set_text(h_wdesc, S.w_desc);
    set_text(d_wdesc, S.w_desc);
    set_text(h_wrange, S.w_range);
    snprintf(buf, sizeof(buf), "今日 %s", S.w_range);
    set_text(d_wrange, S.w_range[0] ? buf : "");
    snprintf(buf, sizeof(buf), "体感 %s", S.w_feels);
    set_text(d_wfeels, S.w_feels[0] ? buf : "");
    snprintf(buf, sizeof(buf), "湿度 %s", S.w_hum);
    set_text(d_whum, S.w_hum[0] ? buf : "");
    set_text(d_wwind, S.w_wind);
    for (int i = 0; i < 3; i++) set_text(d_wfc[i], S.w_fc[i]);

    static const char *hs[] = {"小耄", "小耄 · 在听", "小耄 · 思考中", "小耄 · 说话中"};
    static const char *ds[] = {"说「你好耄耋」叫我", "在听…", "思考中…", "说话中"};
    int b = bot_state();
    set_text(h_botstate, hs[b]);
    set_text(d_botstate, ds[b]);
    if (S.bot_user[0]) snprintf(buf, sizeof(buf), "你：%s", S.bot_user);
    else buf[0] = 0;
    set_text(d_bot_user, buf);
    set_text(d_bot_reply, S.bot_reply);
    set_text(h_bottxt, b == 1 ? "我在听…" : b == 2 ? "让我想想…" : S.bot_reply);

    if (S.art_ver != s_art_ver) {
        s_art_ver = S.art_ver;
        art_fill(h_art, S.art);
        art_fill(d_art, S.art);
        tint_from_art(S.art);
    }
}

static void refresh_clock(void)
{
    static const char *wd[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    char buf[64];
    if (!S.sync_us) {
        set_text(h_clock, "--:--");
        set_text(d_clock, "--:--");
        set_text(h_date, "等待同步");
        return;
    }
    time_t t = S.epoch + S.tz + (esp_timer_get_time() - S.sync_us) / 1000000;
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    set_text(h_clock, buf);
    set_text(d_clock, buf);
    snprintf(buf, sizeof(buf), "%02d", tm.tm_sec);
    set_text(d_sec, buf);
    lv_obj_align_to(d_sec, d_clock, LV_ALIGN_OUT_RIGHT_BOTTOM, 14, -24);
    snprintf(buf, sizeof(buf), "%d年%d月%d日  %s", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, wd[tm.tm_wday]);
    set_text(d_date, buf);
    lv_obj_align(d_date, LV_ALIGN_TOP_MID, 0, 190);
    snprintf(buf, sizeof(buf), "%d月%d日 %s", tm.tm_mon + 1, tm.tm_mday, wd[tm.tm_wday]);
    set_text(h_date, buf);
    int secs = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
    lv_bar_set_value(d_daybar, secs * 1000 / 86400, LV_ANIM_OFF);
    snprintf(buf, sizeof(buf), "今天已经过去 %d%%", secs * 100 / 86400);
    set_text(d_daytxt, buf);
}

static void data_timer(lv_timer_t *t)
{
    if (link_get(&S, &s_ver)) refresh_data();
    refresh_clock();
}

static void anim_timer(lv_timer_t *t)
{
    lv_obj_t *scr = lv_screen_active();
    float now = lv_tick_get() / 1000.0f;
    if (S.playing) s_angle = (s_angle + 20) % 3600;
    if (scr == s_home) {
        if (S.playing) {
            lv_image_set_rotation(h_vinyl, s_angle);
            lv_image_set_rotation(h_art, s_angle);
        }
        wave_update(&h_wave, now);
    } else if (scr == s_detail[V_MUSIC] && S.playing) {
        lv_image_set_rotation(d_vinyl, s_angle);
        lv_image_set_rotation(d_art, s_angle);
    } else if (scr == s_detail[V_BOT]) {
        wave_update(&d_wave, now);
    }
}

void ui_init(void)
{
    for (int i = 0; i < F_COUNT; i++) {
        F[i] = lv_tiny_ttf_create_data(font_start, font_end - font_start, s_font_px[i]);
    }
    make_bg();
    build_home();
    build_details();
    lv_screen_load(s_home);
    link_get(&S, &s_ver);
    refresh_data();
    lv_timer_create(data_timer, 200, NULL);
    lv_timer_create(anim_timer, 40, NULL);
}
