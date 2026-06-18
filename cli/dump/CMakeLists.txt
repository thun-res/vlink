#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

project(vlink-dump LANGUAGES CXX)

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
  dump_cli.h
  dump_context.h
  dump_expr.h
  dump_extract.h
  dump_features.h
  dump_path.h
  dump_plan.h
  dump_proto_cache.h
  dump_schema.h
  dump_slice.h
  dump_types.h
  dump_validate.h
  dump.cc
  dump_context.cc
  dump_expr.cc
  dump_extract.cc
  dump_path.cc
  dump_plan.cc
  dump_proto_cache.cc
  dump_schema.cc
  dump_slice.cc
  dump_validate.cc
)
target_link_libraries(${PROJECT_NAME} PRIVATE vlink::vlink ${VLINK_NO_INTRA_LIBRARIES})
target_link_libraries(${PROJECT_NAME} PRIVATE thirdparty::argparse thirdparty::json protobuf::libprotobuf)

if(NOT TARGET vlink-exprtk_api)
  find_package(vlink-exprtk_api QUIET)
endif()

if(TARGET vlink::exprtk_api)
  target_link_libraries(${PROJECT_NAME} PRIVATE vlink::exprtk_api)
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
  vlink_install_symlink(dump SYMLINK ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# completions
if(ENABLE_COMPLETIONS)
  if(ENABLE_SYMLINKS)
    vlink_install_completions(${PROJECT_NAME} ALIAS dump)
  else()
    vlink_install_completions(${PROJECT_NAME})
  endif()
endif()
