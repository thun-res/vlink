#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(example_fastbuffer LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/example_standalone.cmake")

if(NOT TARGET vlink::vlink)
  find_package(vlink REQUIRED)
endif()

option(ENABLE_FASTBUFFER_CUDA "Build the CUDA FastBuffer plugin" OFF)
option(ENABLE_FASTBUFFER_HIP "Build the HIP FastBuffer plugin" OFF)
option(ENABLE_FASTBUFFER_HBMEM "Build the Horizon hbmem FastBuffer plugin" OFF)

if(ENABLE_FASTBUFFER_CUDA)
  add_subdirectory(cuda)
endif()

if(ENABLE_FASTBUFFER_HIP)
  add_subdirectory(hip)
endif()

if(ENABLE_FASTBUFFER_HBMEM)
  add_subdirectory(hbmem)
endif()
