cmake_minimum_required(VERSION 3.12)
project(
  IntegerSort-LCI
  VERSION 1.0.0
  LANGUAGES C CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CLASS option (can be set at cmake time: cmake -DCLASS=B ..)
set(CLASS
    "A"
    CACHE STRING "NPB problem class (S, W, A, B, C, D, or E)")
set_property(
  CACHE CLASS
  PROPERTY STRINGS
           S
           W
           A
           B
           C
           D
           E)
string(TOUPPER "${CLASS}" CLASS_UPPER)
if(NOT CLASS_UPPER MATCHES "^[SWABCDE]$")
  message(
    FATAL_ERROR "CLASS must be one of: S, W, A, B, C, D, or E (got: ${CLASS})")
endif()

# Find required packages
find_package(MPI REQUIRED COMPONENTS C CXX)
find_package(OpenMP)

# Find LCI library (try common locations)
set(LCI_LIBRARY)
set(LCI_INCLUDE_DIR)

if(APPLE)
  find_library(
    LCI_LIBRARY
    NAMES lci
    PATHS /usr/local/lib
    NO_DEFAULT_PATH)
  find_path(
    LCI_INCLUDE_DIR
    NAMES lci.hpp
    PATHS /usr/local/include
    NO_DEFAULT_PATH)
elseif(UNIX)
    find_library(
      LCI_LIBRARY
      NAMES lci
      PATHS $ENV{HOME}/opt/lci/lib64
      NO_DEFAULT_PATH)
    find_path(
      LCI_INCLUDE_DIR
      NAMES lci.hpp
      PATHS $ENV{HOME}/opt/lci/include
      NO_DEFAULT_PATH)
endif()

if(NOT LCI_LIBRARY)
  message(WARNING "LCI library not found. LCI target may not build correctly.")
endif()

# Include helper functions
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
include(NPBConfig)

# Add subdirectories
add_subdirectory(common)
add_subdirectory(LCI)
add_subdirectory(MPI)
