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

add_library(
  vlink-webviz-common OBJECT
  ${CMAKE_CURRENT_SOURCE_DIR}/webviz_types.h
  ${CMAKE_CURRENT_SOURCE_DIR}/webviz_common.h
  ${CMAKE_CURRENT_SOURCE_DIR}/webviz_app_utils.h
  ${CMAKE_CURRENT_SOURCE_DIR}/webviz_bridge_utils.h
  ${CMAKE_CURRENT_SOURCE_DIR}/webviz_loader_utils.h
  ${CMAKE_CURRENT_SOURCE_DIR}/webviz_time_utils.h
  ${CMAKE_CURRENT_SOURCE_DIR}/zerocopy_message.h
  ${CMAKE_CURRENT_SOURCE_DIR}/zerocopy_message.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/proxy_bridge.h
  ${CMAKE_CURRENT_SOURCE_DIR}/proxy_bridge.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/proxy_config.h
  ${CMAKE_CURRENT_SOURCE_DIR}/proxy_config.cc
)

target_include_directories(vlink-webviz-common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(NOT TARGET vlink-exprtk_api)
  find_package(vlink-exprtk_api QUIET)
endif()

target_link_libraries(vlink-webviz-common PUBLIC vlink::all vlink::proxy_api thirdparty::json thirdparty::argparse)

if(TARGET vlink::exprtk_api)
  target_link_libraries(vlink-webviz-common PUBLIC vlink::exprtk_api)
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
