#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(example_logger_advanced LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/example_standalone.cmake")

if(NOT TARGET vlink::vlink)
  find_package(vlink QUIET COMPONENTS ALL)
endif()
if(NOT TARGET vlink::vlink)
  message(FATAL_ERROR "Can not find_package for vlink.")
endif()

add_executable(${PROJECT_NAME} logger_advanced.cc)
target_link_libraries(${PROJECT_NAME} PRIVATE vlink::all)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
