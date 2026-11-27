#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

project(vlink-launcher LANGUAGES CXX)

# launcher_api
add_subdirectory(launcher_api)

# launcher_server
add_subdirectory(launcher_server)

# launcher_app
add_executable(${PROJECT_NAME} launcher_common.h launcher.cc)
target_link_libraries(${PROJECT_NAME} PRIVATE vlink::vlink vlink::all vlink::launcher_server)
target_link_libraries(${PROJECT_NAME} PRIVATE thirdparty::bitsery)

if(WIN32)
  if(EXISTS ${CMAKE_SOURCE_DIR}/packup/ico/vlink-cmd.rc)
    target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/packup/ico/vlink-cmd.rc)
  endif()
endif()

# install
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# symlinks
if(ENABLE_SYMLINKS)
  vlink_install_symlink(launcher SYMLINK ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# completions
if(ENABLE_COMPLETIONS)
  if(ENABLE_SYMLINKS)
    vlink_install_completions(${PROJECT_NAME} ALIAS launcher)
  else()
    vlink_install_completions(${PROJECT_NAME})
  endif()
endif()

# etc
if(EXISTS ${PROJECT_SOURCE_DIR}/etc)
  add_custom_command(
    TARGET ${PROJECT_NAME}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/etc/
            ${CMAKE_BINARY_DIR}/output/${BUILD_CONFIG_DIR}/vlink-launcher/
  )
  install(
    DIRECTORY ${PROJECT_SOURCE_DIR}/etc/
    DESTINATION ${INSTALL_CONFIG_DIR}/vlink-launcher/
    REGEX "completions" EXCLUDE
  )
endif()
