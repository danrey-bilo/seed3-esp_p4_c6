#pragma once
#include "sdkconfig.h"
#include "esp_attr.h"
#define CFG_TUSB_OS OPT_OS_FREERTOS
#define CFG_TUSB_OS_INC_PATH freertos/
#define CFG_TUSB_RHPORT1_MODE (OPT_MODE_DEVICE | OPT_MODE_HIGH_SPEED)
#define CFG_TUD_ENABLED 1
#define CFG_TUD_MAX_SPEED OPT_MODE_HIGH_SPEED
#define CFG_TUD_ENDPOINT0_SIZE 64
#define CFG_TUD_ENDPPOINT_MAX 3
#define CFG_TUD_INTERFACE_MAX 3
#define CFG_TUD_TASK_QUEUE_SZ 32
#define CFG_TUSB_DEBUG 0
// Custom UAC2 class: no TinyUSB audio FIFO or generic audio callbacks.
#define CFG_TUD_AUDIO 0
#define CFG_TUD_DWC2_DMA_ENABLE 1
#define CFG_TUD_DWC2_SLAVE_ENABLE 0
#define CFG_TUD_MEM_DCACHE_ENABLE 1
#define CFG_TUD_MEM_DCACHE_LINE_SIZE 64
#define CFG_TUSB_MEM_SECTION DRAM_ATTR
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(64)))
