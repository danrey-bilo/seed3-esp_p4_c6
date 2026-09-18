#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the qualified 20 MHz SPI/READY transport and Seed UART. */
esp_err_t seed3_spi_transport_start(void);

/* Start diagnostic commands only after UAC2 has been initialised. */
esp_err_t seed3_spi_transport_start_console(void);

void seed3_spi_transport_log(void);
void seed3_spi_transport_log_ready(void);

#ifdef __cplusplus
}
#endif
