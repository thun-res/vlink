#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

if(NOT ENABLE_LAUNCHER OR NOT TARGET vlink::launcher_api)
  return()
endif()

if(BUILD_SHARED_LIBS OR CMAKE_POSITION_INDEPENDENT_CODE)
  add_library(launcher_demo_sensor_plugin SHARED sensor_plugin.cc)
  target_link_libraries(launcher_demo_sensor_plugin PRIVATE vlink::launcher_api)
endif()

add_executable(launcher_demo_sensor_node sensor_standalone.cc)
target_link_libraries(launcher_demo_sensor_node PRIVATE vlink::launcher_api)
add_custom_command(
  TARGET launcher_demo_sensor_node
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/launcher_demo.toml"
          "$<TARGET_FILE_DIR:launcher_demo_sensor_node>/launcher_demo.toml"
)
