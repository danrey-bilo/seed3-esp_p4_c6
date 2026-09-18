cmake_minimum_required(VERSION 3.16)

# Keep the previous locally modified component on disk, but do not link it.
set(EXCLUDE_COMPONENTS espressif__usb_device_uac)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(seed3_p4_spi_uac2)
