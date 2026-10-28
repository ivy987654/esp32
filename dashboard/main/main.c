#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "board.h"
#include "link.h"
#include "ui.h"

#define DRAW_LINES 40

static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void flush_cb(lv_display_t *disp, const lv_area_t *a, uint8_t *px)
{
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
    size_t sz = LCD_W * DRAW_LINES * 2;
    void *b1 = heap_caps_malloc(sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    void *b2 = heap_caps_malloc(sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    lv_display_set_buffers(disp, b1, b2, sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_cb);

    ui_init();

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
