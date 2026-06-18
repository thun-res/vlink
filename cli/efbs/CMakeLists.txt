#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

project(vlink-efbs LANGUAGES CXX)

# requires
if(NOT TARGET flatbuffers::flatbuffers_shared AND NOT TARGET flatbuffers::flatbuffers)
  find_package(flatbuffers CONFIG REQUIRED)
endif()

# src
add_executable(${PROJECT_NAME} efbs.cc)
target_link_libraries(${PROJECT_NAME} PRIVATE vlink::vlink ${VLINK_NO_INTRA_LIBRARIES})
target_link_libraries(${PROJECT_NAME} PRIVATE thirdparty::argparse thirdparty::json)
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
  vlink_install_symlink(efbs SYMLINK ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# completions
if(ENABLE_COMPLETIONS)
  if(ENABLE_SYMLINKS)
    vlink_install_completions(${PROJECT_NAME} ALIAS efbs)
  else()
    vlink_install_completions(${PROJECT_NAME})
  endif()
endif()
