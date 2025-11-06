# MPI version of IS benchmark Build executables for all classes A through E

foreach(CLASS ${NPB_CLASSES})
  set(TARGET_NAME is_mpi_${CLASS})

  add_executable(${TARGET_NAME} is.c)

  # Include directories
  target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                                    ${CMAKE_SOURCE_DIR}/common)

  # Link libraries
  target_link_libraries(${TARGET_NAME} PRIVATE common MPI::MPI_C)

  # Compile definitions
  set_npb_definitions(${TARGET_NAME} ${CLASS})

  # Compile options
  target_compile_options(${TARGET_NAME} PRIVATE -O3)

  # Output directory
  set_target_properties(${TARGET_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                                  ${CMAKE_BINARY_DIR}/bin)
endforeach()
