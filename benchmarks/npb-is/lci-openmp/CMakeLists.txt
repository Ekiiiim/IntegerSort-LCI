option(IS_BUILD_LCI "Build the LCI version of the Integer Sort benchmark" ON)
if(NOT IS_BUILD_LCI)
  return()
endif()

find_package(OpenMP COMPONENTS CXX)
if(NOT OpenMP_CXX_FOUND)
  message(WARNING "OpenMP not found. LCI version of IS benchmark will not be built.")
  return()
endif()

message(STATUS "Building LCI version of IS benchmark")

option(IS_LCI_ENABLE_TIMERS "Enable LCI irregular AM profiling and per-iteration reports" OFF)

foreach(CLASS ${NPB_CLASSES})
  set(TARGET_NAME is_lci_${CLASS})

  add_executable(${TARGET_NAME}
    main.cpp
    nas/timers.cpp
    benchmark_support/nas_integer_sort_run.cpp
    benchmark_support/run_options.cpp
    profiling/a2a_profile_report.cpp
    benchmark_support/reduction_operations.cpp
    integer_sort/bucket_plan.cpp
    integer_sort/key_redistribution.cpp
    integer_sort/ranking.cpp
    integer_sort/workspace.cpp
    nas/key_generation.cpp
    nas/reporting.cpp
    nas/run_rules.cpp
    nas/verification.cpp)

  # Include directories
  target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

  # Link common library
  target_link_libraries(${TARGET_NAME} PRIVATE npb_is_common OpenMP::OpenMP_CXX LCIIrregular::lci_irregular)

  # Compile definitions
  if(IS_LCI_ENABLE_TIMERS)
    target_compile_definitions(${TARGET_NAME} PRIVATE IS_LCI_ENABLE_TIMERS=1)
  endif()
  set_npb_definitions(${TARGET_NAME} ${CLASS})

  # Output directory
  set_target_properties(${TARGET_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                                  ${CMAKE_BINARY_DIR}/bin)
endforeach()
