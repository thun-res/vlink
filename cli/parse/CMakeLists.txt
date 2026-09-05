#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

project(vlink-parse LANGUAGES CXX)

# requires
if(NOT TARGET protobuf::libprotobuf)
  find_package(Protobuf CONFIG QUIET)
  if(NOT TARGET protobuf::libprotobuf)
    find_package(Protobuf REQUIRED)
  endif()
endif()

# src

add_executable(
  ${PROJECT_NAME}
  parse_cli.h
  parse_context.h
  parse_expr.h
  parse_extract.h
  parse_features.h
  parse_path.h
  parse_plan.h
  parse_proto_cache.h
  parse_schema.h
  parse_slice.h
  parse_types.h
  parse_validate.h
  parse.cc
  parse_context.cc
  parse_expr.cc
  parse_extract.cc
  parse_path.cc
  parse_plan.cc
  parse_proto_cache.cc
  parse_schema.cc
  parse_slice.cc
  parse_validate.cc
)
target_link_libraries(${PROJECT_NAME} PRIVATE vlink::vlink ${VLINK_NO_INTRA_LIBRARIES})
target_link_libraries(${PROJECT_NAME} PRIVATE thirdparty::argparse thirdparty::json protobuf::libprotobuf)
vlink_target_system_includes_from(${PROJECT_NAME} protobuf::libprotobuf)

if(NOT DEFINED ENABLE_EXPRTK OR ENABLE_EXPRTK)
  if(NOT TARGET vlink::exprtk_api)
    find_package(vlink-exprtk_api QUIET)
  endif()
  if(TARGET vlink::exprtk_api)
    target_link_libraries(${PROJECT_NAME} PRIVATE vlink::exprtk_api)
  endif()
endif()

if(TARGET flatbuffers::flatbuffers_shared)
  target_link_libraries(${PROJECT_NAME} PRIVATE flatbuffers::flatbuffers_shared)
elseif(TARGET flatbuffers::flatbuffers)
  target_link_libraries(${PROJECT_NAME} PRIVATE flatbuffers::flatbuffers)
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
  vlink_install_symlink(parse SYMLINK ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# completions
if(ENABLE_COMPLETIONS)
  if(ENABLE_SYMLINKS)
    vlink_install_completions(${PROJECT_NAME} ALIAS parse)
  else()
    vlink_install_completions(${PROJECT_NAME})
  endif()
endif()
