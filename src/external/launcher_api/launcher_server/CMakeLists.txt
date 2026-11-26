#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

set(VLINK_MODULE_NAME launcher_server)
set(VLINK_MODULE_IMPORT_ALL TRUE)

project(vlink-${VLINK_MODULE_NAME} LANGUAGES CXX)

# src
add_library(
  ${PROJECT_NAME}
  ../launcher_common.h
  launcher_config.h
  launcher_config.cc
  resource_control.h
  resource_control.cc
  scheduler_manager.h
  scheduler_manager.cc
  log_manager.h
  log_manager.cc
  component_host.h
  component_host.cc
  process_supervisor.h
  process_supervisor.cc
  launcher_server.cc
)
add_library(${PROJECT_NAMESPACE}${PROJECT_NAME} ALIAS ${PROJECT_NAME})
add_library(${PROJECT_NAMESPACE}${VLINK_MODULE_NAME} ALIAS ${PROJECT_NAME})
target_link_libraries(${PROJECT_NAME} PUBLIC vlink::vlink vlink::all vlink::launcher_api)
target_link_libraries(
  ${PROJECT_NAME} PRIVATE $<BUILD_INTERFACE:thirdparty::tomlplusplus> $<BUILD_INTERFACE:thirdparty::bitsery>
                          $<BUILD_INTERFACE:thirdparty::json>
)
if(WIN32)
  target_link_libraries(${PROJECT_NAME} PRIVATE psapi)
endif()
target_compile_definitions(${PROJECT_NAME} PUBLIC "VLINK_ENABLE_LAUNCHER")
if(BUILD_SHARED_LIBS)
  target_compile_definitions(${PROJECT_NAME} PRIVATE "VLINK_LAUNCHER_SERVER_LIBRARY")
else()
  target_compile_definitions(${PROJECT_NAME} PUBLIC "VLINK_LAUNCHER_SERVER_LIBRARY_STATIC")
endif()

# install
install(
  TARGETS ${PROJECT_NAME}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT runtime
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT devel
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
          COMPONENT runtime
          NAMELINK_COMPONENT devel
)

# export
vlink_export(${PROJECT_NAME} ${PROJECT_NAME} ${PROJECT_NAMESPACE} ${CMAKE_PROJECT_VERSION})
