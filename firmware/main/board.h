#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_lcd_panel_ops.h"

#define LCD_W 800
#define LCD_H 480

extern esp_lcd_panel_handle_t g_panel;

void board_lcd_init(void);
void board_lcd_wait_vsync(void);
bool board_touch_init(void);
/* returns true while pressed, fills x/y */
bool board_touch_read(int16_t *x, int16_t *y);
