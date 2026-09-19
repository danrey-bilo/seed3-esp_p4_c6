#pragma once
#include "lvgl.h"
typedef struct { int lv_adapter_cfg, rotation, tear_avoid_mode;
    struct {int swap_xy, mirror_x, mirror_y;} touch_flags; } bsp_display_cfg_t;
#define ESP_LV_ADAPTER_DEFAULT_CONFIG() 0
#define ESP_LV_ADAPTER_ROTATE_0 0
#define ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL 0
static inline lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *cfg) { return lv_display_get_default(); }
static inline lv_indev_t *bsp_display_get_input_dev(void) { return NULL; }
static inline int bsp_display_lock(unsigned ticks) { return 1; }
static inline void bsp_display_unlock(void) {}
static inline void bsp_display_backlight_on(void) {}
