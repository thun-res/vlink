#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(sample_ddsc_proto LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/example_standalone.cmake")

include(GNUInstallDirs)

if(NOT Protobuf_FOUND)
  find_package(Protobuf CONFIG QUIET)
endif()
if(NOT TARGET vlink::vlink)
  find_package(vlink QUIET COMPONENTS ddsc)
endif()
if(NOT TARGET vlink::vlink)
  message(FATAL_ERROR "Can not find_package for vlink.")
endif()
if(NOT TARGET vlink::ddsc OR NOT Protobuf_FOUND)
  message(STATUS "Skip ${PROJECT_NAME}.")
  return()
endif()

vlink_generate_cpp(TARGET ddsc_proto_gen PROTO ddsc_proto.proto)

add_executable(${PROJECT_NAME} ddsc_proto.cc)

target_link_libraries(${PROJECT_NAME} ddsc_proto_gen vlink::ddsc)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
