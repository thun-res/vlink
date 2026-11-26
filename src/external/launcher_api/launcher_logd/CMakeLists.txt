#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

set(VLINK_MODULE_NAME launcher_logd)

project(vlink-${VLINK_MODULE_NAME} LANGUAGES CXX)

# src
add_library(${PROJECT_NAME} SHARED ../launcher_common.h launcher_logd_plugin.cc)
target_link_libraries(${PROJECT_NAME} PRIVATE vlink::vlink vlink::all $<BUILD_INTERFACE:thirdparty::bitsery>)
target_compile_definitions(${PROJECT_NAME} PRIVATE "VLINK_ENABLE_LAUNCHER")

# install
install(
  TARGETS ${PROJECT_NAME}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT runtime
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT devel
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
          COMPONENT runtime
          NAMELINK_COMPONENT devel
)
