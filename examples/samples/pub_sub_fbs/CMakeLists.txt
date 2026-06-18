#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(sample_pub_sub_fbs LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/example_standalone.cmake")

include(GNUInstallDirs)

if(NOT flatbuffers_FOUND)
  find_package(flatbuffers CONFIG QUIET)
endif()
if(NOT TARGET vlink::vlink)
  find_package(vlink QUIET COMPONENTS ddsc)
endif()
if(NOT TARGET vlink::vlink)
  message(FATAL_ERROR "Can not find_package for vlink.")
endif()
if(NOT TARGET vlink::ddsc OR NOT flatbuffers_FOUND)
  message(STATUS "Skip ${PROJECT_NAME}.")
  return()
endif()

vlink_generate_cpp(TARGET pub_sub_fbs_gen FBS fbs/helloworld.fbs)

add_executable(${PROJECT_NAME} pub_sub_fbs.cc)

target_link_libraries(${PROJECT_NAME} PRIVATE pub_sub_fbs_gen vlink::ddsc)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
