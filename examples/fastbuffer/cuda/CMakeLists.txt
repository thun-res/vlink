#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

cmake_minimum_required(VERSION 3.17...3.31)

project(example_fastbuffer_cuda LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/example_standalone.cmake")

if(NOT TARGET vlink::vlink)
  find_package(vlink REQUIRED)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "This Cuda IPC example requires 64-bit Linux.")
endif()

find_package(CUDAToolkit REQUIRED)
add_library(fastbuffer_cuda SHARED cuda_plugin.cc)
target_link_libraries(fastbuffer_cuda PRIVATE vlink::vlink CUDA::cudart)
target_compile_features(fastbuffer_cuda PRIVATE cxx_std_17)
install(TARGETS fastbuffer_cuda LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
