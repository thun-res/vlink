#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

cmake_minimum_required(VERSION 3.15...3.31)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# version
file(STRINGS version.txt VLINK_VERSION)
string(TIMESTAMP VLINK_VERSION_TIMESTAMP "%Y-%m-%d %H:%M:%S")
execute_process(
  COMMAND git rev-parse HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE VLINK_VERSION_COMMIT_ID
  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
)
execute_process(
  COMMAND git describe --tags
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE VLINK_VERSION_TAG
  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
)

# project
project(
  vlink
  VERSION ${VLINK_VERSION}
  DESCRIPTION "A high-performance communication middleware"
  LANGUAGES CXX
)
set(DOC_PROJECT_NAME "VLink")
set(DOC_PROJECT_LOGO "./doc/images/vlink-icon.svg")
set(PROJECT_NAMESPACE "vlink::")

# common
include(cmake/functions/common.cmake)
include(cmake/functions/generator.cmake)
vlink_check()
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME runtime)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output/lib)
foreach(OUTPUTCONFIG Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${OUTPUTCONFIG}" OUTPUTCONFIG_U)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG_U} ${CMAKE_BINARY_DIR}/output/bin)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${OUTPUTCONFIG_U} ${CMAKE_BINARY_DIR}/output/lib)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${OUTPUTCONFIG_U} ${CMAKE_BINARY_DIR}/output/lib)
endforeach()
# set(CMAKE_CONFIGURATION_TYPES "RelWithDebInfo;Debug;Release;MinSizeRel" CACHE STRING "" FORCE)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release")
endif()

# compiler-optimize
if(MSVC)
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /MANIFEST:NO")
  set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} /MANIFEST:NO")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /MANIFEST:NO")
  add_compile_options(/wd4068 /wd4158 /wd4819 /wd5287)
  add_compile_options(/GL)
  add_link_options(/LTCG)
  add_link_options(/IGNORE:4078)
endif()
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-std=c++20" COMPILER_SUPPORTS_CXX20)

# ipo
if(POLICY CMP0069)
  cmake_policy(SET CMP0069 NEW)
endif()
if(NOT MSVC AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  include(CheckIPOSupported)
  check_ipo_supported(RESULT ipo_supported)
  if(ipo_supported)
    message(STATUS "Enable Interprocedural Optimization")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
  else()
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION FALSE)
  endif()
endif()

# options
option(BUILD_SHARED_LIBS "Build shared libraries" ON)
option(ENABLE_CPM "Enable cpm build" OFF)
option(ENABLE_CPM_ALL "Enable cpm-all build" OFF)
option(ENABLE_CCACHE_BUILD "Enable ccache build" OFF)
option(ENABLE_DOC "Enable doc" OFF)
option(ENABLE_CXX_STD_20 "Enable cxx standard 20 features" ${COMPILER_SUPPORTS_CXX20})
option(ENABLE_C_API "Enable c api" ON)
option(ENABLE_PYTHON_API "Enable python api" OFF)
option(ENABLE_SECURITY "Enable security(ssl)" ON)
option(ENABLE_SQLITE "Enable sqlite" ON)
option(ENABLE_ZSTD "Enable zstd" ON)
if(ANDROID
   OR QNX
   OR CMAKE_SYSTEM_NAME STREQUAL "QNX"
)
  option(ENABLE_LOG_BACKEND "Use LoggerBackend instead of platform logging" OFF)
else()
  option(ENABLE_LOG_BACKEND "Use LoggerBackend instead of platform logging" ON)
endif()
option(ENABLE_CLI_INFO "Enable cli-info" ON)
option(ENABLE_CLI_BAG "Enable cli-bag" ON)
option(ENABLE_CLI_EPROTO "Enable cli-eproto" ON)
option(ENABLE_CLI_EFBS "Enable cli-efbs" ON)
option(ENABLE_CLI_LIST "Enable cli-list" ON)
option(ENABLE_CLI_MONITOR "Enable cli-monitor" ON)
option(ENABLE_CLI_PARSE "Enable cli-parse" ON)
option(ENABLE_CLI_CHECK "Enable cli-check" ON)
option(ENABLE_CLI_BENCH "Enable cli-bench" ON)
option(ENABLE_CLI_TRIGGER "Enable cli-trigger" ON)
option(ENABLE_PROXY "Enable proxy" ON)
option(ENABLE_VIEWER "Enable viewer" OFF)
option(ENABLE_WEBVIZ "Enable webviz" OFF)
option(ENABLE_EXPRTK "Enable exprtk expression engine (required by cli-parse/viewer/webviz)" ON)
option(ENABLE_SYMLINKS "Install symlink" ON)
option(ENABLE_COMPLETIONS "Install shell completion scripts" ON)
if(CMAKE_EXPORT_COMPILE_COMMANDS OR CMAKE_CXX_CLANG_TIDY)
  option(ENABLE_EXAMPLES "Enable examples" ON)
else()
  option(ENABLE_EXAMPLES "Enable examples" OFF)
endif()
option(ENABLE_EXAMPLES_ALL "Enable examples-all" OFF)
option(ENABLE_TEST "Enable test" OFF)
option(ENABLE_TEST_WARN "Enable test compiler warnings" OFF)
option(ENABLE_TEST_SANITIZE "Enable test memory sanitize" OFF)
option(ENABLE_TEST_COVERAGE "Enable test code coverage" OFF)
if(ENABLE_CXX_STD_20)
  set(CMAKE_CXX_STANDARD 20)
endif()
if(NOT ENABLE_LOG_BACKEND
   AND NOT
       (ANDROID
        OR QNX
        OR CMAKE_SYSTEM_NAME STREQUAL "QNX"
        OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
)
  message(WARNING "Platform logging is not supported on this platform, set ENABLE_LOG_BACKEND to ON")
  set(ENABLE_LOG_BACKEND ON)
endif()

set(INSTALL_CONFIG_DIR
    ${CMAKE_INSTALL_SYSCONFDIR}/${CMAKE_PROJECT_NAME}
    CACHE STRING "Install dir for config files (relative to prefix)"
)
if(IS_ABSOLUTE "${INSTALL_CONFIG_DIR}")
  string(REGEX REPLACE "^([A-Za-z]:)?[\\/]+" "" BUILD_CONFIG_DIR "${INSTALL_CONFIG_DIR}")
else()
  set(BUILD_CONFIG_DIR ${INSTALL_CONFIG_DIR})
endif()

# cpm-cmake
if(ENABLE_CPM OR ENABLE_CPM_ALL)
  include(cmake/cpm_thirdparty.cmake)
endif()

# requires
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake/modules)
find_package(Threads REQUIRED)
if(ENABLE_SECURITY)
  find_package(OpenSSL QUIET)
  if(NOT TARGET OpenSSL::Crypto)
    message(WARNING "Can not find openssl, set ENABLE_SECURITY to OFF")
    set(ENABLE_SECURITY OFF)
  endif()
endif()
if(ENABLE_SQLITE)
  find_package(SQLite3 QUIET)
  if(NOT TARGET SQLite3::SQLite3 AND NOT TARGET SQLite::SQLite3)
    message(WARNING "Can not find sqlite, set ENABLE_SQLITE to OFF")
    set(ENABLE_SQLITE OFF)
  endif()
endif()
if(ENABLE_ZSTD)
  find_package(zstd CONFIG QUIET)
  if(NOT zstd_FOUND)
    find_package(zstd MODULE QUIET)
  endif()
  if(NOT TARGET zstd::libzstd_shared AND NOT TARGET zstd::libzstd_static)
    message(WARNING "Can not find zstd, set ENABLE_ZSTD to OFF")
    set(ENABLE_ZSTD OFF)
  endif()
endif()
if(ENABLE_CLI_EPROTO
   OR ENABLE_CLI_PARSE
   OR ENABLE_VIEWER
   OR ENABLE_WEBVIZ
)
  find_package(Protobuf CONFIG QUIET)
  if(NOT TARGET protobuf::libprotobuf)
    find_package(Protobuf QUIET)
  endif()
  if(NOT TARGET protobuf::libprotobuf)
    message(WARNING "Can not find protobuf, set ENABLE_CLI_EPROTO/ENABLE_CLI_PARSE/ENABLE_VIEWER/ENABLE_WEBVIZ to OFF")
    set(ENABLE_CLI_EPROTO OFF)
    set(ENABLE_CLI_PARSE OFF)
    set(ENABLE_VIEWER OFF)
    set(ENABLE_WEBVIZ OFF)
  endif()
endif()
if(ENABLE_PROXY)
  if(NOT SKIP_DDS)
    find_package(fastdds CONFIG QUIET)
    find_package(fastrtps CONFIG QUIET)
  endif()
  if(NOT SKIP_DDSC)
    find_package(CycloneDDS CONFIG QUIET)
  endif()
  if(NOT TARGET fastrtps
     AND NOT TARGET fastdds
     AND NOT TARGET CycloneDDS::ddsc
  )
    message(WARNING "Can not find fastdds or cyclonedds, set ENABLE_PROXY to OFF")
    set(ENABLE_PROXY OFF)
  endif()
endif()
if(ENABLE_CLI_EFBS
   OR ENABLE_CLI_PARSE
   OR ENABLE_VIEWER
   OR ENABLE_WEBVIZ
)
  find_package(flatbuffers CONFIG QUIET)
  if((NOT DEFINED flatbuffers_VERSION OR flatbuffers_VERSION STREQUAL "") AND DEFINED flatbuffers_VERSION_STRING)
    set(flatbuffers_VERSION ${flatbuffers_VERSION_STRING})
  endif()
  if((NOT TARGET flatbuffers::flatbuffers_shared AND NOT TARGET flatbuffers::flatbuffers)
     OR (NOT DEFINED flatbuffers_VERSION)
     OR (flatbuffers_VERSION VERSION_LESS "22.0.0")
  )
    message(WARNING "Can not find flatbuffers, set ENABLE_CLI_EFBS/ENABLE_CLI_PARSE/ENABLE_VIEWER/ENABLE_WEBVIZ to OFF")
    set(ENABLE_CLI_EFBS OFF)
    set(ENABLE_CLI_PARSE OFF)
    set(ENABLE_VIEWER OFF)
    set(ENABLE_WEBVIZ OFF)
  endif()
endif()
if(ENABLE_VIEWER OR ENABLE_WEBVIZ)
  if(NOT ENABLE_PROXY)
    message(WARNING "Can not build viewer without proxy, set ENABLE_VIEWER/ENABLE_WEBVIZ to OFF")
    set(ENABLE_VIEWER OFF)
    set(ENABLE_WEBVIZ OFF)
  endif()
endif()
set(VLINK_ENABLE_CXX_STD_20 ${ENABLE_CXX_STD_20})
set(VLINK_ENABLE_C_API ${ENABLE_C_API})
set(VLINK_ENABLE_SECURITY ${ENABLE_SECURITY})
set(VLINK_ENABLE_SQLITE ${ENABLE_SQLITE})
set(VLINK_ENABLE_ZSTD ${ENABLE_ZSTD})
set(VLINK_ENABLE_LOG_BACKEND ${ENABLE_LOG_BACKEND})
set(VLINK_ENABLE_CLI_INFO ${ENABLE_CLI_INFO})
set(VLINK_ENABLE_CLI_BAG ${ENABLE_CLI_BAG})
set(VLINK_ENABLE_CLI_BENCH ${ENABLE_CLI_BENCH})
set(VLINK_ENABLE_CLI_EPROTO ${ENABLE_CLI_EPROTO})
set(VLINK_ENABLE_CLI_EFBS ${ENABLE_CLI_EFBS})
set(VLINK_ENABLE_CLI_LIST ${ENABLE_CLI_LIST})
set(VLINK_ENABLE_CLI_MONITOR ${ENABLE_CLI_MONITOR})
set(VLINK_ENABLE_CLI_PARSE ${ENABLE_CLI_PARSE})
set(VLINK_ENABLE_CLI_CHECK ${ENABLE_CLI_CHECK})
set(VLINK_ENABLE_CLI_TRIGGER ${ENABLE_CLI_TRIGGER})
set(VLINK_ENABLE_EXPRTK ${ENABLE_EXPRTK})
set(VLINK_ENABLE_PROXY ${ENABLE_PROXY})
set(VLINK_ENABLE_VIEWER ${ENABLE_VIEWER})
set(VLINK_ENABLE_WEBVIZ ${ENABLE_WEBVIZ})
set(VLINK_ENABLE_SYMLINKS ${ENABLE_SYMLINKS})
set(VLINK_ENABLE_COMPLETIONS ${ENABLE_COMPLETIONS})
set(VLINK_ENABLE_EXAMPLES ${ENABLE_EXAMPLES})
set(VLINK_ENABLE_EXAMPLES_ALL ${ENABLE_EXAMPLES_ALL})
set(VLINK_ENABLE_TEST ${ENABLE_TEST})

# thirdparty(headeronly)
add_subdirectory(thirdparty/lzav)
add_subdirectory(thirdparty/argparse)
add_subdirectory(thirdparty/bitsery)
add_subdirectory(thirdparty/doctest)
add_subdirectory(thirdparty/json)
add_subdirectory(thirdparty/mcap)
add_subdirectory(thirdparty/eigen3)
add_subdirectory(thirdparty/exprtk)
add_subdirectory(thirdparty/robin-map)
add_subdirectory(thirdparty/asio)
add_subdirectory(thirdparty/websocketpp)

# ccache
if(ENABLE_CCACHE_BUILD)
  if(WIN32)
    set(VLINK_COMPILER_CACHE sccache)
  else()
    set(VLINK_COMPILER_CACHE ccache)
  endif()
  find_program(CCACHE_FOUND ${VLINK_COMPILER_CACHE})
  if(CCACHE_FOUND)
    set(CMAKE_C_COMPILER_LAUNCHER ${VLINK_COMPILER_CACHE})
    set(CMAKE_CXX_COMPILER_LAUNCHER ${VLINK_COMPILER_CACHE})
  endif()
endif()

# src
file(GLOB_RECURSE PROJECT_HDRS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/include/*.h)
file(GLOB_RECURSE PROJECT_SRCS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/*.cc)
add_library(${PROJECT_NAME} ${PROJECT_HDRS} ${PROJECT_SRCS})
add_library(${PROJECT_NAMESPACE}${PROJECT_NAME} ALIAS ${PROJECT_NAME})
set_target_properties(
  ${PROJECT_NAME} PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
)
if(ENABLE_CXX_STD_20)
  target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_20)
  target_compile_options(
    ${PROJECT_NAME}
    PUBLIC
      $<$<AND:$<CXX_COMPILER_ID:GNU>,$<VERSION_GREATER_EQUAL:$<CXX_COMPILER_VERSION>,10>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,11>>:-fcoroutines>
  )
else()
  target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_17)
endif()
target_include_directories(
  ${PROJECT_NAME}
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/${CMAKE_PROJECT_NAME}>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(BUILD_SHARED_LIBS)
  set(VLINK_APP_DEFINES)
  set(VLINK_LIB_DEFINES "VLINK_LIBRARY")
  target_compile_definitions(${PROJECT_NAME} PRIVATE ${VLINK_LIB_DEFINES})
else()
  set(VLINK_APP_DEFINES "VLINK_LIBRARY_STATIC")
  set(VLINK_LIB_DEFINES "VLINK_LIBRARY_STATIC")
  target_compile_definitions(${PROJECT_NAME} PUBLIC ${VLINK_LIB_DEFINES})
endif()
if(WIN32)
  list(APPEND VLINK_PLATFORM_LIBS Iphlpapi ws2_32)
  if(MSVC)
    set_target_properties(${PROJECT_NAME} PROPERTIES COMPILE_FLAGS "/wd4819")
    target_compile_options(${PROJECT_NAME} PUBLIC "/utf-8")
  endif()
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(${PROJECT_NAME} PUBLIC -Wno-ignored-attributes)
  endif()
else()
  target_compile_options(${PROJECT_NAME} PUBLIC $<BUILD_INTERFACE:-fvisibility=hidden>)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND VLINK_PLATFORM_LIBS ${CMAKE_DL_LIBS} rt)
    list(APPEND VLINK_PLATFORM_LIBS
         "$<$<AND:$<CXX_COMPILER_ID:GNU>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,9.0>>:-lstdc++fs>"
    )
  elseif(QNX OR CMAKE_SYSTEM_NAME STREQUAL "QNX")
    list(APPEND VLINK_PLATFORM_LIBS socket)
    if(NOT ENABLE_LOG_BACKEND)
      list(APPEND VLINK_PLATFORM_LIBS slog2)
    endif()
    if(CMAKE_SYSTEM_VERSION VERSION_GREATER_EQUAL "8.0.0")
      list(APPEND VLINK_PLATFORM_LIBS fsnotify)
    else()
      list(APPEND VLINK_PLATFORM_LIBS c++fs)
    endif()
  elseif(ANDROID)
    if(NOT ENABLE_LOG_BACKEND)
      list(APPEND VLINK_PLATFORM_LIBS log)
    endif()
  elseif(APPLE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE "_DARWIN_C_SOURCE")
  endif()
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 12)
    target_compile_options(${PROJECT_NAME} PRIVATE -Wno-stringop-overflow)
    target_link_options(${PROJECT_NAME} PRIVATE -Wno-stringop-overflow)
  endif()
endif()
target_link_libraries(${PROJECT_NAME} PUBLIC Threads::Threads)
target_link_libraries(${PROJECT_NAME} PRIVATE ${VLINK_PLATFORM_LIBS})
target_link_libraries(
  ${PROJECT_NAME} PRIVATE $<BUILD_INTERFACE:thirdparty::lzav> $<BUILD_INTERFACE:thirdparty::mcap>
                          $<BUILD_INTERFACE:thirdparty::json> $<BUILD_INTERFACE:thirdparty::robin-map>
)
if(ENABLE_SECURITY)
  target_link_libraries(${PROJECT_NAME} PRIVATE OpenSSL::Crypto)
  target_compile_definitions(${PROJECT_NAME} PRIVATE "VLINK_ENABLE_SECURITY")
endif()
if(ENABLE_SQLITE)
  if(TARGET SQLite3::SQLite3)
    target_link_libraries(${PROJECT_NAME} PRIVATE SQLite3::SQLite3)
  elseif(TARGET SQLite::SQLite3)
    target_link_libraries(${PROJECT_NAME} PRIVATE SQLite::SQLite3)
  endif()
  target_compile_definitions(${PROJECT_NAME} PRIVATE "VLINK_ENABLE_SQLITE")
endif()
if(ENABLE_ZSTD)
  if(TARGET zstd::libzstd_shared)
    target_link_libraries(${PROJECT_NAME} PRIVATE zstd::libzstd_shared)
  elseif(TARGET zstd::libzstd_static)
    target_link_libraries(${PROJECT_NAME} PRIVATE zstd::libzstd_static)
  endif()
  target_compile_definitions(${PROJECT_NAME} PRIVATE "VLINK_ENABLE_ZSTD")
endif()
if(ENABLE_LOG_BACKEND)
  target_compile_definitions(${PROJECT_NAME} PRIVATE "VLINK_ENABLE_LOG_BACKEND")
endif()

# install
install(
  TARGETS ${PROJECT_NAME}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT runtime
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT devel
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
          COMPONENT runtime
          NAMELINK_COMPONENT devel
)
install(
  DIRECTORY ${PROJECT_SOURCE_DIR}/include/${CMAKE_PROJECT_NAME}
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  COMPONENT devel
  PATTERN ".*" EXCLUDE
)
if(EXISTS ${PROJECT_SOURCE_DIR}/CHANGELOG.md)
  install(FILES ${PROJECT_SOURCE_DIR}/CHANGELOG.md DESTINATION ${INSTALL_CONFIG_DIR})
endif()
if(ENABLE_DOC)
  install(
    DIRECTORY ${PROJECT_SOURCE_DIR}/examples
    DESTINATION ${INSTALL_CONFIG_DIR}
    COMPONENT devel
  )
endif()

# uninstall
if(NOT TARGET uninstall)
  configure_file("${CMAKE_SOURCE_DIR}/cmake/uninstall.cmake.in" "${CMAKE_BINARY_DIR}/uninstall.cmake" @ONLY)
  add_custom_target(
    uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_BINARY_DIR}/uninstall.cmake
    COMMENT "Uninstalling files listed in install_manifest.txt ..."
  )
endif()

# modules
add_subdirectory(modules/intra)
add_subdirectory(modules/shm)
add_subdirectory(modules/shm2)
add_subdirectory(modules/zenoh)
add_subdirectory(modules/dds)
add_subdirectory(modules/ddsc)
add_subdirectory(modules/ddsr)
add_subdirectory(modules/someip)
add_subdirectory(modules/fdbus)
add_subdirectory(modules/mqtt)
set(VLINK_DEFAULT_LIBRARIES ${VLINK_LIBRARIES})
if(VLINK_LIBRARIES)
  set(VLINK_NO_INTRA_LIBRARIES ${VLINK_LIBRARIES})
  list(REMOVE_ITEM VLINK_NO_INTRA_LIBRARIES ${PROJECT_NAMESPACE}intra)
endif()
string(REPLACE "${PROJECT_NAMESPACE}" "" VLINK_LIBRARIES_INFO "${VLINK_LIBRARIES}")
string(REPLACE ";" "," VLINK_LIBRARIES_INFO "${VLINK_LIBRARIES_INFO}")

# all
if(NOT TARGET vlink::all)
  add_library(vlink::all INTERFACE IMPORTED GLOBAL)
  set_target_properties(vlink::all PROPERTIES INTERFACE_LINK_LIBRARIES "vlink::vlink;${VLINK_LIBRARIES}")
endif()

# c-interface
if(ENABLE_C_API)
  add_subdirectory(languages/c_api)
endif()

# python-interface
if(ENABLE_PYTHON_API)
  add_subdirectory(languages/python_api)
endif()

# exprtk
if(ENABLE_EXPRTK
   AND (ENABLE_CLI_PARSE
        OR ENABLE_VIEWER
        OR ENABLE_WEBVIZ)
)
  add_subdirectory(exprtk)
endif()

# cli
if(ENABLE_CLI_INFO)
  add_subdirectory(cli/info)
endif()
if(ENABLE_CLI_BAG)
  add_subdirectory(cli/bag)
endif()
if(ENABLE_CLI_EPROTO)
  add_subdirectory(cli/eproto)
endif()
if(ENABLE_CLI_EFBS)
  add_subdirectory(cli/efbs)
endif()
if(ENABLE_CLI_LIST)
  add_subdirectory(cli/list)
endif()
if(ENABLE_CLI_MONITOR)
  add_subdirectory(cli/monitor)
endif()
if(ENABLE_CLI_PARSE)
  add_subdirectory(cli/parse)
endif()
if(ENABLE_CLI_CHECK)
  add_subdirectory(cli/check)
endif()
if(ENABLE_CLI_BENCH)
  add_subdirectory(cli/bench)
endif()
if(ENABLE_CLI_TRIGGER)
  add_subdirectory(cli/trigger)
endif()

# proxy
if(ENABLE_PROXY)
  add_subdirectory(proxy)
endif()

# viewer
if(ENABLE_VIEWER)
  add_subdirectory(viewer)
endif()

# webviz
if(ENABLE_WEBVIZ)
  add_subdirectory(webviz)
endif()

# examples
if(ENABLE_EXAMPLES OR ENABLE_EXAMPLES_ALL)
  add_subdirectory(examples)
endif()

# add_subdirectory(tools/test_plugins/plugin_decoder)

# test
if(ENABLE_TEST)
  enable_testing()
  add_subdirectory(test)
  if(ENABLE_TEST_SANITIZE)
    vlink_test_sanitize(${PROJECT_NAME})
  endif()
  if(ENABLE_TEST_COVERAGE)
    set(VLINK_TEST_COVERAGE_EXCLUDES
        */build-*/*
        */languages/c_api/*
        */cli/*
        */exprtk/*
        */proxy/*
        */test/*
        */thirdparty/*
        */.vlink-cpm-cache/*
        */modules/dds/builtin/*
        */modules/ddsc/builtin/*
        */modules/ddsr/builtin/*
        */include/private/*
        */src/private/*
    )
    list(REMOVE_DUPLICATES VLINK_TEST_COVERAGE_EXCLUDES)
    vlink_test_coverage(${PROJECT_NAME} TYPE lcov EXCLUDES ${VLINK_TEST_COVERAGE_EXCLUDES})
  endif()
endif()
if(ENABLE_TEST_WARN)
  vlink_test_warn(${PROJECT_NAME})
endif()

# doc
if(ENABLE_DOC)
  add_subdirectory(doc)
endif()

# export
vlink_export(${PROJECT_NAME} ${PROJECT_NAME} ${PROJECT_NAMESPACE} ${CMAKE_PROJECT_VERSION})

# license
include(${CMAKE_SOURCE_DIR}/cmake/install_licenses.cmake)

# script
execute_process(COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/output")
execute_process(COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/output/${BUILD_CONFIG_DIR}")
if(ENABLE_WEBVIZ)
  if(ENABLE_WEBVIZ_FOXGLOVE AND TARGET vlink-foxglove)
    set(VLINK_WEBVIZ_COMPLETION_TARGET "foxglove")
  elseif(ENABLE_WEBVIZ_RERUN AND TARGET vlink-rerun)
    set(VLINK_WEBVIZ_COMPLETION_TARGET "rerun")
  else()
    set(VLINK_WEBVIZ_COMPLETION_TARGET "")
  endif()
else()
  set(VLINK_WEBVIZ_COMPLETION_TARGET "")
endif()
if(WIN32)
  configure_file(
    "${CMAKE_SOURCE_DIR}/packup/scripts/vlink-setup.bat.in" "${CMAKE_BINARY_DIR}/output/bin/vlink-setup.bat" @ONLY
  )
  install(FILES ${CMAKE_BINARY_DIR}/output/bin/vlink-setup.bat DESTINATION ${CMAKE_INSTALL_BINDIR})
else()
  if(ENABLE_COMPLETIONS)
    configure_file(
      "${CMAKE_SOURCE_DIR}/packup/scripts/vlink-completions.sh.in"
      "${CMAKE_BINARY_DIR}/output/${BUILD_CONFIG_DIR}/vlink-completions.sh" @ONLY
    )
    install(FILES ${CMAKE_BINARY_DIR}/output/${BUILD_CONFIG_DIR}/vlink-completions.sh DESTINATION ${INSTALL_CONFIG_DIR})
  endif()
  configure_file(
    "${CMAKE_SOURCE_DIR}/packup/scripts/vlink-setup.sh.in" "${CMAKE_BINARY_DIR}/output/vlink-setup.sh" @ONLY
  )
  install(FILES ${CMAKE_BINARY_DIR}/output/vlink-setup.sh DESTINATION ${INSTALL_CONFIG_DIR})
endif()

# message
set(VLINK_INFO_FILE ${CMAKE_BINARY_DIR}/output/${BUILD_CONFIG_DIR}/vlink-options.txt)
message("")
vlink_log()
vlink_log("┌──────── VLink Options ─────────────────────────────────────────────────────────")
vlink_log("│ Project:                     ${CMAKE_PROJECT_NAME}")
vlink_log("│ Version:                     ${VLINK_VERSION}")
vlink_log("│ Time stamp:                  ${VLINK_VERSION_TIMESTAMP}")
vlink_log("│ Git tag:                     ${VLINK_VERSION_TAG}")
vlink_log("│ Git commit-id:               ${VLINK_VERSION_COMMIT_ID}")
vlink_log("│ Install prefix:              ${CMAKE_INSTALL_PREFIX}")
vlink_log("│ Compiler:                    ${CMAKE_CXX_COMPILER}")
vlink_log("│ Modules:                     ${VLINK_LIBRARIES_INFO}")
vlink_log("│ CMAKE_BUILD_TYPE:            ${CMAKE_BUILD_TYPE}")
vlink_log("│ BUILD_SHARED_LIBS:           ${BUILD_SHARED_LIBS}")
vlink_log("│ INSTALL_CONFIG_DIR:          ${INSTALL_CONFIG_DIR}")
vlink_log("│ ENABLE_CPM:                  ${ENABLE_CPM}")
vlink_log("│ ENABLE_CPM_ALL:              ${ENABLE_CPM_ALL}")
vlink_log("│ ENABLE_CCACHE_BUILD:         ${ENABLE_CCACHE_BUILD}")
vlink_log("│ ENABLE_DOC:                  ${ENABLE_DOC}")
vlink_log("│ ENABLE_CXX_STD_20:           ${ENABLE_CXX_STD_20}")
vlink_log("│ ENABLE_C_API:                ${ENABLE_C_API}")
vlink_log("│ ENABLE_PYTHON_API:           ${ENABLE_PYTHON_API}")
vlink_log("│ ENABLE_SECURITY:             ${ENABLE_SECURITY}")
vlink_log("│ ENABLE_SQLITE:               ${ENABLE_SQLITE}")
vlink_log("│ ENABLE_ZSTD:                 ${ENABLE_ZSTD}")
vlink_log("│ ENABLE_LOG_BACKEND:          ${ENABLE_LOG_BACKEND}")
vlink_log("│ ENABLE_CLI_INFO:             ${ENABLE_CLI_INFO}")
vlink_log("│ ENABLE_CLI_BAG:              ${ENABLE_CLI_BAG}")
vlink_log("│ ENABLE_CLI_EPROTO:           ${ENABLE_CLI_EPROTO}")
vlink_log("│ ENABLE_CLI_EFBS:             ${ENABLE_CLI_EFBS}")
vlink_log("│ ENABLE_CLI_LIST:             ${ENABLE_CLI_LIST}")
vlink_log("│ ENABLE_CLI_MONITOR:          ${ENABLE_CLI_MONITOR}")
vlink_log("│ ENABLE_CLI_PARSE:            ${ENABLE_CLI_PARSE}")
vlink_log("│ ENABLE_CLI_CHECK:            ${ENABLE_CLI_CHECK}")
vlink_log("│ ENABLE_CLI_BENCH:            ${ENABLE_CLI_BENCH}")
vlink_log("│ ENABLE_CLI_TRIGGER:          ${ENABLE_CLI_TRIGGER}")
vlink_log("│ ENABLE_EXPRTK:               ${ENABLE_EXPRTK}")
vlink_log("│ ENABLE_PROXY:                ${ENABLE_PROXY}")
vlink_log("│ ENABLE_VIEWER:               ${ENABLE_VIEWER}")
vlink_log("│ ENABLE_WEBVIZ:               ${ENABLE_WEBVIZ}")
vlink_log("│ ENABLE_SYMLINKS:             ${ENABLE_SYMLINKS}")
vlink_log("│ ENABLE_COMPLETIONS:          ${ENABLE_COMPLETIONS}")
vlink_log("│ ENABLE_EXAMPLES:             ${ENABLE_EXAMPLES}")
vlink_log("│ ENABLE_EXAMPLES_ALL:         ${ENABLE_EXAMPLES_ALL}")
vlink_log("│ ENABLE_TEST:                 ${ENABLE_TEST}")
vlink_log("│ ENABLE_TEST_WARN:            ${ENABLE_TEST_WARN}")
vlink_log("│ ENABLE_TEST_SANITIZE:        ${ENABLE_TEST_SANITIZE}")
vlink_log("│ ENABLE_TEST_COVERAGE:        ${ENABLE_TEST_COVERAGE}")
vlink_log("└────────────────────────────────────────────────────────────────────────────────")
message("")
install(FILES ${VLINK_INFO_FILE} DESTINATION ${INSTALL_CONFIG_DIR}/)
if("${VLINK_LIBRARIES}" STREQUAL "")
  message(WARNING "VLINK_LIBRARIES is empty")
endif()

# cpack
include(cmake/package.cmake)
