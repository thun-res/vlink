#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

set(VLINK_MODULE_NAME zenoh)
set(VLINK_MODULE_IMPORT_ALL FALSE)

project(vlink-${VLINK_MODULE_NAME} LANGUAGES CXX)

# options
option(SKIP_ZENOH "Skip zenoh module" OFF)

if(SKIP_ZENOH)
  message(STATUS "Skip zenoh module")
  return()
endif()

option(ENABLE_ZENOH_PICO "Enable zenoh-pico libs" OFF)

# requires
if(ENABLE_ZENOH_PICO)
  if(NOT TARGET zenohpico::zenohpico_shared AND NOT TARGET zenohpico::zenohpico_static)
    find_package(zenohpico CONFIG QUIET)
  endif()

  if(NOT TARGET zenohpico::zenohpico_shared AND NOT TARGET zenohpico::zenohpico_static)
    message(STATUS "Ignore zenoh module")
    return()
  endif()
  message(STATUS "Use zenoh-pico")
else()
  if(NOT TARGET zenohc::lib)
    find_package(zenohc CONFIG QUIET)
  endif()

  if(NOT TARGET zenohc::lib)
    message(STATUS "Ignore zenoh module")
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
if(ENABLE_ZENOH_PICO)
  if(TARGET zenohpico::zenohpico_shared)
    target_link_libraries(${PROJECT_NAME} PRIVATE zenohpico::zenohpico_shared)
  elseif(TARGET zenohpico::zenohpico_static)
    target_link_libraries(${PROJECT_NAME} PRIVATE zenohpico::zenohpico_static)
  endif()
  target_compile_definitions(${PROJECT_NAME} PRIVATE "VLINK_ENABLE_ZENOH_PICO")
else()
  target_link_libraries(${PROJECT_NAME} PRIVATE zenohc::lib)
endif()

target_compile_definitions(
  ${PROJECT_NAME}
  PRIVATE "${VLINK_LIB_DEFINES}"
  PUBLIC "VLINK_SUPPORT_ZENOH"
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
