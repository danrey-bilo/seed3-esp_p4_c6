idf_component_register(SRCS "p4_wifi_settings.c" INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_wifi_remote esp_netif esp_event nvs_flash)
