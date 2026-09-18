get_filename_component(stream_component "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
get_filename_component(stream_component "${stream_component}" NAME)
idf_component_register(SRCS "main.c" INCLUDE_DIRS "." REQUIRES ${stream_component})
