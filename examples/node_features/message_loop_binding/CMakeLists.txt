#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(example_message_loop_binding LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/example_standalone.cmake")

include(GNUInstallDirs)

if(NOT TARGET vlink::vlink)
  find_package(vlink QUIET)
endif()
if(NOT TARGET vlink::vlink)
  message(FATAL_ERROR "Can not find_package for vlink.")
endif()

add_executable(${PROJECT_NAME} message_loop_binding.cc)

target_link_libraries(${PROJECT_NAME} vlink::all)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
