idf_component_register(
    SRCS "seed3_spi_transport.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "../../../protocol"
    REQUIRES esp_driver_gpio esp_driver_spi esp_driver_uart p4_uac2_stream
             p4_audio_control p4_audio_dashboard
)
