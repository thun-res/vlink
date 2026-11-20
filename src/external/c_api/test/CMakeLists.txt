#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

project(vlink-c-test LANGUAGES C CXX)

include(GNUInstallDirs)

add_executable(${PROJECT_NAME} c_api_test.c c_api_schema_test.cc)
target_link_libraries(${PROJECT_NAME} PRIVATE vlink::c_api vlink::vlink)

if(ENABLE_SECURITY)
  target_compile_definitions(${PROJECT_NAME} PRIVATE "VLINK_ENABLE_SECURITY")
endif()
