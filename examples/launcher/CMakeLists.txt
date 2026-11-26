#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(examples_launcher LANGUAGES CXX)

if(NOT TARGET vlink)
  include(GNUInstallDirs)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output/bin)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output/lib)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output/lib)
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
  endif()
endif()

if(NOT TARGET vlink AND NOT TARGET vlink::launcher_server)
  find_package(vlink QUIET COMPONENTS ALL launcher_server)
endif()
if(NOT TARGET vlink::launcher_server)
  message(STATUS "Skipping launcher examples because vlink::launcher_server is unavailable.")
  return()
endif()

add_subdirectory(component_pipeline)
add_subdirectory(hybrid_dataflow)
add_subdirectory(embedded_orchestrator)
add_subdirectory(nested_includes)
