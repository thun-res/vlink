#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

set(VLINK_MODULE_NAME dds)
set(VLINK_MODULE_IMPORT_ALL FALSE)

project(vlink-${VLINK_MODULE_NAME} LANGUAGES CXX)

# options
option(SKIP_DDS "Skip dds module" OFF)

if(SKIP_DDS)
  message(STATUS "Skip dds module")
  return()
endif()

# requires
if(NOT TARGET fastrtps AND NOT TARGET fastdds)
  find_package(fastdds CONFIG QUIET)
  find_package(fastrtps CONFIG QUIET)
endif()

if(NOT TARGET fastrtps AND NOT TARGET fastdds)
  message(STATUS "Ignore dds module")
  return()
endif()

# rt-libs
list(APPEND VLINK_LIBRARIES ${PROJECT_NAMESPACE}${VLINK_MODULE_NAME})
set(VLINK_LIBRARIES
    ${VLINK_LIBRARIES}
    PARENT_SCOPE
)

# src
if(TARGET fastdds)
  file(GLOB PROJECT_HDRS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/*.h ${PROJECT_SOURCE_DIR}/builtin/3_x/*.hpp
       ${PROJECT_SOURCE_DIR}/builtin/3_x/*.ipp
  )
  file(GLOB PROJECT_SRCS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/*.cc ${PROJECT_SOURCE_DIR}/builtin/3_x/*.cxx)
else()
  file(GLOB PROJECT_HDRS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/*.h ${PROJECT_SOURCE_DIR}/builtin/2_x/*.hpp)
  file(GLOB PROJECT_SRCS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/*.cc ${PROJECT_SOURCE_DIR}/builtin/2_x/*.cxx)
endif()
add_library(${PROJECT_NAME} ${PROJECT_HDRS} ${PROJECT_SRCS})
add_library(${PROJECT_NAMESPACE}${PROJECT_NAME} ALIAS ${PROJECT_NAME})
add_library(${PROJECT_NAMESPACE}${VLINK_MODULE_NAME} ALIAS ${PROJECT_NAME})
target_link_libraries(${PROJECT_NAME} PUBLIC vlink::vlink)
if(TARGET fastdds)
  target_link_libraries(${PROJECT_NAME} PRIVATE fastdds)
  target_compile_definitions(${PROJECT_NAME} PRIVATE "VLINK_SUPPORT_DDS_V3")
  message(STATUS "Use fastdds v3")
else()
  target_link_libraries(${PROJECT_NAME} PRIVATE fastrtps)
  message(STATUS "Use fastdds v2")
endif()
target_compile_definitions(
  ${PROJECT_NAME}
  PRIVATE "${VLINK_LIB_DEFINES}"
  PUBLIC "VLINK_SUPPORT_DDS"
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
