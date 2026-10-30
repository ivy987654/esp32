/* ATK DNESP32S3 + ATK-MD0430R (800x480 RGB, GT9xxx touch).
 * Pins/timings taken from ALIENTEK's 23_rgb / 24_touch examples. */
#include "board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_rgb.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "board";
esp_lcd_panel_handle_t g_panel;

/* ---------- RGB LCD (double frame buffer, swap on vsync = no tearing) ---------- */
#include "freertos/semphr.h"
#include "esp_attr.h"
static SemaphoreHandle_t s_vsync;

static IRAM_ATTR bool on_vsync(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *ed, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_vsync, &woken);
    return woken == pdTRUE;
}

void board_lcd_init(void)
{
    esp_lcd_rgb_panel_config_t cfg = {
        .data_width = 16,
        .clk_src = LCD_CLK_SRC_PLL160M,
        .disp_gpio_num = -1,
        .pclk_gpio_num = 5,
        .hsync_gpio_num = -1,
        .vsync_gpio_num = -1,
        .de_gpio_num = 4,
        .data_gpio_nums = {
            17, 16, 15, 7, 6,          /* B3..B7 */
            10, 9, 46, 3, 8, 18,       /* G2..G7 */
            45, 48, 47, 21, 14,        /* R3..R7 */
        },
        .timings = {
            .pclk_hz = 16 * 1000 * 1000,
            .h_res = LCD_W,
            .v_res = LCD_H,
            .hsync_back_porch = 88,
            .hsync_front_porch = 40,
            .hsync_pulse_width = 3,
            .vsync_back_porch = 32,
            .vsync_front_porch = 13,
            .vsync_pulse_width = 48,
            .flags.pclk_active_neg = true,
        },
        .flags.fb_in_psram = true,
        .bounce_buffer_size_px = LCD_W * 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &g_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(g_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(g_panel));

    s_vsync = xSemaphoreCreateBinary();
    esp_lcd_rgb_panel_event_callbacks_t cbs = {.on_vsync = on_vsync};
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(g_panel, &cbs, NULL));
}


void board_lcd_wait_vsync(void)
{
    xSemaphoreTake(s_vsync, 0);
    xSemaphoreTake(s_vsync, pdMS_TO_TICKS(40));
}

/* ---------- XL9555 (I2C0) : LCD_BL = P1.0, CT_RST = P1.1 ---------- */
static i2c_master_bus_handle_t s_bus0, s_bus1;
static i2c_master_dev_handle_t s_xl, s_gt;

static esp_err_t xl_write(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(s_xl, b, 2, 100);
}

static uint8_t xl_read(uint8_t reg)
{
    uint8_t v = 0xFF;
    i2c_master_transmit_receive(s_xl, &reg, 1, &v, 1, 100);
    return v;
}

/* ---------- GT9xxx (I2C1 SDA39 SCL38) ---------- */
static esp_err_t gt_write(uint16_t reg, const uint8_t *buf, int len)
{
    uint8_t b[8] = {reg >> 8, reg & 0xFF};
    for (int i = 0; i < len; i++) b[2 + i] = buf[i];
    return i2c_master_transmit(s_gt, b, 2 + len, 50);
}

static esp_err_t gt_read(uint16_t reg, uint8_t *buf, int len)
{
    uint8_t r[2] = {reg >> 8, reg & 0xFF};
    return i2c_master_transmit_receive(s_gt, r, 2, buf, len, 50);
}

bool board_touch_init(void)
{
    i2c_master_bus_config_t b0 = {
        .i2c_port = I2C_NUM_0, .sda_io_num = 41, .scl_io_num = 42,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&b0, &s_bus0));
    i2c_device_config_t xd = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x20, .scl_speed_hz = 400000};
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus0, &xd, &s_xl));

    /* P1.0/P1.1 as outputs, backlight on, pulse touch reset */
    uint8_t out1 = xl_read(3) | 0x01;
    xl_write(3, out1 & ~0x02);
    xl_write(7, xl_read(7) & ~0x03);
    vTaskDelay(pdMS_TO_TICKS(20));
    xl_write(3, out1 | 0x02);
    vTaskDelay(pdMS_TO_TICKS(100));

    gpio_config_t io = {.pin_bit_mask = 1ULL << 40, .mode = GPIO_MODE_INPUT};
    gpio_config(&io);

    i2c_master_bus_config_t b1 = {
        .i2c_port = I2C_NUM_1, .sda_io_num = 39, .scl_io_num = 38,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&b1, &s_bus1));
    uint16_t addr = 0;
    for (int tries = 0; tries < 10 && !addr; tries++) {
        if (i2c_master_probe(s_bus1, 0x14, 50) == ESP_OK) addr = 0x14;
        else if (i2c_master_probe(s_bus1, 0x5D, 50) == ESP_OK) addr = 0x5D;
        else vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!addr) {
        ESP_LOGE(TAG, "touch controller not found");
        return false;
    }
    i2c_device_config_t gd = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 400000};
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus1, &gd, &s_gt));
    uint8_t id[5] = {0};
    gt_read(0x8140, id, 4);
    ESP_LOGW(TAG, "touch @0x%02X id=%s", addr, (char *)id);
    return true;
}

bool board_touch_read(int16_t *x, int16_t *y)
{
    static bool pressed;
    static int16_t lx, ly;
    if (!s_gt) return false;
    uint8_t st = 0;
    if (gt_read(0x814E, &st, 1) != ESP_OK) return pressed;
    if (st & 0x80) {
        int n = st & 0x0F;
        if (n > 0 && n <= 5) {
            uint8_t p[4];
            if (gt_read(0x8150, p, 4) == ESP_OK) {
                lx = p[0] | (p[1] << 8);
                ly = p[2] | (p[3] << 8);
                if (lx >= LCD_W) lx = LCD_W - 1;
                if (ly >= LCD_H) ly = LCD_H - 1;
            }
            pressed = true;
        } else {
            pressed = false;
        }
        uint8_t z = 0;
        gt_write(0x814E, &z, 1);
    }
    *x = lx;
    *y = ly;
    return pressed;
}
