# MPI version of IS benchmark

add_executable(is_mpi is.c)

# Include directories
target_include_directories(is_mpi PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                          ${CMAKE_SOURCE_DIR}/common)

# Link libraries
target_link_libraries(is_mpi PRIVATE common MPI::MPI_C)

# Compile definitions
set_npb_definitions(is_mpi)

# Compile options
target_compile_options(is_mpi PRIVATE -O3)

# Output directory
set_target_properties(is_mpi PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                        ${CMAKE_BINARY_DIR}/bin)
