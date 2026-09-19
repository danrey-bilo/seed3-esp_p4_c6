#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { P4_WIFI_MAX_APS=16 };
typedef struct { char ssid[33]; int8_t rssi; bool secured; } p4_wifi_ap_t;
typedef struct {
    uint32_t revision;
    bool busy,connected,reconnect;
    char ssid[33],ip[16],message[96];
    unsigned ap_count;
    p4_wifi_ap_t aps[P4_WIFI_MAX_APS];
} p4_wifi_status_t;
/* Nonblocking requests. All Wi-Fi/flash work lives in a low-priority task. */
esp_err_t p4_wifi_settings_init(void);
bool p4_wifi_scan(void);
bool p4_wifi_connect(const char *ssid,const char *password);
bool p4_wifi_disconnect(void);
bool p4_wifi_set_reconnect(bool enabled);
void p4_wifi_get_status(p4_wifi_status_t *out);
#ifdef __cplusplus
}
#endif
