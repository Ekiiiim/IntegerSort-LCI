# MPI version of IS benchmark Build executables for all classes A through E

find_package(MPI COMPONENTS C REQUIRED)

message(STATUS "Building MPI version of IS benchmark")

foreach(CLASS ${NPB_CLASSES})
  set(TARGET_NAME is_mpi_${CLASS})

  add_executable(${TARGET_NAME} is.c)

  # Link libraries
  target_link_libraries(${TARGET_NAME} PRIVATE npb_is_common MPI::MPI_C)

  # Compile definitions
  set_npb_definitions(${TARGET_NAME} ${CLASS})

  # Output directory
  set_target_properties(${TARGET_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                                  ${CMAKE_BINARY_DIR}/bin)
endforeach()
