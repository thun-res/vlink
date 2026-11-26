#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(example_launcher_embedded_orchestrator LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/example_standalone.cmake")

include(GNUInstallDirs)

if(NOT TARGET vlink::launcher_server)
  find_package(vlink QUIET COMPONENTS ALL launcher_server)
endif()
if(NOT TARGET vlink::launcher_server)
  message(STATUS "Skipping example_launcher_embedded because vlink::launcher_server is unavailable.")
  return()
endif()

# Self-contained plugin library hosting VehicleBridge, CruisePlanner, ParkingPlanner and TelemetryTap
add_library(example_launcher_embedded_components SHARED embedded_components.cc)
target_link_libraries(example_launcher_embedded_components PRIVATE vlink::all)
set_target_properties(
  example_launcher_embedded_components PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
                                                  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
)
foreach(OUTPUTCONFIG DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  if(DEFINED CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG})
    set(_plugin_runtime_output_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG}}")
  else()
    set(_plugin_runtime_output_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
  endif()
  set_target_properties(
    example_launcher_embedded_components
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG} "${_plugin_runtime_output_dir}"
               LIBRARY_OUTPUT_DIRECTORY_${OUTPUTCONFIG} "${_plugin_runtime_output_dir}"
  )
endforeach()

# Embedded C++ LauncherServer + LauncherAPI host executable
add_executable(example_launcher_embedded embedded_orchestrator.cc)
target_link_libraries(example_launcher_embedded PRIVATE vlink::launcher_server)
add_dependencies(example_launcher_embedded example_launcher_embedded_components)

add_custom_command(
  TARGET example_launcher_embedded
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/embedded_orchestrator.toml"
          "$<TARGET_FILE_DIR:example_launcher_embedded>/embedded_orchestrator.toml"
)

install(
  TARGETS example_launcher_embedded example_launcher_embedded_components
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES embedded_orchestrator.toml DESTINATION ${CMAKE_INSTALL_BINDIR})
