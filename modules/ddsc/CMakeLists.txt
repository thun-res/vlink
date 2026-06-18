#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

set(VLINK_MODULE_NAME ddsc)
set(VLINK_MODULE_IMPORT_ALL FALSE)

project(vlink-${VLINK_MODULE_NAME} LANGUAGES C CXX)

# options
option(SKIP_DDSC "Skip ddsc module" OFF)

if(SKIP_DDSC)
  message(STATUS "Skip ddsc module")
  return()
endif()

# requires
if(NOT TARGET CycloneDDS::ddsc)
  find_package(CycloneDDS CONFIG QUIET)
endif()

if(NOT TARGET CycloneDDS::ddsc)
  message(STATUS "Ignore ddsc module")
  return()
endif()

# rt-libs
list(APPEND VLINK_LIBRARIES ${PROJECT_NAMESPACE}${VLINK_MODULE_NAME})
set(VLINK_LIBRARIES
    ${VLINK_LIBRARIES}
    PARENT_SCOPE
)

# src
file(GLOB PROJECT_HDRS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/*.hpp ${PROJECT_SOURCE_DIR}/builtin/*.h)
file(GLOB PROJECT_SRCS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/*.cc ${PROJECT_SOURCE_DIR}/builtin/*.c)
add_library(${PROJECT_NAME} ${PROJECT_HDRS} ${PROJECT_SRCS})
add_library(${PROJECT_NAMESPACE}${PROJECT_NAME} ALIAS ${PROJECT_NAME})
add_library(${PROJECT_NAMESPACE}${VLINK_MODULE_NAME} ALIAS ${PROJECT_NAME})
target_link_libraries(${PROJECT_NAME} PUBLIC vlink::vlink)
target_link_libraries(${PROJECT_NAME} PRIVATE CycloneDDS::ddsc)
target_compile_definitions(
  ${PROJECT_NAME}
  PRIVATE "${VLINK_LIB_DEFINES}"
  PUBLIC "VLINK_SUPPORT_DDSC"
)
if(DEFINED ddsi_INCLUDE_DIR)
  target_include_directories(${PROJECT_NAME} BEFORE PRIVATE "${ddsi_INCLUDE_DIR}")
endif()
if(WIN32)
  target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 iphlpapi bcrypt)
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
