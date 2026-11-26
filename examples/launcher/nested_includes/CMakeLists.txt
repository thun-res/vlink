#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(example_launcher_nested_includes NONE)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/example_standalone.cmake")

include(GNUInstallDirs)

set(_NESTED_DST "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/nested_includes")

add_custom_target(
  example_launcher_nested_includes ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory "${_NESTED_DST}/subsystems/sensors"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/system_root.toml"
          "${_NESTED_DST}/system_root.toml"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/subsystems/diagnostics.toml"
          "${_NESTED_DST}/subsystems/diagnostics.toml"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/subsystems/control.toml"
          "${_NESTED_DST}/subsystems/control.toml"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/subsystems/perception.toml"
          "${_NESTED_DST}/subsystems/perception.toml"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/subsystems/sensors/camera_unit.toml"
          "${_NESTED_DST}/subsystems/sensors/camera_unit.toml"
  COMMENT "Copying nested TOML manifest example to ${_NESTED_DST}"
)

if(TARGET demo_launcher_components)
  add_dependencies(example_launcher_nested_includes demo_launcher_components)
endif()

install(FILES system_root.toml DESTINATION ${CMAKE_INSTALL_BINDIR}/nested_includes)
install(FILES subsystems/diagnostics.toml subsystems/control.toml subsystems/perception.toml
        DESTINATION ${CMAKE_INSTALL_BINDIR}/nested_includes/subsystems
)
install(FILES subsystems/sensors/camera_unit.toml
        DESTINATION ${CMAKE_INSTALL_BINDIR}/nested_includes/subsystems/sensors
)
