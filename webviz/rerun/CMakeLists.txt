#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(vlink-rerun LANGUAGES CXX)

set(RERUN_SDK_DIR
    "$ENV{RERUN_SDK_DIR}"
    CACHE PATH "Rerun SDK source directory or installation prefix"
)

function(vlink_add_rerun_sdk)
  set(BUILD_SHARED_LIBS OFF)
  add_subdirectory("${RERUN_SDK_DIR}" "${CMAKE_BINARY_DIR}/rerun-sdk-build" EXCLUDE_FROM_ALL)
endfunction()

if(NOT TARGET rerun_sdk)
  if(RERUN_SDK_DIR)
    if(EXISTS "${RERUN_SDK_DIR}/src/rerun.hpp" AND EXISTS "${RERUN_SDK_DIR}/CMakeLists.txt")
      if(CMAKE_VERSION VERSION_LESS 3.16)
        message(FATAL_ERROR "Rerun C++ SDK source bundles require CMake 3.16 or newer; use an installed SDK package")
      endif()

      vlink_add_rerun_sdk()
    endif()
  endif()

  if(NOT TARGET rerun_sdk)
    if(CMAKE_VERSION VERSION_LESS 3.18)
      find_package(Arrow QUIET)

      foreach(VLINK_RERUN_ARROW_TARGET IN ITEMS Arrow::arrow_shared Arrow::arrow_static)
        if(TARGET ${VLINK_RERUN_ARROW_TARGET})
          get_target_property(VLINK_RERUN_ARROW_IMPORTED ${VLINK_RERUN_ARROW_TARGET} IMPORTED)
          get_target_property(VLINK_RERUN_ARROW_GLOBAL ${VLINK_RERUN_ARROW_TARGET} IMPORTED_GLOBAL)

          if(VLINK_RERUN_ARROW_IMPORTED AND NOT VLINK_RERUN_ARROW_GLOBAL)
            set_property(TARGET ${VLINK_RERUN_ARROW_TARGET} PROPERTY IMPORTED_GLOBAL TRUE)
          endif()
        endif()
      endforeach()

      unset(VLINK_RERUN_ARROW_GLOBAL)
      unset(VLINK_RERUN_ARROW_IMPORTED)
      unset(VLINK_RERUN_ARROW_TARGET)
    endif()

    if(RERUN_SDK_DIR)
      find_package(rerun_sdk CONFIG REQUIRED PATHS "${RERUN_SDK_DIR}" NO_DEFAULT_PATH)
    else()
      find_package(rerun_sdk CONFIG REQUIRED)
    endif()
  endif()
endif()

# src
find_package(Python3 REQUIRED COMPONENTS Interpreter)
get_target_property(RERUN_INCLUDE_HINTS rerun_sdk INTERFACE_INCLUDE_DIRECTORIES)
find_path(VLINK_RERUN_INCLUDE_DIR rerun/archetypes/points3d.hpp HINTS "${RERUN_SDK_DIR}/src" "${RERUN_SDK_DIR}/include"
                                                                      ${RERUN_INCLUDE_HINTS}
)
if(NOT VLINK_RERUN_INCLUDE_DIR)
  message(FATAL_ERROR "Cannot locate Rerun SDK archetype declarations")
endif()
file(GLOB RERUN_SCHEMA_HEADERS CONFIGURE_DEPENDS "${VLINK_RERUN_INCLUDE_DIR}/rerun/archetypes/*.hpp"
     "${VLINK_RERUN_INCLUDE_DIR}/rerun/components/*.hpp" "${VLINK_RERUN_INCLUDE_DIR}/rerun/encodings/*.hpp"
)
set(RERUN_SCHEMA_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/rerun_schema.cc")
add_custom_command(
  OUTPUT "${RERUN_SCHEMA_SOURCE}"
  COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/generate_registry.py" "${VLINK_RERUN_INCLUDE_DIR}/rerun"
          "${RERUN_SCHEMA_SOURCE}"
  DEPENDS "${PROJECT_SOURCE_DIR}/generate_registry.py" ${RERUN_SCHEMA_HEADERS}
  VERBATIM
)
file(GLOB PROJECT_HDRS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/*.h)
file(GLOB PROJECT_SRCS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/*.cc)

set(CONVERSION_SRCS ${PROJECT_SOURCE_DIR}/src/rerun_converter.cc ${PROJECT_SOURCE_DIR}/src/rerun_writer.cc
                    ${PROJECT_SOURCE_DIR}/src/rerun_native.cc
)
list(REMOVE_ITEM PROJECT_SRCS ${CONVERSION_SRCS})
add_library(vlink-rerun-conversion STATIC ${CONVERSION_SRCS} ${RERUN_SCHEMA_SOURCE})
target_include_directories(vlink-rerun-conversion PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(vlink-rerun-conversion PUBLIC vlink-webviz-common rerun_sdk)
if(TARGET rerun_arrow_target)
  set(VLINK_RERUN_ARROW_TARGET rerun_arrow_target)
else()
  find_package(Arrow REQUIRED)
  if(RERUN_ARROW_LINK_SHARED)
    set(VLINK_RERUN_ARROW_TARGET Arrow::arrow_shared)
  else()
    set(VLINK_RERUN_ARROW_TARGET Arrow::arrow_static)
  endif()
endif()
target_link_libraries(vlink-rerun-conversion PUBLIC ${VLINK_RERUN_ARROW_TARGET})

add_executable(${PROJECT_NAME} ${PROJECT_HDRS} ${PROJECT_SRCS})
target_link_libraries(${PROJECT_NAME} PRIVATE vlink-rerun-conversion)

target_include_directories(${PROJECT_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(
  ${PROJECT_NAME} PRIVATE vlink-webviz-common ${protobuf_libs} ${flatbuffers_libs} thirdparty::json
                          thirdparty::argparse rerun_sdk
)
vlink_target_system_includes_from(${PROJECT_NAME} ${protobuf_libs})
vlink_target_system_includes_from(${PROJECT_NAME} ${flatbuffers_libs})

if(WIN32)
  target_compile_definitions(${PROJECT_NAME} PRIVATE "WIN32_LEAN_AND_MEAN" "NOMINMAX")
  if(EXISTS ${CMAKE_SOURCE_DIR}/packup/ico/vlink-cmd.rc)
    target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/packup/ico/vlink-cmd.rc)
  endif()
  if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /wd4127)
  endif()
endif()

# install
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# symlinks
if(ENABLE_SYMLINKS)
  vlink_install_symlink(webviz_rerun SYMLINK ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# completions
if(ENABLE_COMPLETIONS)
  if(ENABLE_SYMLINKS)
    vlink_install_completions(${PROJECT_NAME} ALIAS webviz_rerun)
  else()
    vlink_install_completions(${PROJECT_NAME})
  endif()
endif()

# cli
add_subdirectory(cli)

# etc
if(EXISTS ${PROJECT_SOURCE_DIR}/etc)
  add_custom_command(
    TARGET ${PROJECT_NAME}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/etc/
            ${CMAKE_BINARY_DIR}/output/${BUILD_CONFIG_DIR}/vlink-rerun/
  )
  install(
    DIRECTORY ${PROJECT_SOURCE_DIR}/etc/
    DESTINATION ${INSTALL_CONFIG_DIR}/vlink-rerun/
    REGEX "completions" EXCLUDE
  )
endif()
