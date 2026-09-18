idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES p4_uac2_stream p4_audio_control p4_audio_dashboard
             seed3_spi_transport
)
