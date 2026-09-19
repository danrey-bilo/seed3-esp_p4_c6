#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mount the onboard microSD and atomically install its verified SeedFX
 * catalogue/autorun graph. Absence or corruption is non-fatal: the built-in
 * safe catalogue remains active and ESP_OK is returned. */
esp_err_t p4_seedfx_storage_init(void);

#ifdef __cplusplus
}
#endif
