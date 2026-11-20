#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

set(VLINK_MODULE_NAME proxy_api)
set(VLINK_MODULE_IMPORT_ALL TRUE)

project(vlink-${VLINK_MODULE_NAME} LANGUAGES CXX)

# src
add_library(${PROJECT_NAME} ../proxy_common.h proxy_api.cc)
add_library(${PROJECT_NAMESPACE}${PROJECT_NAME} ALIAS ${PROJECT_NAME})
add_library(${PROJECT_NAMESPACE}${VLINK_MODULE_NAME} ALIAS ${PROJECT_NAME})
target_link_libraries(${PROJECT_NAME} PUBLIC vlink::vlink vlink::all)
target_link_libraries(${PROJECT_NAME} PRIVATE $<BUILD_INTERFACE:thirdparty::bitsery>)
target_compile_definitions(${PROJECT_NAME} PUBLIC "VLINK_ENABLE_PROXY")
if(BUILD_SHARED_LIBS)
  target_compile_definitions(${PROJECT_NAME} PRIVATE "VLINK_PROXY_API_LIBRARY")
else()
  target_compile_definitions(${PROJECT_NAME} PUBLIC "VLINK_PROXY_API_LIBRARY_STATIC")
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
