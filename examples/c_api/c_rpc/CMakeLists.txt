#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(example_c_rpc LANGUAGES C)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/example_standalone.cmake")

include(GNUInstallDirs)

if(NOT TARGET vlink::c_api)
  find_package(vlink QUIET)
endif()
if(NOT TARGET vlink::c_api)
  message(FATAL_ERROR "Can not find_package for vlink c_api.")
endif()

add_executable(${PROJECT_NAME} c_rpc.c)

target_link_libraries(${PROJECT_NAME} vlink::c_api)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
