# LCI version of IS benchmark Build executables for all classes A through E

foreach(CLASS ${NPB_CLASSES})
  set(TARGET_NAME is_lci_${CLASS})

  add_executable(${TARGET_NAME} is.cpp a2a_tl_timers.cpp)

  # Include directories
  target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                                    ${CMAKE_SOURCE_DIR}/common)

  # Link common library
  target_link_libraries(${TARGET_NAME} PRIVATE common)

  # Link MPI (required for compilation, but LCI uses its own runtime)
  target_link_libraries(${TARGET_NAME} PRIVATE MPI::MPI_CXX)

  # Compile definitions
  target_compile_definitions(${TARGET_NAME} PRIVATE A2A_TL_TIMERS)
  set_npb_definitions(${TARGET_NAME} ${CLASS})

  # Compile options
  target_compile_options(${TARGET_NAME} PRIVATE -O3)

  # OpenMP configuration
  if(APPLE)
    # macOS: use Homebrew libomp
    target_compile_options(${TARGET_NAME} PRIVATE -Xpreprocessor -fopenmp)
    target_include_directories(${TARGET_NAME}
                               PRIVATE /opt/homebrew/opt/libomp/include)
    target_link_directories(${TARGET_NAME} PRIVATE /opt/homebrew/opt/libomp/lib)
    target_link_libraries(${TARGET_NAME} PRIVATE omp)
  elseif(OpenMP_CXX_FOUND)
    # Linux: use CMake's OpenMP if found
    target_link_libraries(${TARGET_NAME} PRIVATE OpenMP::OpenMP_CXX)
  else()
    # Fallback: try -fopenmp
    target_compile_options(${TARGET_NAME} PRIVATE -fopenmp)
  endif()

  # LCI-specific paths for Linux
  if(UNIX AND NOT APPLE)
    if(DEFINED ENV{HOME})
      target_include_directories(${TARGET_NAME}
                                 PRIVATE $ENV{HOME}/opt/lci/include)
      target_link_directories(${TARGET_NAME} PRIVATE $ENV{HOME}/opt/lci/lib64)
    endif()
  endif()

  # Link LCI library
  if(LCI_LIBRARY)
    target_link_libraries(${TARGET_NAME} PRIVATE ${LCI_LIBRARY})
    if(LCI_INCLUDE_DIR)
      target_include_directories(${TARGET_NAME} PRIVATE ${LCI_INCLUDE_DIR})
    endif()
    # Set rpath for LCI library
    if(APPLE)
      target_link_options(${TARGET_NAME} PRIVATE -Wl,-rpath,/usr/local/lib)
    elseif(UNIX AND DEFINED ENV{HOME})
      target_link_options(${TARGET_NAME} PRIVATE
                          -Wl,-rpath,$ENV{HOME}/opt/lci/lib64)
    endif()
  else()
    # Fallback: try to link with -llci
    target_link_libraries(${TARGET_NAME} PRIVATE lci)
  endif()

  # Output directory
  set_target_properties(${TARGET_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                                  ${CMAKE_BINARY_DIR}/bin)
endforeach()
