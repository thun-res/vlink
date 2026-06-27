#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(vlink-samples)

if(NOT TARGET vlink)
  include(GNUInstallDirs)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output/bin)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output/lib)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output/lib)
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
  endif()
endif()

add_subdirectory(helloworld)
add_subdirectory(dds_dynamic)
# add_subdirectory(dds_idl)
add_subdirectory(ddsc_proto)
add_subdirectory(pub_sub_fbs)
add_subdirectory(someip_flat)
add_subdirectory(shm_raw)
add_subdirectory(fdbus_proto)
add_subdirectory(ping_pong)
