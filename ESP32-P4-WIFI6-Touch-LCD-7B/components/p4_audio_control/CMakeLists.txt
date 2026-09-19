idf_component_register(
    SRCS "p4_audio_control.c"
    INCLUDE_DIRS "include" "../../../protocol"
    REQUIRES p4_uac2_stream p4_audio_dashboard nvs_flash
)
