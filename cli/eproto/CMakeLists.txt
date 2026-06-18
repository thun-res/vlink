#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

project(vlink-eproto LANGUAGES CXX)

# requires
if(NOT TARGET protobuf::libprotobuf)
  find_package(Protobuf CONFIG QUIET)
  if(NOT TARGET protobuf::libprotobuf)
    find_package(Protobuf REQUIRED)
  endif()
endif()

# src
add_executable(${PROJECT_NAME} eproto.cc)
target_link_libraries(${PROJECT_NAME} PRIVATE vlink::vlink ${VLINK_NO_INTRA_LIBRARIES})
target_link_libraries(${PROJECT_NAME} PRIVATE thirdparty::argparse thirdparty::json)
target_link_libraries(${PROJECT_NAME} PRIVATE protobuf::libprotobuf)

if(WIN32)
  if(EXISTS ${CMAKE_SOURCE_DIR}/packup/ico/vlink-cmd.rc)
    target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/packup/ico/vlink-cmd.rc)
  endif()
endif()

# install
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# symlinks
if(ENABLE_SYMLINKS)
  vlink_install_symlink(eproto SYMLINK ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# completions
if(ENABLE_COMPLETIONS)
  if(ENABLE_SYMLINKS)
    vlink_install_completions(${PROJECT_NAME} ALIAS eproto)
  else()
    vlink_install_completions(${PROJECT_NAME})
  endif()
endif()
