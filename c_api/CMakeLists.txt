#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

set(VLINK_MODULE_NAME c_api)
set(VLINK_MODULE_IMPORT_ALL TRUE)

project(vlink-${VLINK_MODULE_NAME} LANGUAGES CXX)

# src
add_library(${PROJECT_NAME} c_api.cc)
add_library(${PROJECT_NAMESPACE}${PROJECT_NAME} ALIAS ${PROJECT_NAME})
add_library(${PROJECT_NAMESPACE}${VLINK_MODULE_NAME} ALIAS ${PROJECT_NAME})
target_link_libraries(${PROJECT_NAME} PUBLIC vlink::vlink vlink::all)
target_compile_definitions(${PROJECT_NAME} PRIVATE "VLINK_ENABLE_C_INTERFACE")
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_CLANG_TIDY "")
if(BUILD_SHARED_LIBS)
  target_compile_definitions(${PROJECT_NAME} PRIVATE "VLINK_C_API_LIBRARY")
else()
  target_compile_definitions(${PROJECT_NAME} PUBLIC "VLINK_C_API_LIBRARY_STATIC")
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

if(ENABLE_TEST)
  add_subdirectory(test)
endif()
