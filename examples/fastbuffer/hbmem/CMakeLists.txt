#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(example_fastbuffer_hbmem LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/example_standalone.cmake")

if(NOT TARGET vlink::vlink)
  find_package(vlink REQUIRED)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "This Hbmem IPC example requires 64-bit Linux.")
endif()

set(FASTBUFFER_HBMEM_ROOT
    ""
    CACHE PATH "Horizon hbmem SDK root"
)
find_path(
  FASTBUFFER_HBMEM_INCLUDE_DIR hb_mem_mgr.h
  HINTS "${FASTBUFFER_HBMEM_ROOT}"
  PATH_SUFFIXES include
)
find_library(
  FASTBUFFER_HBMEM_LIBRARY hbmem
  HINTS "${FASTBUFFER_HBMEM_ROOT}"
  PATH_SUFFIXES lib lib64
)
if(NOT FASTBUFFER_HBMEM_INCLUDE_DIR OR NOT FASTBUFFER_HBMEM_LIBRARY)
  message(FATAL_ERROR "The hbmem plugin requires hb_mem_mgr.h and libhbmem.")
endif()
add_library(fastbuffer_hbmem SHARED hbmem_plugin.cc)
target_include_directories(fastbuffer_hbmem SYSTEM PRIVATE "${FASTBUFFER_HBMEM_INCLUDE_DIR}")
target_link_libraries(fastbuffer_hbmem PRIVATE vlink::vlink "${FASTBUFFER_HBMEM_LIBRARY}")
target_compile_features(fastbuffer_hbmem PRIVATE cxx_std_17)
install(TARGETS fastbuffer_hbmem LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
