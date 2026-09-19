idf_component_register(
    SRCS "audio_dashboard.c" "pedalboard_widgets.c" "pedal_patch.c" "pedal_layout.c" "wifi_view.c"
    INCLUDE_DIRS "include" "../../../protocol"
    REQUIRES esp32_p4_wifi6_touch_lcd_7b lvgl__lvgl p4_lvgl_memory p4_wifi_settings
)
