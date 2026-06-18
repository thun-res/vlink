#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

set(VLINK_MODULE_NAME shm)
set(VLINK_MODULE_IMPORT_ALL FALSE)

project(vlink-${VLINK_MODULE_NAME} LANGUAGES CXX)

# options
option(SKIP_SHM "Skip shm module" OFF)
option(ENABLE_IOX_ROUDI "Install iox-roudi" OFF)

if(SKIP_SHM)
  message(STATUS "Skip shm module")
  return()
endif()

# requires
if(NOT TARGET iceoryx_posh::iceoryx_posh)
  find_package(iceoryx CONFIG QUIET)
  find_package(iceoryx_posh CONFIG QUIET)
endif()

if(ANDROID OR NOT TARGET iceoryx_posh::iceoryx_posh)
  message(STATUS "Ignore shm module")
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
target_link_libraries(${PROJECT_NAME} PRIVATE iceoryx_posh::iceoryx_posh)

if(TARGET iceoryx_posh::iceoryx_posh_roudi AND TARGET iceoryx_posh::iceoryx_posh_config)
  target_link_libraries(${PROJECT_NAME} PRIVATE iceoryx_posh::iceoryx_posh_roudi iceoryx_posh::iceoryx_posh_config)
  target_compile_definitions(${PROJECT_NAME} PRIVATE "VLINK_SUPPORT_SHM_ROUDI")
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(${PROJECT_NAME} PRIVATE "-Wl,--allow-multiple-definition")
  endif()
endif()

if(WIN32)
  target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32)
endif()

target_compile_definitions(
  ${PROJECT_NAME}
  PRIVATE "${VLINK_LIB_DEFINES}"
  PUBLIC "VLINK_SUPPORT_SHM"
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

# iox-roudi
if(ENABLE_IOX_ROUDI)
  if(TARGET iceoryx_posh::iox-roudi)
    get_target_property(ROUDI_EXECUTABLE iceoryx_posh::iox-roudi IMPORTED_LOCATION_RELEASE)
    if(NOT EXISTS "${ROUDI_EXECUTABLE}")
      get_target_property(ROUDI_EXECUTABLE iceoryx_posh::iox-roudi IMPORTED_LOCATION_RELWITHDEBINFO)
    endif()
    if(NOT EXISTS "${ROUDI_EXECUTABLE}")
      get_target_property(ROUDI_EXECUTABLE iceoryx_posh::iox-roudi IMPORTED_LOCATION_MINSIZEREL)
    endif()
    if(NOT EXISTS "${ROUDI_EXECUTABLE}")
      get_target_property(ROUDI_EXECUTABLE iceoryx_posh::iox-roudi IMPORTED_LOCATION_DEBUG)
    endif()
    if(NOT EXISTS "${ROUDI_EXECUTABLE}")
      get_target_property(ROUDI_EXECUTABLE iceoryx_posh::iox-roudi IMPORTED_LOCATION_NOCONFIG)
    endif()
    if(EXISTS "${ROUDI_EXECUTABLE}")
      add_dependencies(${PROJECT_NAME} iceoryx_posh::iox-roudi)
      add_custom_command(
        TARGET ${PROJECT_NAME}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "${ROUDI_EXECUTABLE}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
      )
    endif()
  elseif(iceoryx_PACKAGE_FOLDER_RELEASE)
    if(WIN32)
      add_custom_command(
        TARGET ${PROJECT_NAME}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "${iceoryx_PACKAGE_FOLDER_RELEASE}/bin/iox-roudi.exe"
                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
      )
    else()
      add_custom_command(
        TARGET ${PROJECT_NAME}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "${iceoryx_PACKAGE_FOLDER_RELEASE}/bin/iox-roudi"
                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
      )
    endif()
  endif()
endif()
