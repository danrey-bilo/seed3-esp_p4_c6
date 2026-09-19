idf_component_register(
    SRCS "p4_seedfx_storage.c"
    INCLUDE_DIRS "include" "../../../protocol"
    REQUIRES esp32_p4_wifi6_touch_lcd_7b json p4_audio_control
             p4_audio_dashboard
)
