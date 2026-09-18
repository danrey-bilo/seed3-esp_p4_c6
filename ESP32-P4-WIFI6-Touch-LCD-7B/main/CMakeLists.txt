idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "." "../../protocol"
    REQUIRES esp_driver_gpio esp_driver_spi esp_driver_uart p4_uac2_stream
)
