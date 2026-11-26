#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(example_launcher_hybrid_dataflow LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/example_standalone.cmake")

include(GNUInstallDirs)

if(NOT TARGET vlink::vlink)
  find_package(vlink QUIET COMPONENTS ALL)
endif()
if(NOT TARGET vlink::vlink)
  message(FATAL_ERROR "Can not find_package for vlink.")
endif()

# External standalone sensor & actuator processes
add_executable(example_launcher_sensor_node sensor_node.cc)
target_link_libraries(example_launcher_sensor_node PRIVATE vlink::all)

add_executable(example_launcher_actuator_node actuator_node.cc)
target_link_libraries(example_launcher_actuator_node PRIVATE vlink::all)

# Isolated [[container]] plugin library (HybridPlanner + SafetySupervisor)
add_library(example_launcher_planner_plugin SHARED planner_plugin.cc)
target_link_libraries(example_launcher_planner_plugin PRIVATE vlink::all)
set_target_properties(
  example_launcher_planner_plugin PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
                                             LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
)
foreach(OUTPUTCONFIG DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  if(DEFINED CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG})
    set(_plugin_runtime_output_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG}}")
  else()
    set(_plugin_runtime_output_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
  endif()
  set_target_properties(
    example_launcher_planner_plugin PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG} "${_plugin_runtime_output_dir}"
                                               LIBRARY_OUTPUT_DIRECTORY_${OUTPUTCONFIG} "${_plugin_runtime_output_dir}"
  )
endforeach()

add_custom_command(
  TARGET example_launcher_planner_plugin
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/hybrid_dataflow.toml"
          "$<TARGET_FILE_DIR:example_launcher_planner_plugin>/hybrid_dataflow.toml"
)

install(
  TARGETS example_launcher_sensor_node example_launcher_actuator_node example_launcher_planner_plugin
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES hybrid_dataflow.toml DESTINATION ${CMAKE_INSTALL_BINDIR})
