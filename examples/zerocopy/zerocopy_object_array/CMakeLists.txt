#
# Copyright (C) 2026 by Thun Lu. All rights reserved. Licensed under the Apache License, Version 2.0.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(example_zerocopy_object_array LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/example_standalone.cmake")

include(GNUInstallDirs)

if(NOT TARGET vlink::vlink)
  find_package(vlink QUIET)
endif()
if(NOT TARGET vlink::vlink)
  message(FATAL_ERROR "Can not find_package for vlink.")
endif()

# Producer executable: creates and publishes 3-D object arrays
add_executable(example_object_array_producer producer.cc)
target_link_libraries(example_object_array_producer vlink::all)
install(TARGETS example_object_array_producer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Consumer executable: receives and validates 3-D object arrays
add_executable(example_object_array_consumer consumer.cc)
target_link_libraries(example_object_array_consumer vlink::all)
install(TARGETS example_object_array_consumer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
