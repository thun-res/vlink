#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(example_launcher_component_pipeline LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/example_standalone.cmake")

include(GNUInstallDirs)

if(NOT TARGET vlink::vlink)
  find_package(vlink QUIET COMPONENTS ALL)
endif()
if(NOT TARGET vlink::vlink)
  message(FATAL_ERROR "Can not find_package for vlink.")
endif()

# In-process perception -> fusion -> control pipeline plugin
add_library(demo_launcher_components SHARED demo_components.cc)
target_link_libraries(demo_launcher_components PRIVATE vlink::all)
set_target_properties(
  demo_launcher_components PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
                                      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
)
foreach(OUTPUTCONFIG DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  if(DEFINED CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG})
    set(_plugin_runtime_output_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG}}")
  else()
    set(_plugin_runtime_output_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
  endif()
  set_target_properties(
    demo_launcher_components PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG} "${_plugin_runtime_output_dir}"
                                        LIBRARY_OUTPUT_DIRECTORY_${OUTPUTCONFIG} "${_plugin_runtime_output_dir}"
  )
endforeach()

add_custom_command(
  TARGET demo_launcher_components
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/demo_launch.toml"
          "$<TARGET_FILE_DIR:demo_launcher_components>/demo_launch.toml"
)

install(
  TARGETS demo_launcher_components
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES demo_launch.toml DESTINATION ${CMAKE_INSTALL_BINDIR})
