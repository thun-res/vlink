#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(vlink-foxglove LANGUAGES CXX)

# requires
if(NOT TARGET vlink)
  find_package(
    vlink
    COMPONENTS ALL
    REQUIRED
  )
endif()

if(NOT TARGET protobuf::libprotobuf)
  find_package(Protobuf CONFIG QUIET)
  if(NOT TARGET protobuf::libprotobuf)
    find_package(Protobuf REQUIRED)
  endif()
endif()

if(TARGET protobuf::libprotobuf)
  set(protobuf_libs protobuf::libprotobuf)
elseif(TARGET protobuf::libprotobuf-lite)
  set(protobuf_libs protobuf::libprotobuf-lite)
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

file(GLOB FOXGLOVE_FBS_FILES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/fbs/*.fbs)
vlink_generate_cpp(
  TARGET
  foxglove_fbs_gen
  FBS
  ${FOXGLOVE_FBS_FILES}
  IN_DIR
  ${PROJECT_SOURCE_DIR}/fbs
  FLAGS
  "--bfbs-gen-embed"
)

# src
file(GLOB PROJECT_HDRS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/*.h)
file(GLOB PROJECT_SRCS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/*.cc)

add_executable(${PROJECT_NAME} ${PROJECT_HDRS} ${PROJECT_SRCS})

target_include_directories(${PROJECT_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(
  ${PROJECT_NAME}
  PRIVATE vlink-webviz-common
          ${protobuf_libs}
          ${flatbuffers_libs}
          thirdparty::json
          thirdparty::websocketpp
          thirdparty::asio
          thirdparty::argparse
          $<BUILD_INTERFACE:foxglove_fbs_gen>
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
  vlink_install_symlink(webviz_foxglove SYMLINK ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# completions
if(ENABLE_COMPLETIONS)
  if(ENABLE_SYMLINKS)
    vlink_install_completions(${PROJECT_NAME} ALIAS webviz_foxglove)
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
            ${CMAKE_BINARY_DIR}/output/${BUILD_CONFIG_DIR}/vlink-foxglove/
  )
  install(
    DIRECTORY ${PROJECT_SOURCE_DIR}/etc/
    DESTINATION ${INSTALL_CONFIG_DIR}/vlink-foxglove/
    REGEX "completions" EXCLUDE
  )
endif()
