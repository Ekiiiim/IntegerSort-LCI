# MPI version of IS benchmark Build executables for all classes A through E

option(IS_BUILD_MPI "Build the MPI version of the Integer Sort benchmark" ON)
if(NOT IS_BUILD_MPI)
  return()
endif()

find_package(MPI COMPONENTS C)

if(NOT MPI_FOUND)
  message(WARNING "Could not find MPI. MPI version of IS benchmark will not be built.")
  return()
endif()

message(STATUS "Building MPI version of IS benchmark")

foreach(CLASS ${NPB_CLASSES})
  set(TARGET_NAME is_mpi_${CLASS})

  add_executable(${TARGET_NAME} is.c)

  # Link libraries
  target_link_libraries(${TARGET_NAME} PRIVATE common MPI::MPI_C)

  # Compile definitions
  set_npb_definitions(${TARGET_NAME} ${CLASS})

  # Output directory
  set_target_properties(${TARGET_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                                  ${CMAKE_BINARY_DIR}/bin)
endforeach()
