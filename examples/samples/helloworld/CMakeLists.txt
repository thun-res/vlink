#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(sample_helloworld LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/example_standalone.cmake")

include(GNUInstallDirs)

if(NOT Protobuf_FOUND)
  find_package(Protobuf CONFIG QUIET)
endif()
if(NOT TARGET vlink::vlink)
  find_package(vlink QUIET COMPONENTS ALL)
endif()
if(NOT TARGET vlink::vlink)
  message(FATAL_ERROR "Can not find_package for vlink.")
endif()

if(NOT Protobuf_FOUND)
  message(STATUS "Skip ${PROJECT_NAME}.")
  return()
endif()

# proto
file(GLOB PROTO_SRCS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/proto/*.proto)
vlink_generate_cpp(TARGET helloworld_gen PROTO ${PROTO_SRCS})

# server
add_subdirectory(server)

# client
add_subdirectory(client)
