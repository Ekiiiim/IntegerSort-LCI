cmake_minimum_required(VERSION 3.15)
project(
  LCIIrregular
  VERSION 1.0.0
  LANGUAGES CXX)

option(LCI_IRREGULAR_BUILD_EXAMPLES "Build lci-irregular examples" OFF)
option(LCI_IRREGULAR_BUILD_BENCHMARKS "Build NPB IS research benchmarks" OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

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

add_library(lci_irregular src/irregular_runtime.cpp src/profile_writer.cpp)
add_library(LCIIrregular::lci_irregular ALIAS lci_irregular)
target_compile_features(lci_irregular PUBLIC cxx_std_17)
target_include_directories(
  lci_irregular PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                       $<INSTALL_INTERFACE:include>)
target_link_libraries(lci_irregular PUBLIC LCI::lci)

install(
  TARGETS lci_irregular
  EXPORT LCIIrregularTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(
  EXPORT LCIIrregularTargets
  FILE LCIIrregularTargets.cmake
  NAMESPACE LCIIrregular::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LCIIrregular)

configure_package_config_file(
  cmake/LCIIrregularConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/LCIIrregularConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LCIIrregular)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/LCIIrregularConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(
  FILES ${CMAKE_CURRENT_BINARY_DIR}/LCIIrregularConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/LCIIrregularConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LCIIrregular)

if(LCI_IRREGULAR_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(LCI_IRREGULAR_BUILD_BENCHMARKS)
  enable_language(C)
  list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
  add_subdirectory(benchmarks)
endif()
