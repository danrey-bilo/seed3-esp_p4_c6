cmake_minimum_required(VERSION 3.16)

# The component folder itself is portable; no dependency on the parent repository.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")
# Optional offline build with an already installed, pinned TinyUSB checkout.
if(P4_UAC2_LOCAL_TINYUSB_DIR)
    list(APPEND EXTRA_COMPONENT_DIRS "${P4_UAC2_LOCAL_TINYUSB_DIR}")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(p4_uac2_compile_check)
