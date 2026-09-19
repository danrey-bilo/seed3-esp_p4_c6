#pragma once
#include "FreeRTOS.h"
static inline void vTaskDelay(unsigned n) { (void)n; }
static inline int xTaskCreatePinnedToCore(void (*fn)(void *), const char *name,
    unsigned stack, void *arg, int priority, void *handle, int core) { return 1; }
