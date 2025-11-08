cmake_minimum_required(VERSION 3.12)
project(
  IntegerSort-LCI
  VERSION 1.0.0
  LANGUAGES C CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(NPB_CLASSES A B C D E)

# Include helper functions
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
include(NPBConfig)

# Add subdirectories
add_subdirectory(common)
add_subdirectory(LCI)
add_subdirectory(MPI)
