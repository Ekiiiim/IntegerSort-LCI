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

find_package(
  LCI
  CONFIG
  REQUIRED
  HINTS
  ${LCI_ROOT}
  $ENV{LCI_ROOT}
  PATH_SUFFIXES
  lib/cmake
  lib64/cmake)

add_library(lci_irregular src/irregular_runtime.cpp src/am_exchange_state.cpp)
add_library(LCIIrregular::lci_irregular ALIAS lci_irregular)
target_compile_features(lci_irregular PUBLIC cxx_std_17)
target_include_directories(
  lci_irregular PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                       $<INSTALL_INTERFACE:include>)
target_link_libraries(lci_irregular PUBLIC LCI::lci)

# Add subdirectories
add_subdirectory(common)
add_subdirectory(LCI)
add_subdirectory(MPI)
