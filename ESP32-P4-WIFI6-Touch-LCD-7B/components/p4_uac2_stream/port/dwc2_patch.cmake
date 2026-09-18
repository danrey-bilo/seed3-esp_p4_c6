# Build a checked, private DCD variant. Never modify managed_components or IDF.
set(dwc2_dir "${tusb_dir}/src/portable/synopsys/dwc2")
set(dwc2_generated "${CMAKE_CURRENT_BINARY_DIR}/p4_dcd_dwc2.c")
execute_process(COMMAND "${PYTHON}" "${COMPONENT_DIR}/port/patch_dwc2.py"
    "${dwc2_dir}/dcd_dwc2.c" "${dwc2_generated}"
    RESULT_VARIABLE patch_result)
if(NOT patch_result EQUAL 0)
    message(FATAL_ERROR "P4 DWC2 patch failed; do not use an unverified TinyUSB version")
endif()
get_target_property(tusb_sources ${tusb_lib} SOURCES)
list(FILTER tusb_sources EXCLUDE REGEX "(^|/)dcd_dwc2\\.c$")
set_property(TARGET ${tusb_lib} PROPERTY SOURCES "${tusb_sources}")
target_sources(${tusb_lib} PRIVATE "${dwc2_generated}")
target_include_directories(${tusb_lib} PRIVATE "${dwc2_dir}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${COMPONENT_DIR}/port/patch_dwc2.py" "${dwc2_dir}/dcd_dwc2.c")
