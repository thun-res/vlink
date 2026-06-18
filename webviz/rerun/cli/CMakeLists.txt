#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

project(vlink-bag2rrd LANGUAGES CXX)

# src
add_executable(${PROJECT_NAME} bag2rrd.cc)
target_include_directories(${PROJECT_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/../src ${PROJECT_SOURCE_DIR}/../..)
target_sources(
  ${PROJECT_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/../src/rerun_converter.cc
                          ${PROJECT_SOURCE_DIR}/../src/rerun_converter.h
)
target_link_libraries(
  ${PROJECT_NAME}
  PRIVATE vlink::vlink
          vlink::proxy_api
          ${protobuf_libs}
          ${flatbuffers_libs}
          thirdparty::json
          thirdparty::argparse
          rerun_sdk
)
vlink_target_system_includes_from(${PROJECT_NAME} ${protobuf_libs})
vlink_target_system_includes_from(${PROJECT_NAME} ${flatbuffers_libs})

if(NOT TARGET vlink-exprtk_api)
  find_package(vlink-exprtk_api QUIET)
endif()

if(TARGET vlink::exprtk_api)
  target_link_libraries(${PROJECT_NAME} PRIVATE vlink::exprtk_api)
endif()
if(WIN32)
  if(EXISTS ${CMAKE_SOURCE_DIR}/packup/ico/vlink-cmd.rc)
    target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/packup/ico/vlink-cmd.rc)
  endif()
endif()

# install
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# symlinks
if(ENABLE_SYMLINKS)
  vlink_install_symlink(bag2rrd SYMLINK ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# completions
if(ENABLE_COMPLETIONS)
  if(ENABLE_SYMLINKS)
    vlink_install_completions(${PROJECT_NAME} ALIAS bag2rrd)
  else()
    vlink_install_completions(${PROJECT_NAME})
  endif()
endif()
