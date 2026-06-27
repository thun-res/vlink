#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(sample_shm_raw LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/example_standalone.cmake")

include(GNUInstallDirs)

if(NOT TARGET vlink::vlink)
  find_package(vlink QUIET COMPONENTS shm)
endif()
if(NOT TARGET vlink::vlink)
  message(FATAL_ERROR "Can not find_package for vlink.")
endif()
if(NOT TARGET vlink::shm)
  message(STATUS "Skip ${PROJECT_NAME}.")
  return()
endif()

add_executable(${PROJECT_NAME} shm_raw.cc)

target_link_libraries(${PROJECT_NAME} vlink::shm)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
