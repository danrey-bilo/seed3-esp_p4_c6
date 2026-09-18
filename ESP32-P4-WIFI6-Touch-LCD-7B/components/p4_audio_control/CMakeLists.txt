idf_component_register(
    SRCS "p4_audio_control.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "../../../protocol"
    REQUIRES p4_uac2_stream p4_audio_dashboard nvs_flash
)
