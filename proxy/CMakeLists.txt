#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

project(vlink-proxy)

# proxy_api
add_subdirectory(proxy_api)

# proxy_server
add_subdirectory(proxy_server)

# proxy_app
add_executable(${PROJECT_NAME} proxy_common.h proxy.cc)
target_link_libraries(${PROJECT_NAME} PRIVATE vlink::vlink vlink::all)
target_link_libraries(${PROJECT_NAME} PRIVATE thirdparty::bitsery thirdparty::argparse vlink::proxy_server)

if(WIN32)
  if(EXISTS ${CMAKE_SOURCE_DIR}/packup/ico/vlink-cmd.rc)
    target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/packup/ico/vlink-cmd.rc)
  endif()
endif()

# install
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# symlinks
if(ENABLE_SYMLINKS)
  vlink_install_symlink(proxy SYMLINK ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# completions
if(ENABLE_COMPLETIONS)
  if(ENABLE_SYMLINKS)
    vlink_install_completions(${PROJECT_NAME} ALIAS proxy)
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
            ${CMAKE_BINARY_DIR}/output/${BUILD_CONFIG_DIR}/vlink-proxy/
  )
  install(
    DIRECTORY ${PROJECT_SOURCE_DIR}/etc/
    DESTINATION ${INSTALL_CONFIG_DIR}/vlink-proxy/
    REGEX "completions" EXCLUDE
  )
endif()
