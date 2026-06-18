#
# Copyright (C) 2026 by Thun Lu. All rights reserved. Licensed under the Apache License, Version 2.0.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(example_zerocopy_tensor LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/example_standalone.cmake")

include(GNUInstallDirs)

if(NOT TARGET vlink::vlink)
  find_package(vlink QUIET)
endif()
if(NOT TARGET vlink::vlink)
  message(FATAL_ERROR "Can not find_package for vlink.")
endif()

# Producer executable: creates and publishes N-D tensors
add_executable(example_tensor_producer producer.cc)
target_link_libraries(example_tensor_producer vlink::all)
install(TARGETS example_tensor_producer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Consumer executable: receives and validates N-D tensors
add_executable(example_tensor_consumer consumer.cc)
target_link_libraries(example_tensor_consumer vlink::all)
install(TARGETS example_tensor_consumer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
