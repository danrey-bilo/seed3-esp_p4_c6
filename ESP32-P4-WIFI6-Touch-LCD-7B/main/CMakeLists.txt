idf_component_register(
    SRCS "main.c" "audio_dashboard.c"
    INCLUDE_DIRS "." "../../protocol"
    REQUIRES esp_driver_gpio esp_driver_spi esp_driver_uart p4_uac2_stream
             esp32_p4_wifi6_touch_lcd_7b lvgl__lvgl
)
