#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(vlink-examples)

if(NOT TARGET vlink)
  include(GNUInstallDirs)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output/bin)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output/lib)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output/lib)
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
  endif()
endif()

add_subdirectory(samples)

if(ENABLE_EXAMPLES_ALL)
  add_subdirectory(quickstart)
  add_subdirectory(base)
  add_subdirectory(serialization)
  add_subdirectory(communication)
  add_subdirectory(url_guide)
  add_subdirectory(qos)
  add_subdirectory(security)
  add_subdirectory(zerocopy)
  add_subdirectory(recording)
  add_subdirectory(plugin)
  add_subdirectory(proxy)
  add_subdirectory(c_api)
  add_subdirectory(node_features)
endif()
