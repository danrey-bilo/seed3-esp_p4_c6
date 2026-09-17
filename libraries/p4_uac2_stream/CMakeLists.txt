idf_component_register(SRCS "p4_uac2_stream.c" "usb_descriptors.c"
    INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "private"
    PRIV_REQUIRES usb esp_timer esp_hw_support
    REQUIRES espressif__tinyusb)
if(NOT IDF_TARGET STREQUAL "esp32p4")
    message(FATAL_ERROR "p4_uac2_stream supports ESP32-P4 only")
endif()
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
idf_component_get_property(tusb_dir espressif__tinyusb COMPONENT_DIR)
target_include_directories(${tusb_lib} PUBLIC "${COMPONENT_DIR}/private")
include("${COMPONENT_DIR}/port/dwc2_patch.cmake")
