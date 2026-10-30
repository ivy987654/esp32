#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "lvgl.h"
#include "board.h"
#include "link.h"
#include "ui.h"

static const char *TAG = "main";

/* LXGW WenKai subset lives in its own flash partition (flashed once, see README),
 * read in place through the cache: never copied to RAM. Layout: "FONT" u32 len ttf[len] */
static const uint8_t *font_map(size_t *len)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "font");
    if (!p) return NULL;
    const uint8_t *hdr;
    esp_partition_mmap_handle_t h;
    if (esp_partition_mmap(p, 0, 0x10000, ESP_PARTITION_MMAP_DATA, (const void **)&hdr, &h) != ESP_OK) return NULL;
    if (memcmp(hdr, "FONT", 4)) return NULL;
    uint32_t n = hdr[4] | hdr[5] << 8 | hdr[6] << 16 | (uint32_t)hdr[7] << 24;
    esp_partition_munmap(h);
    if (n == 0 || n + 8 > p->size) return NULL;
    const uint8_t *base;
    if (esp_partition_mmap(p, 0, (n + 8 + 0xFFFF) & ~0xFFFF, ESP_PARTITION_MMAP_DATA, (const void **)&base, &h) != ESP_OK)
        return NULL;
    *len = n;
    return base + 8;
}

static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void flush_cb(lv_display_t *disp, const lv_area_t *a, uint8_t *px)
{
    /* copies into the PSRAM frame buffer before returning */
    esp_lcd_panel_draw_bitmap(g_panel, a->x1, a->y1, a->x2 + 1, a->y2 + 1, px);
    lv_display_flush_ready(disp);
}

static void touch_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    int16_t x, y;
    bool p = board_touch_read(&x, &y);
    data->point.x = x;
    data->point.y = y;
    data->state = p ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void gui_task(void *arg)
{
    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    /* render buffer in internal RAM: LVGL draws here, flush copies to the frame buffer */
    size_t sz = LCD_W * 60 * 2;
    void *b1 = heap_caps_malloc(sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    lv_display_set_buffers(disp, b1, NULL, sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_cb);

    size_t flen = 0;
    const uint8_t *font = font_map(&flen);
    if (!font) {
        ESP_LOGE(TAG, "font partition missing: flash font/font.bin at 0x400000");
        while (1) vTaskDelay(1000);
    }
    ui_init(font, flen);

    while (1) {
        uint32_t ms = lv_timer_handler();
        if (ms > 20) ms = 20;
        if (ms < 2) ms = 2;
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    board_lcd_init();
    board_touch_init();
    link_start();
    xTaskCreatePinnedToCore(gui_task, "gui", 16384, NULL, 4, NULL, 1);
}
