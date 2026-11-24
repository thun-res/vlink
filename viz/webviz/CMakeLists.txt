#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(vlink-webviz)

option(ENABLE_WEBVIZ_FOXGLOVE "Enable Foxglove for webviz" ON)
option(ENABLE_WEBVIZ_RERUN "Enable Rerun for webviz" OFF)

if(NOT TARGET vlink)
  find_package(
    vlink
    COMPONENTS ALL
    REQUIRED
  )
endif()

if(NOT TARGET vlink-proxy_api)
  find_package(vlink-proxy_api REQUIRED)
endif()

if(NOT TARGET vlink-proxy_server)
  find_package(vlink-proxy_server REQUIRED)
endif()

if(NOT TARGET protobuf::libprotobuf)
  find_package(Protobuf CONFIG QUIET)

  if(NOT TARGET protobuf::libprotobuf)
    find_package(Protobuf REQUIRED)
  endif()
endif()

if(TARGET protobuf::libprotobuf)
  set(protobuf_libs protobuf::libprotobuf)
endif()

if(NOT TARGET flatbuffers::flatbuffers_shared AND NOT TARGET flatbuffers::flatbuffers)
  find_package(flatbuffers CONFIG QUIET)
endif()

if(TARGET flatbuffers::flatbuffers_shared)
  set(flatbuffers_libs flatbuffers::flatbuffers_shared)
elseif(TARGET flatbuffers::flatbuffers)
  set(flatbuffers_libs flatbuffers::flatbuffers)
endif()

if(NOT protobuf_libs OR NOT flatbuffers_libs)
  message(WARNING "Can not find proto or flatbuffers, skipped.")
  return()
endif()

file(GLOB PROJECT_COMMON_HDRS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/*.h)
file(GLOB PROJECT_COMMON_SRCS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/*.cc)

add_library(vlink-webviz-common OBJECT ${PROJECT_COMMON_HDRS} ${PROJECT_COMMON_SRCS})

target_include_directories(vlink-webviz-common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(
  vlink-webviz-common
  PUBLIC vlink::all
         vlink::proxy_api
         vlink::proxy_server
         ${protobuf_libs}
         ${flatbuffers_libs}
         thirdparty::json
         thirdparty::argparse
)

vlink_target_system_includes_from(vlink-webviz-common ${protobuf_libs})

if(NOT DEFINED ENABLE_EXPRTK OR ENABLE_EXPRTK)
  if(NOT TARGET vlink::exprtk_api)
    find_package(vlink-exprtk_api QUIET)
  endif()

  if(TARGET vlink::exprtk_api)
    target_link_libraries(vlink-webviz-common PUBLIC vlink::exprtk_api)
  endif()
endif()

if(ENABLE_WEBVIZ_FOXGLOVE)
  add_subdirectory(foxglove)
endif()

if(ENABLE_WEBVIZ_RERUN)
  add_subdirectory(rerun)
endif()

# symlinks
if(ENABLE_SYMLINKS)
  if(ENABLE_WEBVIZ_FOXGLOVE AND TARGET vlink-foxglove)
    vlink_install_symlink(webviz SYMLINK vlink-foxglove DESTINATION ${CMAKE_INSTALL_BINDIR})
  elseif(ENABLE_WEBVIZ_RERUN AND TARGET vlink-rerun)
    vlink_install_symlink(webviz SYMLINK vlink-rerun DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
endif()
