#
# Copyright (C) 2026 by Thun Lu. All rights reserved. Licensed under the Apache License, Version 2.0.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(example_method_async LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/example_standalone.cmake")

include(GNUInstallDirs)

if(NOT TARGET vlink::vlink)
  find_package(vlink QUIET COMPONENTS all)
endif()
if(NOT TARGET vlink::vlink)
  message(FATAL_ERROR "Can not find_package for vlink.")
endif()

add_executable(${PROJECT_NAME} method_async.cc)
target_link_libraries(${PROJECT_NAME} vlink::all)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
