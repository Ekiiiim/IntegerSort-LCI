option(IS_BUILD_LCI "Build the LCI version of the Integer Sort benchmark" ON)
if(NOT IS_BUILD_LCI)
  return()
endif()

find_package(OpenMP)
if(NOT OpenMP_FOUND)
  message(WARNING "OpenMP not found. LCI version of IS benchmark will not be built.")
  return()
endif()

find_package(
  LCI
  CONFIG
  HINTS
  ${LCI_ROOT}
  $ENV{LCI_ROOT}
  PATH_SUFFIXES
  lib/cmake
  lib64/cmake)
if(NOT LCI_FOUND)
  message(STATUS "Existing LCI installation not found. Try FetchContent.")
  include(FetchContent)
  FetchContent_Declare(
    lci
    GIT_REPOSITORY https://github.com/uiuc-hpc/lci.git
    GIT_TAG master)
  FetchContent_MakeAvailable(lci)
  set(LCI_FOUND TRUE)
endif()

if(NOT LCI_FOUND)
  message(WARNING "LCI not found. LCI version of IS benchmark will not be built.")
  return()
endif()

if(NOT IS_BUILD_LCI)
  return()
endif()

message(STATUS "Building LCI version of IS benchmark")

option(IS_LCI_ENABLE_TIMERS "Enable LCI alltoall thread-local timer profiling (verbose per-iteration output; off by default)" OFF)

foreach(CLASS ${NPB_CLASSES})
  set(TARGET_NAME is_lci_${CLASS})

  add_executable(${TARGET_NAME} is.cpp a2a_tl_timers.cpp)

  # Include directories
  target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                                    ${CMAKE_SOURCE_DIR}/common)

  # Link common library
  target_link_libraries(${TARGET_NAME} PRIVATE common OpenMP::OpenMP_CXX LCI::lci)

  # Compile definitions
  if(IS_LCI_ENABLE_TIMERS)
    target_compile_definitions(${TARGET_NAME} PRIVATE A2A_TL_TIMERS)
  endif()
  set_npb_definitions(${TARGET_NAME} ${CLASS})

  # Output directory
  set_target_properties(${TARGET_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                                  ${CMAKE_BINARY_DIR}/bin)
endforeach()
