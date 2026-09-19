/* LVGL objects and draw tasks must not compete with USB/SPI DMA SRAM.
 * This allocator deliberately has NO internal-memory fallback. */
#include "lvgl.h"
#include "esp_heap_caps.h"

#if LV_USE_STDLIB_MALLOC != LV_STDLIB_CUSTOM
#error "p4_lvgl_memory requires CONFIG_LV_USE_CUSTOM_MALLOC=y"
#endif

static const uint32_t ui_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{ (void)mem; (void)bytes; return NULL; }
void lv_mem_remove_pool(lv_mem_pool_t pool) { (void)pool; }

void *lv_malloc_core(size_t size)
{
    return heap_caps_malloc(size, ui_caps);
}

void *lv_realloc_core(void *ptr, size_t size)
{
    return heap_caps_realloc(ptr, size, ui_caps);
}

void lv_free_core(void *ptr) { heap_caps_free(ptr); }

void lv_mem_monitor_core(lv_mem_monitor_t *monitor)
{
    /* Measures the shared PSRAM heap, not only LVGL allocations. */
    multi_heap_info_t info;
    heap_caps_get_info(&info, ui_caps);
    *monitor = (lv_mem_monitor_t){
        .total_size = info.total_allocated_bytes + info.total_free_bytes,
        .free_cnt = info.free_blocks,
        .free_size = info.total_free_bytes,
        .free_biggest_size = info.largest_free_block,
        .used_cnt = info.allocated_blocks,
        .max_used = info.total_allocated_bytes + info.total_free_bytes
                    - info.minimum_free_bytes,
    };
    if (monitor->total_size)
        monitor->used_pct = 100 - monitor->free_size * 100 / monitor->total_size;
    if (monitor->free_size)
        monitor->frag_pct = 100 - monitor->free_biggest_size * 100 / monitor->free_size;
}

lv_result_t lv_mem_test_core(void)
{
    return heap_caps_check_integrity(ui_caps, true) ? LV_RESULT_OK : LV_RESULT_INVALID;
}
