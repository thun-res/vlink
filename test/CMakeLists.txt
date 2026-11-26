#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

project(vlink-test)

# src
file(GLOB_RECURSE PROJECT_HDRS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/*.h)
file(GLOB_RECURSE PROJECT_SRCS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/*.cc ${PROJECT_SOURCE_DIR}/*.cxx)
list(FILTER PROJECT_SRCS EXCLUDE REGEX ".*/launcher/launcher_(test|legacy|logger)_plugin\\.cc$")
if(NOT ENABLE_LAUNCHER)
  list(FILTER PROJECT_SRCS EXCLUDE REGEX ".*/launcher/.*")
endif()
add_executable(${PROJECT_NAME} ${PROJECT_HDRS} ${PROJECT_SRCS})
target_include_directories(${PROJECT_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/idl)
target_link_libraries(${PROJECT_NAME} PRIVATE vlink::vlink vlink::all)
target_link_libraries(${PROJECT_NAME} PRIVATE thirdparty::doctest thirdparty::json)
target_compile_definitions(${PROJECT_NAME} PRIVATE ${VLINK_APP_DEFINES})
target_compile_definitions(${PROJECT_NAME} PRIVATE "DOCTEST_CONFIG_USE_STD_HEADERS")
target_compile_definitions(${PROJECT_NAME} PRIVATE VLINK_TEST_IDL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/idl")
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_CLANG_TIDY "")

if(MSVC)
  target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
endif()

if(WIN32)
  if(EXISTS ${CMAKE_SOURCE_DIR}/packup/ico/vlink-cmd.rc)
    target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/packup/ico/vlink-cmd.rc)
  endif()
endif()

if(ENABLE_SQLITE)
  target_compile_definitions(${PROJECT_NAME} PRIVATE "VLINK_TEST_SUPPORT_SQLITE")
  if(TARGET SQLite3::SQLite3)
    target_link_libraries(${PROJECT_NAME} PRIVATE SQLite3::SQLite3)
  else()
    target_link_libraries(${PROJECT_NAME} PRIVATE SQLite::SQLite3)
  endif()
endif()

if(ENABLE_SECURITY)
  target_compile_definitions(${PROJECT_NAME} PRIVATE "VLINK_TEST_SUPPORT_SECURITY")
  if(TARGET OpenSSL::Crypto)
    target_link_libraries(${PROJECT_NAME} PRIVATE OpenSSL::Crypto)
  endif()
endif()

if(ENABLE_LAUNCHER AND TARGET vlink::launcher_server)
  target_link_libraries(${PROJECT_NAME} PRIVATE vlink::launcher_server)

  add_library(launcher_test_plugin SHARED ${PROJECT_SOURCE_DIR}/launcher/launcher_test_plugin.cc)
  target_link_libraries(launcher_test_plugin PRIVATE vlink::all)
  set_target_properties(
    launcher_test_plugin
    PROPERTIES CXX_CLANG_TIDY ""
               RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
               LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
  )

  add_library(launcher_legacy_plugin SHARED ${PROJECT_SOURCE_DIR}/launcher/launcher_legacy_plugin.cc)
  target_link_libraries(launcher_legacy_plugin PRIVATE vlink::vlink)
  set_target_properties(
    launcher_legacy_plugin
    PROPERTIES CXX_CLANG_TIDY ""
               RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
               LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
  )

  add_library(launcher_logger_plugin SHARED ${PROJECT_SOURCE_DIR}/launcher/launcher_logger_plugin.cc)
  target_link_libraries(launcher_logger_plugin PRIVATE vlink::vlink)
  set_target_properties(
    launcher_logger_plugin
    PROPERTIES CXX_CLANG_TIDY ""
               RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
               LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
  )

  foreach(OUTPUTCONFIG DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    if(DEFINED CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG})
      set(_test_plugin_output_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG}}")
    else()
      set(_test_plugin_output_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
    endif()
    set_target_properties(
      launcher_test_plugin launcher_legacy_plugin launcher_logger_plugin
      PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG} "${_test_plugin_output_dir}"
                 LIBRARY_OUTPUT_DIRECTORY_${OUTPUTCONFIG} "${_test_plugin_output_dir}"
    )
  endforeach()

  add_dependencies(${PROJECT_NAME} launcher_test_plugin launcher_legacy_plugin launcher_logger_plugin)
  if(TARGET vlink-launcher_logd)
    add_dependencies(${PROJECT_NAME} vlink-launcher_logd)
  endif()
endif()

# test
set(_VLINK_TEST_SUITES "")
set(_VLINK_TRANSPORT_TEST_SUITES "")
foreach(_SRC IN LISTS PROJECT_SRCS)
  get_filename_component(_SRC_NAME "${_SRC}" NAME_WE)
  string(REGEX REPLACE "_test$" "" _SRC_NAME "${_SRC_NAME}")
  string(REGEX REPLACE "_transport$" "" _SRC_NAME "${_SRC_NAME}")
  if(_SRC_NAME STREQUAL "intra_data")
    set(_SRC_NAME intra)
  elseif(_SRC_NAME STREQUAL "coroutine" AND NOT ENABLE_CXX_STD_20)
    continue()
  endif()
  if(EXISTS "${CMAKE_SOURCE_DIR}/src/transports/${_SRC_NAME}/CMakeLists.txt")
    if(NOT "${PROJECT_NAMESPACE}${_SRC_NAME}" IN_LIST VLINK_LIBRARIES)
      continue()
    endif()
  endif()
  file(STRINGS ${_SRC} _SUITE_LINES REGEX "TEST_SUITE\\(\"[^\"]+\"\\)")
  foreach(_LINE IN LISTS _SUITE_LINES)
    string(REGEX REPLACE ".*TEST_SUITE\\(\"([^\"]+)\"\\).*" "\\1" _SUITE_NAME "${_LINE}")
    if(_SUITE_NAME AND NOT _SUITE_NAME MATCHES "^[ \t]*$")
      list(APPEND _VLINK_TEST_SUITES "${_SUITE_NAME}")
    endif()
  endforeach()
endforeach()
list(REMOVE_DUPLICATES _VLINK_TEST_SUITES)
list(SORT _VLINK_TEST_SUITES)
foreach(_SUITE IN LISTS _VLINK_TEST_SUITES)
  add_test(NAME "${_SUITE}" COMMAND ${PROJECT_NAME} "--test-suite=${_SUITE}" "--no-intro=1")
  if(_SUITE MATCHES "^([^-]+)-")
    set(_SUITE_TRANSPORT "${CMAKE_MATCH_1}")
    if("${PROJECT_NAMESPACE}${_SUITE_TRANSPORT}" IN_LIST VLINK_LIBRARIES)
      list(APPEND _VLINK_TRANSPORT_TEST_SUITES "${_SUITE}")
    endif()
  endif()
endforeach()
if(NOT _VLINK_TEST_SUITES)
  add_test(NAME ${PROJECT_NAME} COMMAND ${PROJECT_NAME})
  set(_VLINK_TEST_SUITES ${PROJECT_NAME})
endif()

set(_VLINK_TEST_ENV "VLINK_DDS_IP=127.0.0.1")
if(UNIX AND NOT APPLE)
  set(_VLINK_TEST_LIBRARY_PATH LD_LIBRARY_PATH)
elseif(APPLE)
  set(_VLINK_TEST_LIBRARY_PATH DYLD_LIBRARY_PATH)
endif()
if(_VLINK_TEST_LIBRARY_PATH)
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.22)
    set_tests_properties(
      ${_VLINK_TEST_SUITES}
      PROPERTIES ENVIRONMENT_MODIFICATION
                 "${_VLINK_TEST_LIBRARY_PATH}=path_list_prepend:${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
    )
  else()
    list(APPEND _VLINK_TEST_ENV
         "${_VLINK_TEST_LIBRARY_PATH}=${CMAKE_LIBRARY_OUTPUT_DIRECTORY}:$ENV{${_VLINK_TEST_LIBRARY_PATH}}"
    )
  endif()
endif()
set_tests_properties(${_VLINK_TEST_SUITES} PROPERTIES ENVIRONMENT "${_VLINK_TEST_ENV}")

if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.16)
  set_tests_properties(
    ${_VLINK_TEST_SUITES} PROPERTIES SKIP_REGULAR_EXPRESSION
                                     "(^|\n)\\[doctest\\] test cases: +0 +\\| +0 passed +\\| +0 failed"
  )
endif()

if(_VLINK_TRANSPORT_TEST_SUITES)
  set_tests_properties(${_VLINK_TRANSPORT_TEST_SUITES} PROPERTIES RESOURCE_LOCK vlink_transport)
endif()

# requires
if(ENABLE_ZENOH_PICO)
  set_source_files_properties(zenoh_test.cc PROPERTIES COMPILE_DEFINITIONS VLINK_ENABLE_ZENOH_PICO)
endif()

if(TARGET vlink-fdbus)
  target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE "${FDBUS_INCLUDE_DIR}")
  target_link_libraries(${PROJECT_NAME} PRIVATE "${FDBUS_LIBRARY}")
endif()

if(NOT Protobuf_FOUND)
  find_package(Protobuf CONFIG QUIET)
endif()

if(NOT flatbuffers_FOUND)
  find_package(flatbuffers CONFIG QUIET)
endif()

set(ENABLE_FASTDDS_GEN_TEST OFF)

# idl(fastdds-gen)
if(ENABLE_FASTDDS_GEN_TEST AND TARGET vlink-dds)
  find_program(FASTDDS_GEN_EXECUTABLE NAMES fastddsgen)
  if(FASTDDS_GEN_EXECUTABLE)
    file(GLOB IDL_SRCS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/idl/*.idl)
    vlink_generate_cpp(TARGET sample_gen_fastdds DDS ${IDL_SRCS})
    target_link_libraries(${PROJECT_NAME} PRIVATE sample_gen_fastdds)
    target_compile_definitions(${PROJECT_NAME} PRIVATE "VLINK_TEST_SUPPORT_FASTDDS_GEN")
  endif()
endif()

if(Protobuf_FOUND)
  file(GLOB IDL_SRCS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/idl/*.proto)
  vlink_generate_cpp(TARGET sample_gen_protobuf PROTO ${IDL_SRCS})
  target_link_libraries(${PROJECT_NAME} PRIVATE sample_gen_protobuf)
  vlink_target_system_includes_from(${PROJECT_NAME} protobuf::libprotobuf)
  target_compile_definitions(${PROJECT_NAME} PRIVATE "VLINK_TEST_SUPPORT_PROTOBUF")
endif()

if(flatbuffers_FOUND)
  file(GLOB IDL_SRCS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/idl/*.fbs)
  vlink_generate_cpp(TARGET sample_gen_flatbuffers FBS ${IDL_SRCS})
  target_link_libraries(${PROJECT_NAME} PRIVATE sample_gen_flatbuffers)
  vlink_target_system_includes_from(${PROJECT_NAME} flatbuffers::flatbuffers)
  vlink_target_system_includes_from(${PROJECT_NAME} flatbuffers::flatbuffers_shared)
  target_compile_definitions(${PROJECT_NAME} PRIVATE "VLINK_TEST_SUPPORT_FLATBUFFERS")
endif()

# install
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
