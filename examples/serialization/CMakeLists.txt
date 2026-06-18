#
# Copyright (C) 2026 by Thun Lu. All rights reserved. Licensed under the Apache License, Version 2.0.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(examples_serialization)

if(NOT TARGET vlink)
  include(GNUInstallDirs)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output/bin)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output/lib)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output/lib)
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
  endif()
endif()

add_subdirectory(basic_types)
add_subdirectory(custom_type)
add_subdirectory(dynamic_data)
add_subdirectory(intra_data)
