/* LVGL heap in PSRAM (LV_USE_CUSTOM_MALLOC): glyph caches and image buffers would otherwise
 * eat the internal RAM that the UART driver, the render buffer and the RGB bounce buffers need. */
#include "lvgl.h"
#include "esp_heap_caps.h"

#define CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) { return NULL; }
void lv_mem_remove_pool(lv_mem_pool_t pool) {}

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, CAPS);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    void *q = heap_caps_realloc(p, new_size, CAPS);
    return q ? q : heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    lv_memzero(mon_p, sizeof(*mon_p));
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}
