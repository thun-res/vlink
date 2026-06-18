#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

set(VLINK_MODULE_NAME mqtt)
set(VLINK_MODULE_IMPORT_ALL FALSE)

project(vlink-${VLINK_MODULE_NAME} LANGUAGES CXX)

# options
option(SKIP_MQTT "Skip mqtt module" OFF)

if(SKIP_MQTT)
  message(STATUS "Skip mqtt module")
  return()
endif()

# requires
if(NOT TARGET eclipse-paho-mqtt-c::paho-mqtt3c AND NOT TARGET eclipse-paho-mqtt-c::paho-mqtt3cs)
  find_package(eclipse-paho-mqtt-c CONFIG QUIET)
endif()

if(NOT TARGET eclipse-paho-mqtt-c::paho-mqtt3c AND NOT TARGET eclipse-paho-mqtt-c::paho-mqtt3cs)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(PAHO_MQTT_C QUIET IMPORTED_TARGET paho-mqtt3cs)

    if(NOT PAHO_MQTT_C_FOUND)
      pkg_check_modules(PAHO_MQTT_C QUIET IMPORTED_TARGET paho-mqtt3c)
    endif()
  endif()

  if(NOT PAHO_MQTT_C_FOUND)
    message(STATUS "Ignore mqtt module")
    return()
  endif()
endif()

# rt-libs
list(APPEND VLINK_LIBRARIES ${PROJECT_NAMESPACE}${VLINK_MODULE_NAME})
set(VLINK_LIBRARIES
    ${VLINK_LIBRARIES}
    PARENT_SCOPE
)

# src
file(GLOB PROJECT_HDRS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/*.h)
file(GLOB PROJECT_SRCS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/*.cc)
add_library(${PROJECT_NAME} ${PROJECT_HDRS} ${PROJECT_SRCS})
add_library(${PROJECT_NAMESPACE}${PROJECT_NAME} ALIAS ${PROJECT_NAME})
add_library(${PROJECT_NAMESPACE}${VLINK_MODULE_NAME} ALIAS ${PROJECT_NAME})
target_link_libraries(${PROJECT_NAME} PUBLIC vlink::vlink)
if(TARGET eclipse-paho-mqtt-c::paho-mqtt3cs)
  target_link_libraries(${PROJECT_NAME} PRIVATE eclipse-paho-mqtt-c::paho-mqtt3cs)
elseif(TARGET eclipse-paho-mqtt-c::paho-mqtt3c)
  target_link_libraries(${PROJECT_NAME} PRIVATE eclipse-paho-mqtt-c::paho-mqtt3c)
elseif(TARGET PkgConfig::PAHO_MQTT_C)
  target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::PAHO_MQTT_C)
elseif(PAHO_MQTT_C_FOUND)
  target_include_directories(${PROJECT_NAME} PRIVATE ${PAHO_MQTT_C_INCLUDE_DIRS})
  target_link_directories(${PROJECT_NAME} PRIVATE ${PAHO_MQTT_C_LIBRARY_DIRS})
  target_link_libraries(${PROJECT_NAME} PRIVATE ${PAHO_MQTT_C_LIBRARIES})
  target_link_options(${PROJECT_NAME} PRIVATE ${PAHO_MQTT_C_LDFLAGS_OTHER})
endif()

target_compile_definitions(
  ${PROJECT_NAME}
  PRIVATE "${VLINK_LIB_DEFINES}"
  PUBLIC "VLINK_SUPPORT_MQTT"
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
