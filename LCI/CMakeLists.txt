# LCI version of IS benchmark

add_executable(is_lci is.cpp a2a_tl_timers.cpp)

# Include directories
target_include_directories(is_lci PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                          ${CMAKE_SOURCE_DIR}/common)

# Link common library
target_link_libraries(is_lci PRIVATE common)

# Link MPI (required for compilation, but LCI uses its own runtime)
target_link_libraries(is_lci PRIVATE MPI::MPI_CXX)

# Compile definitions
target_compile_definitions(is_lci PRIVATE A2A_TL_TIMERS)
set_npb_definitions(is_lci)

# Compile options
target_compile_options(is_lci PRIVATE -O3)

# OpenMP configuration
if(APPLE)
  # macOS: use Homebrew libomp
  target_compile_options(is_lci PRIVATE -Xpreprocessor -fopenmp)
  target_include_directories(is_lci PRIVATE /opt/homebrew/opt/libomp/include)
  target_link_directories(is_lci PRIVATE /opt/homebrew/opt/libomp/lib)
  target_link_libraries(is_lci PRIVATE omp)
elseif(OpenMP_CXX_FOUND)
  # Linux: use CMake's OpenMP if found
  target_link_libraries(is_lci PRIVATE OpenMP::OpenMP_CXX)
else()
  # Fallback: try -fopenmp
  target_compile_options(is_lci PRIVATE -fopenmp)
endif()

# LCI-specific paths for Linux
if(UNIX AND NOT APPLE)
  if(DEFINED ENV{HOME})
    target_include_directories(is_lci PRIVATE $ENV{HOME}/opt/lci/include)
    target_link_directories(is_lci PRIVATE $ENV{HOME}/opt/lci/lib64)
  endif()
endif()

# Link LCI library
if(LCI_LIBRARY)
  target_link_libraries(is_lci PRIVATE ${LCI_LIBRARY})
  if(LCI_INCLUDE_DIR)
    target_include_directories(is_lci PRIVATE ${LCI_INCLUDE_DIR})
  endif()
  # Set rpath for LCI library
  if(APPLE)
    target_link_options(is_lci PRIVATE -Wl,-rpath,/usr/local/lib)
  elseif(UNIX AND DEFINED ENV{HOME})
    target_link_options(is_lci PRIVATE -Wl,-rpath,$ENV{HOME}/opt/lci/lib64)
  endif()
else()
  # Fallback: try to link with -llci
  target_link_libraries(is_lci PRIVATE lci)
endif()

# Output directory
set_target_properties(is_lci PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                        ${CMAKE_BINARY_DIR}/bin)
