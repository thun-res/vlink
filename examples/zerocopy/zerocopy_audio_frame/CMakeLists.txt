#
# Copyright (C) 2026 by Thun Lu. All rights reserved. Licensed under the Apache License, Version 2.0.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(example_zerocopy_audio_frame LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/example_standalone.cmake")

include(GNUInstallDirs)

if(NOT TARGET vlink::vlink)
  find_package(vlink QUIET)
endif()
if(NOT TARGET vlink::vlink)
  message(FATAL_ERROR "Can not find_package for vlink.")
endif()

# Producer executable: creates and publishes audio frames
add_executable(example_audio_frame_producer producer.cc)
target_link_libraries(example_audio_frame_producer vlink::all)
install(TARGETS example_audio_frame_producer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Consumer executable: receives and validates audio frames
add_executable(example_audio_frame_consumer consumer.cc)
target_link_libraries(example_audio_frame_consumer vlink::all)
install(TARGETS example_audio_frame_consumer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
