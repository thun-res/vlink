#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(example_proxy_api_basic LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/example_standalone.cmake")

include(GNUInstallDirs)

if(NOT TARGET vlink::proxy_api)
  find_package(vlink QUIET)
endif()
if(NOT TARGET vlink::proxy_api)
  message(FATAL_ERROR "Can not find_package for vlink proxy_api.")
endif()

add_executable(${PROJECT_NAME} proxy_api_basic.cc)

target_link_libraries(${PROJECT_NAME} vlink::proxy_api)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
