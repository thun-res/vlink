#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(examples_c_api)

if(NOT TARGET vlink)
  include(GNUInstallDirs)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output/bin)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output/lib)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output/lib)
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
  endif()
endif()

if(TARGET vlink::c_api)
  add_subdirectory(c_pubsub)
  add_subdirectory(c_rpc)
  add_subdirectory(c_field)
  add_subdirectory(c_security)
else()
  message(STATUS "Skip C API examples (vlink::c_api not available).")
endif()
