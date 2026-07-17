#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

project(vlink-test)

# src
file(GLOB_RECURSE PROJECT_HDRS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/*.h)
file(GLOB_RECURSE PROJECT_SRCS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/*.cc ${PROJECT_SOURCE_DIR}/*.cxx)
add_executable(${PROJECT_NAME} ${PROJECT_HDRS} ${PROJECT_SRCS})
target_include_directories(${PROJECT_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/idl)
target_link_libraries(${PROJECT_NAME} PRIVATE vlink::vlink vlink::all)
target_link_libraries(${PROJECT_NAME} PRIVATE thirdparty::doctest thirdparty::json)
target_compile_definitions(${PROJECT_NAME} PRIVATE ${VLINK_APP_DEFINES})
target_compile_definitions(${PROJECT_NAME} PRIVATE "DOCTEST_CONFIG_USE_STD_HEADERS")
target_compile_definitions(${PROJECT_NAME} PRIVATE VLINK_TEST_IDL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/idl")
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_CLANG_TIDY "")

if(WIN32)
  if(EXISTS ${CMAKE_SOURCE_DIR}/packup/ico/vlink-cmd.rc)
    target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/packup/ico/vlink-cmd.rc)
  endif()
endif()

if(ENABLE_SECURITY)
  target_compile_definitions(${PROJECT_NAME} PRIVATE "VLINK_TEST_SUPPORT_SECURITY")
  if(TARGET OpenSSL::Crypto)
    target_link_libraries(${PROJECT_NAME} PRIVATE OpenSSL::Crypto)
  endif()
endif()

# test
set(_VLINK_TEST_SUITES "")
set(_VLINK_TRANSPORT_TEST_SUITES "")
foreach(_SRC IN LISTS PROJECT_SRCS)
  get_filename_component(_SRC_MODULE "${_SRC}" NAME_WE)
  string(REGEX REPLACE "_test$" "" _SRC_MODULE "${_SRC_MODULE}")
  string(REGEX REPLACE "_conf$" "" _SRC_MODULE "${_SRC_MODULE}")
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/modules/${_SRC_MODULE}/CMakeLists.txt")
    file(STRINGS ${_SRC} _SRC_SUPPORT_LINES REGEX "VLINK_SUPPORT_[A-Z0-9_]+")
    set(_SRC_SUPPORT_MODULES "")
    foreach(_LINE IN LISTS _SRC_SUPPORT_LINES)
      if(_LINE MATCHES "VLINK_SUPPORT_([A-Z0-9_]+)")
        string(TOLOWER "${CMAKE_MATCH_1}" _SRC_SUPPORT_MODULE)
        list(APPEND _SRC_SUPPORT_MODULES "${_SRC_SUPPORT_MODULE}")
      endif()
    endforeach()
    if(_SRC_SUPPORT_MODULES)
      list(REMOVE_DUPLICATES _SRC_SUPPORT_MODULES)
      list(LENGTH _SRC_SUPPORT_MODULES _SRC_SUPPORT_MODULE_COUNT)
      if(_SRC_SUPPORT_MODULE_COUNT EQUAL 1)
        list(GET _SRC_SUPPORT_MODULES 0 _SRC_MODULE)
      endif()
    endif()
  endif()
  if(EXISTS "${CMAKE_SOURCE_DIR}/modules/${_SRC_MODULE}/CMakeLists.txt")
    if(NOT "${PROJECT_NAMESPACE}${_SRC_MODULE}" IN_LIST VLINK_LIBRARIES)
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
    set(_SUITE_MODULE "${CMAKE_MATCH_1}")
    if("${PROJECT_NAMESPACE}${_SUITE_MODULE}" IN_LIST VLINK_LIBRARIES)
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
  list(APPEND _VLINK_TEST_ENV "LD_LIBRARY_PATH=${CMAKE_LIBRARY_OUTPUT_DIRECTORY}:$ENV{LD_LIBRARY_PATH}")
elseif(APPLE)
  list(APPEND _VLINK_TEST_ENV "DYLD_LIBRARY_PATH=${CMAKE_LIBRARY_OUTPUT_DIRECTORY}:$ENV{DYLD_LIBRARY_PATH}")
endif()
set_tests_properties(${_VLINK_TEST_SUITES} PROPERTIES ENVIRONMENT "${_VLINK_TEST_ENV}")

if(_VLINK_TRANSPORT_TEST_SUITES)
  set_tests_properties(${_VLINK_TRANSPORT_TEST_SUITES} PROPERTIES RESOURCE_LOCK vlink_transport)
endif()

if(UNIX)
  find_program(VLINK_TEST_BASH_EXECUTABLE NAMES bash)
  if(VLINK_TEST_BASH_EXECUTABLE)
    function(vlink_add_completion_consistency_test target completion_base)
      if(TARGET ${target})
        add_test(
          NAME completion-${target}
          COMMAND
            ${VLINK_TEST_BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/completion_consistency_test.sh
            $<TARGET_FILE:${target}> ${completion_base}.bash ${completion_base}.zsh
            ${CMAKE_SOURCE_DIR}/packup/scripts/vlink-completions.sh.in ${ARGN}
        )
      endif()
    endfunction()

    vlink_add_completion_consistency_test(
      vlink-bag
      ${CMAKE_SOURCE_DIR}/cli/bag/etc/completions/vlink-bag
      info
      record
      play
      clone
      check
      reindex
      fix
      tag
    )
    vlink_add_completion_consistency_test(
      vlink-bench ${CMAKE_SOURCE_DIR}/cli/bench/etc/completions/vlink-bench run plot pub sub
    )
    vlink_add_completion_consistency_test(
      vlink-check ${CMAKE_SOURCE_DIR}/cli/check/etc/completions/vlink-check diag env test
    )
    vlink_add_completion_consistency_test(vlink-parse ${CMAKE_SOURCE_DIR}/cli/parse/etc/completions/vlink-parse)
    vlink_add_completion_consistency_test(
      vlink-efbs ${CMAKE_SOURCE_DIR}/cli/efbs/etc/completions/vlink-efbs pub sub import
    )
    vlink_add_completion_consistency_test(
      vlink-eproto ${CMAKE_SOURCE_DIR}/cli/eproto/etc/completions/vlink-eproto pub sub import
    )
    vlink_add_completion_consistency_test(vlink-info ${CMAKE_SOURCE_DIR}/cli/info/etc/completions/vlink-info)
    vlink_add_completion_consistency_test(vlink-list ${CMAKE_SOURCE_DIR}/cli/list/etc/completions/vlink-list)
    vlink_add_completion_consistency_test(vlink-monitor ${CMAKE_SOURCE_DIR}/cli/monitor/etc/completions/vlink-monitor)
    vlink_add_completion_consistency_test(vlink-proxy ${CMAKE_SOURCE_DIR}/proxy/etc/completions/vlink-proxy)
    vlink_add_completion_consistency_test(
      vlink-trigger ${CMAKE_SOURCE_DIR}/cli/trigger/etc/completions/vlink-trigger daemon dump
    )
    vlink_add_completion_consistency_test(
      vlink-bag2mcap ${CMAKE_SOURCE_DIR}/webviz/foxglove/cli/etc/completions/vlink-bag2mcap
    )
    vlink_add_completion_consistency_test(
      vlink-bag2rrd ${CMAKE_SOURCE_DIR}/webviz/rerun/cli/etc/completions/vlink-bag2rrd
    )
    vlink_add_completion_consistency_test(
      vlink-foxglove ${CMAKE_SOURCE_DIR}/webviz/foxglove/etc/completions/vlink-foxglove
    )
    vlink_add_completion_consistency_test(vlink-rerun ${CMAKE_SOURCE_DIR}/webviz/rerun/etc/completions/vlink-rerun)
  endif()
endif()

# requires
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
