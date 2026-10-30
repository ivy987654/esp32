/* 黑胶仪表盘 UI: home = music card + clock / weather / 小耄 cards, tap a card for its full screen page.
 *
 * Rules that keep the panel clean (see README "稳定性"):
 *  - every text box has a fixed size; text is fitted by picking a smaller font or wrapping,
 *    never scrolled, so nothing animates and nothing spills out of its box;
 *  - all fills are opaque flat colours (no opacity, gradients or shadows);
 *  - the vinyl + cover is one opaque RGB565 image, recomposed only when the cover changes;
 *  - the only continuous animation is 小耄's line, a few hundred pixels per frame.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "ui.h"
#include "link.h"

/* flat Morandi palette */
#define C_BG     lv_color_hex(0xE9E4DD)   /* oat page */
#define C_INK    lv_color_hex(0x5E5750)   /* text on the page */
#define C_SAGE   lv_color_hex(0x8C9A8E)   /* music */
#define C_DUSK   lv_color_hex(0x96A5B3)   /* clock */
#define C_TAUPE  lv_color_hex(0xB8A396)   /* weather */
#define C_MAUVE  lv_color_hex(0xA0939F)   /* robot */
#define C_WHITE  lv_color_hex(0xFFFFFF)
#define C_SUB    lv_color_hex(0xF1EBE4)
#define C_DIM    lv_color_hex(0xDDD3C9)
#define C_TRACK  lv_color_hex(0x75837A)
#define C_MINT   lv_color_hex(0xD4ECD9)

/* ---------- fonts ---------- */
enum { F20, F24, F28, F32, F40, F48, F56, F72, F96, F120, F_COUNT };
static const int s_px[F_COUNT] = {20, 24, 28, 32, 40, 48, 56, 72, 96, 120};
static lv_font_t *F[F_COUNT];

/* ---------- state ---------- */
static link_state_t S;
static uint32_t s_ver, s_art_ver;
static uint8_t *s_art;              /* local copy of the cover, ART_N x ART_N RGB565 */
static bool s_have_art;

enum { V_MUSIC, V_TIME, V_WEATHER, V_BOT, V_COUNT };
static lv_obj_t *s_home, *s_keys, *s_sys, *s_page[V_COUNT];
#define N_SCREENS 3

/* ---------- small helpers ---------- */
static lv_obj_t *box(lv_obj_t *p, int x, int y, int w, int h, lv_color_t c, int radius)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

/* a text box of fixed size; fonts[] = sizes to try, biggest first */
typedef struct {
    lv_obj_t *l;
    uint8_t fonts[4];       /* copied: the FONTS() literal only lives while building */
    int nf;
    uint32_t hash;          /* of the text last set (the label's own text may carry "...") */
} tbox_t;

static tbox_t tbox(lv_obj_t *p, int x, int y, int w, int h, lv_color_t c, const uint8_t *fonts, int nf)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_remove_style_all(l);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, h);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_style_text_font(l, F[fonts[0]], 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, "");
    tbox_t t = {.l = l, .nf = nf > 4 ? 4 : nf};
    memcpy(t.fonts, fonts, t.nf);
    return t;
}
#define FONTS(...) (const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__})

static void tbox_align(tbox_t *t, lv_text_align_t a)
{
    lv_obj_set_style_text_align(t->l, a, 0);
}

/* biggest font whose wrapped text fits the box; if none fits, smallest font + "..." */
static void tset(tbox_t *t, const char *txt)
{
    lv_obj_t *l = t->l;
    uint32_t hsh = 2166136261u;
    for (const char *p = txt; *p; p++) hsh = (hsh ^ (uint8_t)*p) * 16777619u;
    hsh |= 1;
    if (hsh == t->hash) return;
    t->hash = hsh;
    int w = lv_obj_get_width(l), h = lv_obj_get_height(l);
    int pick = t->fonts[t->nf - 1];
    bool fits = false;
    for (int i = 0; i < t->nf; i++) {
        lv_point_t sz;
        lv_text_get_size(&sz, txt, F[t->fonts[i]], 0, 0, w, LV_TEXT_FLAG_NONE);
        if (sz.y <= h) {
            pick = t->fonts[i];
            fits = true;
            break;
        }
    }
    /* repaint a margin around the box too: TTF ink may poke past the line box */
    lv_area_t a;
    lv_obj_get_coords(l, &a);
    lv_area_increase(&a, 6, 10);
    lv_obj_invalidate_area(lv_obj_get_parent(l), &a);
    lv_obj_set_style_text_font(l, F[pick], 0);
    lv_label_set_long_mode(l, fits ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
    lv_label_set_text(l, txt);
}

/* height the text actually takes in its box */
static int tbox_used_h(tbox_t *t)
{
    lv_point_t sz;
    lv_text_get_size(&sz, lv_label_get_text(t->l), lv_obj_get_style_text_font(t->l, 0), 0,
                     lv_obj_get_style_text_line_space(t->l, 0), lv_obj_get_width(t->l), LV_TEXT_FLAG_NONE);
    int h = lv_obj_get_height(t->l);
    return sz.y < h ? sz.y : h;
}

/* keep the "next" lyric right under the current one, whatever height that one took */
static void follow(tbox_t *below, tbox_t *above, int gap)
{
    int y = lv_obj_get_y(above->l) + tbox_used_h(above) + gap;
    if (lv_obj_get_y(below->l) != y) {
        lv_obj_invalidate(below->l);
        lv_obj_set_y(below->l, y);
    }
}

static void tsetf(tbox_t *t, const char *fmt, ...)
{
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    tset(t, b);
}

/* progress bar = track box + fill box */
typedef struct {
    lv_obj_t *track, *fill;
    int w, v;
} pbar_t;

static pbar_t pbar(lv_obj_t *p, int x, int y, int w, int h, lv_color_t track, lv_color_t fill)
{
    pbar_t b = {.w = w, .v = -1};
    b.track = box(p, x, y, w, h, track, h / 2);
    b.fill = box(b.track, 0, 0, h, h, fill, h / 2);
    return b;
}

static void pbar_set(pbar_t *b, int permille)
{
    if (permille < 0) permille = 0;
    if (permille > 1000) permille = 1000;
    int h = lv_obj_get_height(b->track);
    int w = h + (b->w - h) * permille / 1000;
    if (w == b->v) return;
    b->v = w;
    lv_obj_set_width(b->fill, w);
}

/* ---------- vinyl with the cover baked in (one opaque image) ---------- */
/* the panel's pixels are ~6% wider than tall (95.0 x 53.9 mm for 800x480),
 * so round things are drawn narrower in pixels to look round on the glass */
#define PX_ASPECT 0.944f

typedef struct {
    lv_obj_t *img;
    lv_image_dsc_t dsc;
    uint8_t *buf;
    int D, Dw;
    lv_color_t bg;
} vinyl_t;

static vinyl_t s_vh, s_vd;          /* home, detail */

static inline uint16_t rgb565(int r, int g, int b)
{
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3;
}

static void art_sample(float u, float v, int *r, int *g, int *b)
{
    /* bilinear, u/v in 0..1 */
    float x = u * (ART_N - 1), y = v * (ART_N - 1);
    int x0 = (int)x, y0 = (int)y;
    if (x0 >= ART_N - 1) x0 = ART_N - 2;
    if (y0 >= ART_N - 1) y0 = ART_N - 2;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    float fx = x - x0, fy = y - y0;
    float acc[3] = {0};
    for (int k = 0; k < 4; k++) {
        int xx = x0 + (k & 1), yy = y0 + (k >> 1);
        float wgt = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
        const uint8_t *p = s_art + (yy * ART_N + xx) * 2;
        uint16_t c = p[0] | p[1] << 8;
        acc[0] += wgt * ((c >> 8) & 0xF8);
        acc[1] += wgt * ((c >> 3) & 0xFC);
        acc[2] += wgt * ((c << 3) & 0xF8);
    }
    *r = (int)acc[0];
    *g = (int)acc[1];
    *b = (int)acc[2];
}

static void vinyl_draw(vinyl_t *v)
{
    int D = v->D, Dw = v->Dw;
    float R = D / 2.0f, cover = 0.68f;
    lv_color32_t bg = lv_color_to_32(v->bg, 255);
    uint16_t *px = (uint16_t *)v->buf;
    for (int y = 0; y < D; y++) {
        for (int x = 0; x < Dw; x++) {
            float dx = (x + 0.5f - Dw / 2.0f) / PX_ASPECT, dy = y + 0.5f - R;
            float d = sqrtf(dx * dx + dy * dy);
            float r = d / R;
            int cr, cg, cb;
            if (r < cover) {
                if (s_have_art) {
                    float u = 0.5f + dx / (2 * cover * R), w = 0.5f + dy / (2 * cover * R);
                    art_sample(u, w, &cr, &cg, &cb);
                } else {
                    cr = 0xC9; cg = 0xBF; cb = 0xB4;   /* plain label until a cover arrives */
                }
                if (r < 0.045f) { cr = bg.red; cg = bg.green; cb = bg.blue; }        /* spindle hole */
                else if (r < 0.06f) { cr = cg = cb = 30; }
                /* soften the cover edge into the groove ring */
                float e = (cover - r) * R;
                if (e < 1.0f) {
                    cr = cr * e + 22 * (1 - e); cg = cg * e + 22 * (1 - e); cb = cb * e + 24 * (1 - e);
                }
            } else {
                float g = 24 + 5 * sinf(r * 210.0f);
                if (fmodf(r * 30.0f, 1.0f) < 0.08f) g += 7;
                if (r > 0.975f) g = 36;
                float th = atan2f(dy, dx);
                float sheen = powf(fabsf(cosf(th - 0.8f)), 16.0f) * 46.0f * r;
                cr = g + sheen * 0.9f; cg = g + sheen; cb = g + 4 + sheen;
                /* anti-aliased outer edge onto the background */
                float a = R - d;
                if (a < 1.0f) {
                    if (a < 0) a = 0;
                    cr = cr * a + bg.red * (1 - a);
                    cg = cg * a + bg.green * (1 - a);
                    cb = cb * a + bg.blue * (1 - a);
                }
            }
            px[y * Dw + x] = rgb565(cr, cg, cb);
        }
    }
    lv_image_cache_drop(&v->dsc);
    lv_obj_invalidate(v->img);
}

static void vinyl_create(vinyl_t *v, lv_obj_t *p, int x, int y, int D, lv_color_t bg)
{
    v->D = D;
    v->Dw = (int)(D * PX_ASPECT + 0.5f);
    v->bg = bg;
    v->buf = heap_caps_calloc(1, v->Dw * D * 2, MALLOC_CAP_SPIRAM);
    v->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    v->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    v->dsc.header.w = v->Dw;
    v->dsc.header.h = D;
    v->dsc.header.stride = v->Dw * 2;
    v->dsc.data_size = v->Dw * D * 2;
    v->dsc.data = v->buf;
    v->img = lv_image_create(p);
    lv_obj_remove_style_all(v->img);
    lv_obj_remove_flag(v->img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(v->img, x, y);
    lv_image_set_src(v->img, &v->dsc);
    vinyl_draw(v);
}

/* ---------- 小耄's line ---------- */
#define WAVE_N 40
typedef struct {
    lv_obj_t *line;
    lv_point_precise_t pts[WAVE_N];
    int w, h;
    float amp;
    uint32_t col;
} wave_t;
static wave_t s_wh, s_wd;

static void wave_create(wave_t *wv, lv_obj_t *p, int x, int y, int w, int h, int thick)
{
    wv->w = w;
    wv->h = h;
    for (int i = 0; i < WAVE_N; i++) {
        wv->pts[i].x = thick + i * (w - 1 - 2 * thick) / (WAVE_N - 1);
        wv->pts[i].y = h / 2;
    }
    lv_obj_t *l = lv_line_create(p);
    lv_obj_remove_style_all(l);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, h);
    lv_obj_set_style_line_width(l, thick, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_obj_set_style_line_color(l, C_WHITE, 0);
    lv_obj_set_style_line_opa(l, LV_OPA_COVER, 0);
    lv_line_set_points(l, wv->pts, WAVE_N);
    wv->line = l;
}

static int bot_state(void)
{
    if (!strcmp(S.bot_state, "listen")) return 1;
    if (!strcmp(S.bot_state, "think")) return 2;
    if (!strcmp(S.bot_state, "speak")) return 3;
    return 0;
}

/* idle: slow breathing; listen: follows your voice; think: quick ripple; speak: lively.
 * The mood of the last reply tints and shapes it. */
static void wave_update(wave_t *wv, float t)
{
    int st = bot_state();
    float lvl = g_voice_level / 100.0f;
    float half = wv->h / 2.0f - 4;
    float target, f1, f2, sp;
    uint32_t c;
    switch (st) {
    case 1:  target = 0.12f + lvl * 0.88f; f1 = 2.2f; f2 = 5.1f; sp = 6; c = 0xD4ECD9; break;
    case 2:  target = 0.30f; f1 = 4.0f; f2 = 7.0f; sp = 11; c = 0xF1DFAE; break;
    case 3:  target = 0.25f + 0.55f * fabsf(sinf(t * 7.3f) * sinf(t * 2.9f + 1)); f1 = 3.0f; f2 = 6.2f; sp = 8; c = 0xF6D3D3; break;
    default: target = 0.06f + 0.04f * sinf(t * 1.3f); f1 = 1.4f; f2 = 3.3f; sp = 1.6f; c = 0xFFFFFF; break;
    }
    if (st == 0 || st == 3) {
        const char *e = S.bot_emotion;
        float k = st == 3 ? 1.0f : 0.5f;
        if (!strcmp(e, "happy"))          { c = 0xF7C9C2; f1 += 0.8f; sp *= 1.4f; target += 0.06f * k; }
        else if (!strcmp(e, "playful"))   { c = 0xCBE8C9; f2 += 2; sp *= 1.8f; target += 0.08f * k; }
        else if (!strcmp(e, "sad"))       { c = 0xB9CBE6; f1 = 0.8f; sp *= 0.5f; target *= 0.6f; }
        else if (!strcmp(e, "angry"))     { c = 0xF0A596; f1 = 6; f2 = 11; sp *= 2.2f; target += 0.15f * k; }
        else if (!strcmp(e, "surprised")) { c = 0xF5DE9A; f1 = 2; f2 = 9; target += 0.2f * k * fabsf(sinf(t * 3)); }
    }
    if (target > 1) target = 1;
    wv->amp += (target - wv->amp) * 0.25f;
    for (int i = 0; i < WAVE_N; i++) {
        float u = i / (float)(WAVE_N - 1);
        float env = sinf(u * 3.14159f);
        float v = 0.7f * sinf(6.2832f * f1 * u + t * sp) + 0.3f * sinf(6.2832f * f2 * u - t * sp * 1.4f);
        wv->pts[i].y = (int)(wv->h / 2.0f + v * env * wv->amp * half);
    }
    if (c != wv->col) {
        wv->col = c;
        lv_obj_set_style_line_color(wv->line, lv_color_hex(c), 0);
    }
    lv_line_set_points(wv->line, wv->pts, WAVE_N);
}

/* ---------- navigation ---------- */
static void open_cb(lv_event_t *e)
{
    lv_screen_load(s_page[(int)(intptr_t)lv_event_get_user_data(e)]);
}

static void back_cb(lv_event_t *e)
{
    lv_screen_load(s_home);
}

static lv_obj_t *screen(lv_color_t bg)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_remove_style_all(s);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s, bg, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    return s;
}

static lv_obj_t *card(int v, int x, int y, int w, int h, lv_color_t col)
{
    lv_obj_t *c = box(s_home, x, y, w, h, col, 24);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(c, lv_color_darken(col, 20), LV_STATE_PRESSED);
    lv_obj_add_event_cb(c, open_cb, LV_EVENT_CLICKED, (void *)(intptr_t)v);
    return c;
}

/* full screen page: one big card on the oat page, tap anywhere to go back */
static lv_obj_t *page(int v, lv_color_t col, int x, int w)
{
    lv_obj_t *s = screen(C_BG);
    s_page[v] = s;
    lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s, back_cb, LV_EVENT_CLICKED, NULL);
    return box(s, x, 16, w, 448, col, 24);
}

/* swipe left = next screen, swipe right = previous, wrapping around (no animation: a
 * slide would redraw the whole panel every frame) */
static lv_obj_t *screen_n(int n)
{
    lv_obj_t *all[N_SCREENS] = {s_home, s_keys, s_sys};
    return all[(n % N_SCREENS + N_SCREENS) % N_SCREENS];
}

static void swipe_cb(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    lv_dir_t d = lv_indev_get_gesture_dir(in);
    if (d != LV_DIR_LEFT && d != LV_DIR_RIGHT) return;
    lv_indev_wait_release(in);          /* the swipe must not also count as a tap on a card */
    int cur = (int)(intptr_t)lv_event_get_user_data(e);
    lv_screen_load(screen_n(cur + (d == LV_DIR_LEFT ? 1 : -1)));
}

/* page dots under the cards, `on` = which one is lit */
static void dots(lv_obj_t *scr, int on)
{
    lv_obj_add_event_cb(scr, swipe_cb, LV_EVENT_GESTURE, (void *)(intptr_t)on);
    for (int i = 0; i < N_SCREENS; i++) {
        lv_color_t c = i == on ? C_INK : lv_color_hex(0xC9C0B6);
        box(scr, 400 - 8 * N_SCREENS + i * 16 + 4, 469, 8, 8, c, 4);
    }
}

/* ---------- widgets ---------- */
static tbox_t h_state, h_title, h_artist, h_album, h_t0, h_t1, h_lprev, h_lcur, h_lnext;
static pbar_t h_bar;
static tbox_t h_clock, h_date;
static tbox_t h_wcity, h_wtemp, h_wdesc, h_wrange;
static tbox_t h_bname, h_btxt;

static tbox_t d_back, d_title, d_artist, d_album, d_t0, d_t1, d_lprev, d_lcur, d_lnext;
static pbar_t d_bar;
static tbox_t d_clock, d_sec, d_date, d_daytxt;
static pbar_t d_daybar;
static tbox_t d_wcity, d_wtemp, d_wdesc, d_wfeels, d_whum, d_wwind, d_wrange, d_wfc[3];
static tbox_t d_bname, d_buser, d_breply;
static tbox_t k_title, k_name, k_code, k_slabel, k_stream, k_clabel, k_count, k_rlabel, k_kpm;

/* system page: one card per figure */
enum { Y_CPU, Y_GPU, Y_CODEX, Y_MEM, Y_NET, Y_CLAUDE, Y_COUNT };
static tbox_t y_val[Y_COUNT], y_sub[Y_COUNT];
static pbar_t y_bar[Y_COUNT];

static void build_home(void)
{
    s_home = screen(C_BG);
    dots(s_home, 0);
    lv_obj_t *c;

    /* music: whole left side */
    c = card(V_MUSIC, 16, 16, 500, 448, C_SAGE);
    vinyl_create(&s_vh, c, 20, 20, 200, C_SAGE);
    h_state = tbox(c, 228, 20, 252, 28, C_MINT, FONTS(F20));
    h_title = tbox(c, 228, 50, 252, 100, C_WHITE, FONTS(F48, F40, F32, F28));
    h_artist = tbox(c, 228, 152, 252, 38, C_SUB, FONTS(F32, F28, F24));
    h_album = tbox(c, 228, 192, 252, 30, C_DIM, FONTS(F24, F20));
    h_bar = pbar(c, 24, 238, 452, 8, C_TRACK, C_WHITE);
    h_t0 = tbox(c, 24, 252, 120, 26, C_SUB, FONTS(F20));
    h_t1 = tbox(c, 356, 252, 120, 26, C_SUB, FONTS(F20));
    tbox_align(&h_t1, LV_TEXT_ALIGN_RIGHT);
    h_lprev = tbox(c, 24, 284, 452, 32, C_DIM, FONTS(F24, F20));
    h_lcur = tbox(c, 24, 318, 452, 88, C_WHITE, FONTS(F48, F40, F32, F28));
    h_lnext = tbox(c, 24, 408, 452, 32, C_SUB, FONTS(F24, F20));

    /* clock */
    c = card(V_TIME, 532, 16, 252, 148, C_DUSK);
    h_clock = tbox(c, 20, 8, 212, 86, C_WHITE, FONTS(F72));
    h_date = tbox(c, 22, 100, 212, 38, C_SUB, FONTS(F28, F24));

    /* weather */
    c = card(V_WEATHER, 532, 180, 252, 136, C_TAUPE);
    h_wcity = tbox(c, 20, 10, 110, 32, C_SUB, FONTS(F24, F20));
    h_wtemp = tbox(c, 18, 46, 116, 70, C_WHITE, FONTS(F56, F48));
    h_wdesc = tbox(c, 134, 52, 106, 36, C_WHITE, FONTS(F28, F24, F20));
    h_wrange = tbox(c, 134, 92, 106, 30, C_SUB, FONTS(F24, F20));

    /* robot: the line and what it says */
    c = card(V_BOT, 532, 332, 252, 132, C_MAUVE);
    h_bname = tbox(c, 20, 8, 212, 32, C_SUB, FONTS(F24, F20));
    wave_create(&s_wh, c, 14, 40, 224, 36, 4);
    h_btxt = tbox(c, 20, 78, 212, 50, C_WHITE, FONTS(F24, F20));
}

static void build_keys(void)
{
    s_keys = screen(C_BG);
    dots(s_keys, 1);
    lv_obj_t *c;

    /* the key just pressed, huge, with its key code */
    c = box(s_keys, 16, 16, 500, 448, C_DUSK, 24);
    k_title = tbox(c, 26, 18, 448, 34, C_SUB, FONTS(F28));
    tset(&k_title, "刚按下");
    k_name = tbox(c, 20, 70, 460, 170, C_WHITE, FONTS(F120, F96, F72, F56, F40));
    tbox_align(&k_name, LV_TEXT_ALIGN_CENTER);
    k_code = tbox(c, 20, 246, 460, 40, C_SUB, FONTS(F28, F24));
    tbox_align(&k_code, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *b = box(c, 20, 312, 460, 116, lv_color_hex(0x8494A3), 18);
    k_slabel = tbox(b, 18, 10, 424, 30, C_SUB, FONTS(F24));
    tset(&k_slabel, "最近输入");
    k_stream = tbox(b, 18, 44, 424, 62, C_WHITE, FONTS(F40, F32, F28));

    /* today's count */
    c = box(s_keys, 532, 16, 252, 216, C_TAUPE, 24);
    k_clabel = tbox(c, 22, 18, 212, 34, C_SUB, FONTS(F28));
    tset(&k_clabel, "今天按了");
    k_count = tbox(c, 18, 70, 220, 120, C_WHITE, FONTS(F72, F56, F48));

    /* typing speed */
    c = box(s_keys, 532, 248, 252, 216, C_SAGE, 24);
    k_rlabel = tbox(c, 22, 18, 212, 34, C_SUB, FONTS(F28));
    tset(&k_rlabel, "每分钟");
    k_kpm = tbox(c, 18, 70, 220, 120, C_WHITE, FONTS(F72, F56, F48));
}

static void build_sys(void)
{
    s_sys = screen(C_BG);
    dots(s_sys, 2);
    static const char *names[Y_COUNT] = {"CPU", "GPU", "Codex 本周剩余", "内存", "网速", "Claude 本周剩余"};
    const uint32_t cols[Y_COUNT] = {0x8C9A8E, 0x96A5B3, 0x9A9EA8, 0xB8A396, 0xA0939F, 0xBE9F8E};
    for (int i = 0; i < Y_COUNT; i++) {
        int col = i % 3, row = i / 3;
        int x = 16 + col * 262, y = 16 + row * 232, w = col == 2 ? 244 : 246;
        lv_color_t bg = lv_color_hex(cols[i]);
        lv_obj_t *c = box(s_sys, x, y, w, 216, bg, 24);
        tbox_t t = tbox(c, 20, 14, w - 40, 34, C_SUB, FONTS(F28, F24));
        tset(&t, names[i]);
        if (i == Y_NET) {
            /* two lines: down / up */
            y_val[i] = tbox(c, 20, 58, w - 40, 110, C_WHITE, FONTS(F40, F32, F28));
            y_sub[i] = tbox(c, 20, 172, w - 40, 30, C_SUB, FONTS(F20));
            y_bar[i] = (pbar_t){0};
        } else {
            y_val[i] = tbox(c, 18, 62, w - 36, 80, C_WHITE, FONTS(F56, F48));
            y_bar[i] = pbar(c, 20, 150, w - 40, 10, lv_color_darken(bg, 40), C_WHITE);
            y_sub[i] = tbox(c, 20, 170, w - 40, 30, C_SUB, FONTS(F20));
        }
    }
}

static void refresh_sys(void)
{
    /* S fields: cpu gpu mem mem_detail down up codex_left codex_reset claude_left claude_reset */
    const char *pct[Y_COUNT] = {S.sys[0], S.sys[1], S.sys[6], S.sys[2], "", S.sys[8]};
    for (int i = 0; i < Y_COUNT; i++) {
        if (i == Y_NET) continue;
        if (pct[i][0]) {
            tsetf(&y_val[i], "%s%%", pct[i]);
            pbar_set(&y_bar[i], atoi(pct[i]) * 10);
        } else {
            tset(&y_val[i], "--");
            pbar_set(&y_bar[i], 0);
        }
    }
    tset(&y_sub[Y_CPU], "");
    tset(&y_sub[Y_GPU], "");
    tset(&y_sub[Y_MEM], S.sys[3]);
    if (S.sys[4][0]) tsetf(&y_val[Y_NET], "下 %s\n上 %s", S.sys[4], S.sys[5]);
    else tset(&y_val[Y_NET], "--");
    tset(&y_sub[Y_NET], "");
    tset(&y_sub[Y_CODEX], S.sys[7][0] ? S.sys[7] : (S.sys[6][0] ? "" : "还没有记录"));
    tset(&y_sub[Y_CLAUDE], S.sys[9][0] ? S.sys[9] : (S.sys[8][0] ? "" : "还没有记录"));
}

static void build_pages(void)
{
    lv_obj_t *c, *s;

    /* music: big vinyl on the page, text on a card */
    c = page(V_MUSIC, C_SAGE, 392, 392);
    s = s_page[V_MUSIC];
    d_back = tbox(s, 24, 18, 300, 34, C_INK, FONTS(F24));
    vinyl_create(&s_vd, s, 16, 70, 384, C_BG);
    d_title = tbox(c, 24, 20, 344, 100, C_WHITE, FONTS(F48, F40, F32, F28));
    d_artist = tbox(c, 24, 124, 344, 38, C_SUB, FONTS(F32, F28, F24));
    d_album = tbox(c, 24, 164, 344, 30, C_DIM, FONTS(F24, F20));
    d_bar = pbar(c, 24, 206, 344, 8, C_TRACK, C_WHITE);
    d_t0 = tbox(c, 24, 220, 120, 26, C_SUB, FONTS(F20));
    d_t1 = tbox(c, 248, 220, 120, 26, C_SUB, FONTS(F20));
    tbox_align(&d_t1, LV_TEXT_ALIGN_RIGHT);
    d_lprev = tbox(c, 24, 256, 344, 36, C_DIM, FONTS(F28, F24, F20));
    d_lcur = tbox(c, 24, 296, 344, 100, C_WHITE, FONTS(F48, F40, F32, F28));
    d_lnext = tbox(c, 24, 410, 344, 32, C_SUB, FONTS(F24, F20));

    /* clock */
    c = page(V_TIME, C_DUSK, 16, 768);
    d_clock = tbox(c, 0, 60, 768, 150, C_WHITE, FONTS(F120));
    tbox_align(&d_clock, LV_TEXT_ALIGN_CENTER);
    d_sec = tbox(c, 0, 196, 768, 40, C_SUB, FONTS(F32));
    tbox_align(&d_sec, LV_TEXT_ALIGN_CENTER);
    d_date = tbox(c, 0, 250, 768, 52, C_WHITE, FONTS(F40, F32));
    tbox_align(&d_date, LV_TEXT_ALIGN_CENTER);
    d_daybar = pbar(c, 104, 336, 560, 10, lv_color_hex(0x7D8D9C), C_WHITE);
    d_daytxt = tbox(c, 0, 360, 768, 34, C_SUB, FONTS(F24));
    tbox_align(&d_daytxt, LV_TEXT_ALIGN_CENTER);

    /* weather */
    c = page(V_WEATHER, C_TAUPE, 16, 768);
    d_wcity = tbox(c, 32, 22, 300, 44, C_SUB, FONTS(F32, F28));
    d_wtemp = tbox(c, 26, 66, 320, 140, C_WHITE, FONTS(F120, F96));
    d_wdesc = tbox(c, 370, 34, 370, 60, C_WHITE, FONTS(F48, F40, F32));
    d_wfeels = tbox(c, 370, 104, 370, 38, C_SUB, FONTS(F28, F24));
    d_whum = tbox(c, 370, 146, 370, 38, C_SUB, FONTS(F28, F24));
    d_wwind = tbox(c, 370, 188, 370, 38, C_SUB, FONTS(F28, F24));
    d_wrange = tbox(c, 32, 214, 320, 38, C_SUB, FONTS(F28, F24));
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = box(c, 24 + i * 244, 280, 232, 144, lv_color_hex(0xA8938A), 18);
        d_wfc[i] = tbox(b, 14, 14, 204, 116, C_WHITE, FONTS(F32, F28, F24));
        tbox_align(&d_wfc[i], LV_TEXT_ALIGN_CENTER);
    }

    /* robot */
    c = page(V_BOT, C_MAUVE, 16, 768);
    d_bname = tbox(c, 34, 20, 700, 44, C_SUB, FONTS(F32, F28));
    wave_create(&s_wd, c, 30, 70, 708, 120, 7);
    d_buser = tbox(c, 34, 202, 700, 36, C_SUB, FONTS(F28, F24, F20));
    d_breply = tbox(c, 34, 246, 700, 186, C_WHITE, FONTS(F40, F32, F28, F24));
}

/* ---------- refresh ---------- */
static int32_t music_pos(void)
{
    int64_t p = S.pos_ms;
    if (S.playing && S.pos_at_us) p += (esp_timer_get_time() - S.pos_at_us) / 1000;
    if (S.dur_ms && p > S.dur_ms) p = S.dur_ms;
    return (int32_t)p;
}

static void refresh_progress(void)
{
    char a[16], b[16];
    int32_t p = music_pos() / 1000, d = S.dur_ms / 1000;
    if (S.dur_ms <= 0) {
        a[0] = b[0] = 0;
    } else {
        snprintf(a, sizeof(a), "%ld:%02ld", (long)(p / 60), (long)(p % 60));
        snprintf(b, sizeof(b), "%ld:%02ld", (long)(d / 60), (long)(d % 60));
    }
    int pm = S.dur_ms > 0 ? (int)((int64_t)music_pos() * 1000 / S.dur_ms) : 0;
    tset(&h_t0, a); tset(&d_t0, a);
    tset(&h_t1, b); tset(&d_t1, b);
    pbar_set(&h_bar, pm);
    pbar_set(&d_bar, pm);
}

static void refresh_data(void)
{
    bool online = S.last_rx_us && esp_timer_get_time() - S.last_rx_us < 15000000;
    tset(&h_title, S.title);
    tset(&d_title, S.title);
    tset(&h_artist, S.artist);
    tset(&d_artist, S.artist);
    tset(&h_album, S.album);
    tset(&d_album, S.album);
    follow(&h_artist, &h_title, 4);
    follow(&h_album, &h_artist, 2);
    follow(&d_artist, &d_title, 4);
    follow(&d_album, &d_artist, 2);
    tset(&h_state, !online ? "电脑未连接" : !S.dur_ms ? "" : S.playing ? "● 播放中" : "Ⅱ 已暂停");

    const char *cur = S.lyr[2][0] ? S.lyr[2] : (S.dur_ms ? "♪" : "");
    tset(&h_lprev, S.lyr[1]);
    tset(&h_lcur, cur);
    tset(&h_lnext, S.lyr[3]);
    tset(&d_lprev, S.lyr[1]);
    tset(&d_lcur, cur);
    tset(&d_lnext, S.lyr[3]);
    follow(&h_lnext, &h_lcur, 6);
    follow(&d_lnext, &d_lcur, 6);

    tset(&h_wcity, S.w_city[0] ? S.w_city : "天气");
    tset(&h_wtemp, S.w_temp[0] ? S.w_temp : "--");
    tset(&h_wdesc, S.w_desc);
    tset(&h_wrange, S.w_range);
    tset(&d_wcity, S.w_city);
    tset(&d_wtemp, S.w_temp[0] ? S.w_temp : "--");
    tset(&d_wdesc, S.w_desc);
    if (S.w_feels[0]) tsetf(&d_wfeels, "体感 %s", S.w_feels); else tset(&d_wfeels, "");
    if (S.w_hum[0]) tsetf(&d_whum, "湿度 %s", S.w_hum); else tset(&d_whum, "");
    tset(&d_wwind, S.w_wind);
    if (S.w_range[0]) tsetf(&d_wrange, "今天 %s", S.w_range); else tset(&d_wrange, "");
    for (int i = 0; i < 3; i++) tset(&d_wfc[i], S.w_fc[i]);

    static const char *hs[] = {"小耄", "小耄 · 在听", "小耄 · 想一想", "小耄 · 说话中"};
    static const char *ds[] = {"小耄  ·  说「你好耄耋」叫我", "小耄  ·  在听…", "小耄  ·  想一想…", "小耄  ·  说话中"};
    int b = bot_state();
    tset(&h_bname, hs[b]);
    tset(&d_bname, ds[b]);
    tset(&h_btxt, b == 1 ? "我在听…" : b == 2 ? "让我想想…" : S.bot_reply);
    if (S.bot_user[0]) tsetf(&d_buser, "你：%s", S.bot_user); else tset(&d_buser, "");
    tset(&d_breply, b == 1 && !S.bot_reply[0] ? "我在听…" : S.bot_reply);

    tset(&k_name, S.key_name[0] ? S.key_name : "—");
    tset(&k_code, S.key_code);
    tset(&k_stream, S.key_stream);
    tset(&k_count, S.key_count[0] ? S.key_count : "0");
    tset(&k_kpm, S.key_kpm[0] ? S.key_kpm : "0");
    refresh_sys();

    if (S.art_ver != s_art_ver) {
        s_art_ver = S.art_ver;
        link_get_art(s_art);
        s_have_art = true;
        vinyl_draw(&s_vh);
        vinyl_draw(&s_vd);
    }
    refresh_progress();
}

static void refresh_clock(void)
{
    static const char *wd[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    if (!S.sync_us) {
        tset(&h_clock, "--:--");
        tset(&d_clock, "--:--");
        tset(&h_date, "等待同步");
        return;
    }
    time_t t = S.epoch + S.tz + (esp_timer_get_time() - S.sync_us) / 1000000;
    struct tm tm;
    gmtime_r(&t, &tm);
    tsetf(&h_clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
    tsetf(&d_clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
    tsetf(&d_sec, "%02d 秒", tm.tm_sec);
    tsetf(&h_date, "%d月%d日 %s", tm.tm_mon + 1, tm.tm_mday, wd[tm.tm_wday]);
    tsetf(&d_date, "%d年%d月%d日  %s", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, wd[tm.tm_wday]);
    int secs = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
    pbar_set(&d_daybar, secs * 1000 / 86400);
    tsetf(&d_daytxt, "今天已经过去 %d%%", secs * 100 / 86400);
}

static void data_timer(lv_timer_t *t)
{
    static int64_t last_online_check;
    bool changed = link_get(&S, &s_ver);
    int64_t now = esp_timer_get_time();
    if (changed || now - last_online_check > 2000000) {
        last_online_check = now;
        refresh_data();
    } else {
        refresh_progress();
    }
    refresh_clock();
}

static void anim_timer(lv_timer_t *t)
{
    if (g_goto_screen >= 0) {
        lv_screen_load(screen_n(g_goto_screen));
        g_goto_screen = -1;
    }
    lv_obj_t *scr = lv_screen_active();
    float now = lv_tick_get() / 1000.0f;
    if (scr == s_home) wave_update(&s_wh, now);
    else if (scr == s_page[V_BOT]) wave_update(&s_wd, now);
}

void ui_init(const uint8_t *font, size_t font_len)
{
    for (int i = 0; i < F_COUNT; i++) {
        F[i] = lv_tiny_ttf_create_data_ex(font, font_len, s_px[i], LV_FONT_KERNING_NONE,
                                          s_px[i] >= 72 ? 32 : s_px[i] >= 40 ? 128 : 256);
    }
    s_art = heap_caps_calloc(1, ART_BYTES, MALLOC_CAP_SPIRAM);
    build_home();
    build_keys();
    build_sys();
    build_pages();
    tset(&d_back, "← 点任意处返回");
    lv_screen_load(s_home);
    link_get(&S, &s_ver);
    refresh_data();
    refresh_clock();
    lv_timer_create(data_timer, 200, NULL);
    lv_timer_create(anim_timer, 50, NULL);
}
