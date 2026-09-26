#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(example_fastbuffer_hip LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/example_standalone.cmake")

if(NOT TARGET vlink::vlink)
  find_package(vlink REQUIRED)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "This Hip IPC example requires 64-bit Linux.")
endif()

set(FASTBUFFER_HIP_ROOT
    ""
    CACHE PATH "HIP SDK root"
)
find_path(
  FASTBUFFER_HIP_INCLUDE_DIR hip/hip_runtime_api.h
  HINTS "${FASTBUFFER_HIP_ROOT}" ENV ROCM_PATH
  PATHS /opt/rocm
  PATH_SUFFIXES include
)
find_library(
  FASTBUFFER_HIP_LIBRARY amdhip64
  HINTS "${FASTBUFFER_HIP_ROOT}" ENV ROCM_PATH
  PATHS /opt/rocm
  PATH_SUFFIXES lib lib64
)
if(NOT FASTBUFFER_HIP_INCLUDE_DIR OR NOT FASTBUFFER_HIP_LIBRARY)
  message(FATAL_ERROR "The HIP plugin requires HIP headers and libamdhip64.")
endif()
add_library(fastbuffer_hip SHARED hip_plugin.cc)
target_include_directories(fastbuffer_hip SYSTEM PRIVATE "${FASTBUFFER_HIP_INCLUDE_DIR}")
target_compile_definitions(fastbuffer_hip PRIVATE __HIP_PLATFORM_AMD__)
target_link_libraries(fastbuffer_hip PRIVATE vlink::vlink "${FASTBUFFER_HIP_LIBRARY}")
target_compile_features(fastbuffer_hip PRIVATE cxx_std_17)
install(TARGETS fastbuffer_hip LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
