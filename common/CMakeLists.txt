# Common library for timer and utility functions
add_library(common STATIC c_timers.c wtime.c c_print_results.c)

target_include_directories(common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
