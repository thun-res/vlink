#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

set(VLINK_MODULE_NAME shm2)
set(VLINK_MODULE_IMPORT_ALL FALSE)

project(vlink-${VLINK_MODULE_NAME} LANGUAGES CXX)

# options
option(SKIP_SHM2 "Skip shm2 module" OFF)

if(SKIP_SHM2)
  message(STATUS "Skip shm2 module")
  return()
endif()

# requires
if(NOT TARGET iceoryx2-c::shared-lib AND NOT TARGET iceoryx2-c::static-lib)
  find_package(iceoryx2-c CONFIG QUIET)
endif()

if(ANDROID OR (NOT TARGET iceoryx2-c::shared-lib AND NOT TARGET iceoryx2-c::static-lib))
  message(STATUS "Ignore shm2 module")
  return()
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
if(TARGET iceoryx2-c::shared-lib)
  target_link_libraries(${PROJECT_NAME} PRIVATE iceoryx2-c::shared-lib)
else()
  target_link_libraries(${PROJECT_NAME} PRIVATE iceoryx2-c::static-lib)
endif()

target_compile_definitions(
  ${PROJECT_NAME}
  PRIVATE "${VLINK_LIB_DEFINES}"
  PUBLIC "VLINK_SUPPORT_SHM2"
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
