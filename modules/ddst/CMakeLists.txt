#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

set(VLINK_MODULE_NAME ddst)
set(VLINK_MODULE_IMPORT_ALL FALSE)

project(vlink-${VLINK_MODULE_NAME} LANGUAGES CXX)

# options
option(SKIP_DDST "Skip ddst module" ON)

if(SKIP_DDST)
  message(STATUS "Skip ddst module")
  return()
endif()

# requires
if(NOT TARGET TRAVODDS::travodds AND NOT TARGET TRAVODDS::travoddsstatic)
  find_package(travodds CONFIG QUIET)
endif()

if(TARGET TRAVODDS::travodds)
  set(travodds_libs TRAVODDS::travodds)
elseif(TARGET TRAVODDS::travoddsstatic)
  set(travodds_libs TRAVODDS::travoddsstatic)
else()
  message(STATUS "Ignore ddst module")
  return()
endif()

# rt-libs
list(APPEND VLINK_LIBRARIES ${PROJECT_NAMESPACE}${VLINK_MODULE_NAME})
set(VLINK_LIBRARIES
    ${VLINK_LIBRARIES}
    PARENT_SCOPE
)

# src
file(GLOB PROJECT_HDRS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/*.h ${PROJECT_SOURCE_DIR}/builtin/*.h)
file(GLOB PROJECT_SRCS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/*.cc ${PROJECT_SOURCE_DIR}/builtin/*.cpp)
add_library(${PROJECT_NAME} ${PROJECT_HDRS} ${PROJECT_SRCS})
add_library(${PROJECT_NAMESPACE}${PROJECT_NAME} ALIAS ${PROJECT_NAME})
add_library(${PROJECT_NAMESPACE}${VLINK_MODULE_NAME} ALIAS ${PROJECT_NAME})
target_link_libraries(${PROJECT_NAME} PUBLIC vlink::vlink)
target_link_libraries(${PROJECT_NAME} PRIVATE ${travodds_libs})
target_include_directories(${PROJECT_NAME} PRIVATE ${TRAVODDS_INCLUDE_DIRS})
target_compile_definitions(
  ${PROJECT_NAME}
  PRIVATE "${VLINK_LIB_DEFINES}"
  PUBLIC "VLINK_SUPPORT_DDST"
)

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
