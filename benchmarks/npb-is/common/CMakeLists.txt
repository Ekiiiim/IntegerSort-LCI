# Common library for timer and utility functions
add_library(npb_is_common STATIC c_timers.c wtime.c c_print_results.c)

target_include_directories(npb_is_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
