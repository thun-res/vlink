#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

project(vlink-check LANGUAGES CXX)

# src
add_executable(${PROJECT_NAME} check.cc)
target_link_libraries(${PROJECT_NAME} PRIVATE vlink::vlink vlink::all)
target_link_libraries(${PROJECT_NAME} PRIVATE thirdparty::argparse)

# Propagate CLI-tool enable flags so that env_list / process_checks can hide entries for tools that are not built.
# Module-level VLINK_SUPPORT_* macros are already PUBLIC on each module target and arrive through vlink::all.
if(ENABLE_CLI_BENCH)
  target_compile_definitions(${PROJECT_NAME} PRIVATE "VLINK_ENABLE_CLI_BENCH")
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
  vlink_install_symlink(check SYMLINK ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# completions
if(ENABLE_COMPLETIONS)
  if(ENABLE_SYMLINKS)
    vlink_install_completions(${PROJECT_NAME} ALIAS check)
  else()
    vlink_install_completions(${PROJECT_NAME})
  endif()
endif()
