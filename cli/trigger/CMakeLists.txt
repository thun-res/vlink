#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

project(vlink-trigger LANGUAGES CXX)

# src
add_executable(${PROJECT_NAME} trigger.cc)
target_link_libraries(${PROJECT_NAME} PRIVATE vlink::vlink ${VLINK_NO_INTRA_LIBRARIES})
target_link_libraries(${PROJECT_NAME} PRIVATE thirdparty::argparse thirdparty::json)

if(WIN32)
  if(EXISTS ${CMAKE_SOURCE_DIR}/packup/ico/vlink-cmd.rc)
    target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/packup/ico/vlink-cmd.rc)
  endif()
endif()

# install
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(
  DIRECTORY ${PROJECT_SOURCE_DIR}/etc/
  DESTINATION ${INSTALL_CONFIG_DIR}/trigger
  FILES_MATCHING
  PATTERN "*.json*"
  PATTERN "completions" EXCLUDE
)

# symlinks
if(ENABLE_SYMLINKS)
  vlink_install_symlink(trigger SYMLINK ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# completions
if(ENABLE_COMPLETIONS)
  if(ENABLE_SYMLINKS)
    vlink_install_completions(${PROJECT_NAME} ALIAS trigger)
  else()
    vlink_install_completions(${PROJECT_NAME})
  endif()
endif()
