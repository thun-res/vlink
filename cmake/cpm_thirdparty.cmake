#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

# options
if(ENABLE_CPM)
  option(ENABLE_CPM_SQLITE3 "Enable cpm build for sqlite3" OFF)
  option(ENABLE_CPM_OPENSSL "Enable cpm build for openssl" OFF)
  option(ENABLE_CPM_ZSTD "Enable cpm build for zstd" OFF)
  option(ENABLE_CPM_PROTOBUF "Enable cpm build for protobuf" OFF)
  option(ENABLE_CPM_FLATBUFFERS "Enable cpm build for flatbuffers" OFF)
endif()
if(NOT DEFINED CACHE{CPM_SOURCE_CACHE})
  if(DEFINED ENV{CPM_SOURCE_CACHE})
    set(CPM_SOURCE_CACHE
        "$ENV{CPM_SOURCE_CACHE}"
        CACHE PATH "CPM source cache"
    )
  elseif(WIN32)
    set(CPM_SOURCE_CACHE
        "$ENV{USERPROFILE}/.vlink-cpm-cache"
        CACHE PATH "CPM source cache"
    )
  else()
    set(CPM_SOURCE_CACHE
        "$ENV{HOME}/.vlink-cpm-cache"
        CACHE PATH "CPM source cache"
    )
  endif()
endif()
set(CPM_GITHUB_URL
    "https://github.com"
    CACHE STRING "CPM GitHub mirror URL"
)

# import cpm
include(${CMAKE_CURRENT_LIST_DIR}/cpm.cmake)
if(DEFINED CPM_MODULE_PATH)
  list(APPEND CMAKE_MODULE_PATH ${CPM_MODULE_PATH})
else()
  list(APPEND CMAKE_MODULE_PATH ${CMAKE_BINARY_DIR}/CPM_modules)
endif()
if(UNIX)
  set(ENV{LD_LIBRARY_PATH} "$ENV{LD_LIBRARY_PATH}:${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
endif()

# cpmaddpackage
if(ENABLE_CPM_ALL OR ENABLE_CPM_SQLITE3)
  cpmaddpackage(
    NAME
    sqlite3
    URL
    "${CPM_GITHUB_URL}/sjinks/sqlite3-cmake/archive/refs/tags/v3.45.3.zip"
    PATCHES
    "${CMAKE_SOURCE_DIR}/packup/patch/sqlite3-cmake.patch"
    OPTIONS
    "CMAKE_PROJECT_INCLUDE_BEFORE ${CMAKE_SOURCE_DIR}/cmake/cpm_external.cmake"
    "CMAKE_POSITION_INDEPENDENT_CODE ON"
    "BUILD_SHARED_LIBS OFF"
    "sqlite3_BUILD_SHARED_LIBS OFF"
    EXCLUDE_FROM_ALL
    ON
  )
endif()
if(ENABLE_CPM_ALL OR ENABLE_CPM_OPENSSL)
  cpmaddpackage(
    NAME
    openssl
    GIT_REPOSITORY
    ${CPM_GITHUB_URL}/viaduck/openssl-cmake.git
    GIT_TAG
    v3.6
    OPTIONS
    "CMAKE_PROJECT_INCLUDE_BEFORE ${CMAKE_SOURCE_DIR}/cmake/cpm_external.cmake"
    "CMAKE_POSITION_INDEPENDENT_CODE ON"
    "BUILD_SHARED_LIBS OFF"
    "OPENSSL_USE_STATIC_LIBS ON"
    EXCLUDE_FROM_ALL
    ON
  )
  if(openssl_ADDED)
    if(TARGET crypto AND NOT TARGET OpenSSL::Crypto)
      add_library(OpenSSL::Crypto ALIAS crypto)
    endif()
    if(TARGET ssl AND NOT TARGET OpenSSL::SSL)
      add_library(OpenSSL::SSL ALIAS ssl)
    endif()
    find_package(OpenSSL QUIET)
    set(OPENSSL_FOUND TRUE)
  endif()
endif()
if(ENABLE_CPM_ALL OR ENABLE_CPM_ZSTD)
  cpmaddpackage(
    NAME
    zstd
    URL
    "${CPM_GITHUB_URL}/facebook/zstd/archive/refs/tags/v1.5.7.zip"
    SOURCE_SUBDIR
    "build/cmake"
    OPTIONS
    "CMAKE_PROJECT_INCLUDE_BEFORE ${CMAKE_SOURCE_DIR}/cmake/cpm_external.cmake"
    "CMAKE_POSITION_INDEPENDENT_CODE ON"
    "BUILD_SHARED_LIBS OFF"
    "ZSTD_BUILD_PROGRAMS OFF"
    "ZSTD_BUILD_SHARED OFF"
    "ZSTD_BUILD_STATIC ON"
    EXCLUDE_FROM_ALL
    ON
  )
  if(zstd_ADDED
     AND TARGET libzstd_static
     AND NOT TARGET zstd::libzstd
     AND NOT TARGET zstd::libzstd_static
  )
    add_library(zstd::libzstd_static ALIAS libzstd_static)
    add_library(zstd::libzstd ALIAS libzstd_static)
  endif()
endif()
if(ENABLE_CPM_ALL OR ENABLE_CPM_PROTOBUF)
  cpmaddpackage(
    NAME
    protobuf
    URL
    "${CPM_GITHUB_URL}/protocolbuffers/protobuf/archive/refs/tags/v21.12.zip"
    OPTIONS
    "CMAKE_PROJECT_INCLUDE_BEFORE ${CMAKE_SOURCE_DIR}/cmake/cpm_external.cmake"
    "CMAKE_POSITION_INDEPENDENT_CODE ON"
    "BUILD_SHARED_LIBS OFF"
    "protobuf_BUILD_TESTS OFF"
    "protobuf_BUILD_PROTOC_BINARIES ON"
    "protobuf_MSVC_STATIC_RUNTIME OFF"
    EXCLUDE_FROM_ALL
    ON
  )
  if(protobuf_ADDED)
    if(TARGET protoc)
      set(VLINK_PROTOC_PROGRAM $<TARGET_FILE:protoc>)
    endif()
  endif()
endif()
if(ENABLE_CPM_ALL OR ENABLE_CPM_FLATBUFFERS)
  set(flatbuffers_VERSION_STRING "25.12.19")
  set(VLINK_FLATBUFFERS_STATIC_FLATC ON)
  if(APPLE)
    set(VLINK_FLATBUFFERS_STATIC_FLATC OFF)
  endif()
  cpmaddpackage(
    NAME
    flatbuffers
    URL
    "${CPM_GITHUB_URL}/google/flatbuffers/archive/refs/tags/v${flatbuffers_VERSION_STRING}.zip"
    OPTIONS
    "CMAKE_PROJECT_INCLUDE_BEFORE ${CMAKE_SOURCE_DIR}/cmake/cpm_external.cmake"
    "CMAKE_POSITION_INDEPENDENT_CODE ON"
    "BUILD_SHARED_LIBS OFF"
    "FLATBUFFERS_BUILD_SHAREDLIB OFF"
    "FLATBUFFERS_BUILD_TESTS OFF"
    "FLATBUFFERS_BUILD_FLATC ON"
    "FLATBUFFERS_STATIC_FLATC ${VLINK_FLATBUFFERS_STATIC_FLATC}"
    EXCLUDE_FROM_ALL
    ON
  )
  if(flatbuffers_ADDED
     AND TARGET flatbuffers
     AND NOT TARGET flatbuffers::flatbuffers
  )
    get_target_property(FLATBUFFERS_INCLUDE_DIR flatbuffers INTERFACE_INCLUDE_DIRECTORIES)
    target_include_directories(flatbuffers INTERFACE $<BUILD_INTERFACE:${FLATBUFFERS_INCLUDE_DIR}>)
    add_library(flatbuffers::flatbuffers ALIAS flatbuffers)
    if(TARGET flatc)
      set(VLINK_FLATC_PROGRAM $<TARGET_FILE:flatc>)
    endif()
  endif()
endif()

# cpmaddpackage
cpmaddpackage(
  NAME
  tinyxml2
  URL
  "${CPM_GITHUB_URL}/leethomason/tinyxml2/archive/refs/tags/10.1.0.zip"
  OPTIONS
  "CMAKE_PROJECT_INCLUDE_BEFORE ${CMAKE_SOURCE_DIR}/cmake/cpm_external.cmake"
  "CMAKE_POSITION_INDEPENDENT_CODE ON"
  "BUILD_SHARED_LIBS OFF"
  EXCLUDE_FROM_ALL
  ON
)
if(tinyxml2_ADDED)
  set(TINYXML2_INCLUDE_DIR ${tinyxml2_SOURCE_DIR})
  set(TINYXML2_LIBRARY tinyxml2)
endif()
set(Asio_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/thirdparty/asio/include)
cpmaddpackage(
  NAME
  cpptoml
  URL
  "${CPM_GITHUB_URL}/skystrife/cpptoml/archive/refs/tags/v0.1.1.zip"
  PATCHES
  "${CMAKE_SOURCE_DIR}/packup/patch/cpptoml_0.1.x.patch"
  OPTIONS
  "CMAKE_PROJECT_INCLUDE_BEFORE ${CMAKE_SOURCE_DIR}/cmake/cpm_external.cmake"
  "CMAKE_POSITION_INDEPENDENT_CODE ON"
  "BUILD_SHARED_LIBS OFF"
  "CPPTOML_BUILD_EXAMPLES OFF"
  "CMAKE_POLICY_VERSION_MINIMUM 3.14"
  EXCLUDE_FROM_ALL
  ON
)
cpmaddpackage(
  NAME
  foonathan_memory
  URL
  "${CPM_GITHUB_URL}/foonathan/memory/archive/refs/tags/v0.7-4.zip"
  OPTIONS
  "CMAKE_PROJECT_INCLUDE_BEFORE ${CMAKE_SOURCE_DIR}/cmake/cpm_external.cmake"
  "CMAKE_POSITION_INDEPENDENT_CODE ON"
  "BUILD_SHARED_LIBS OFF"
  "FOONATHAN_MEMORY_BUILD_EXAMPLES OFF"
  "FOONATHAN_MEMORY_BUILD_TESTS OFF"
  "FOONATHAN_MEMORY_BUILD_TOOLS OFF"
  "FOONATHAN_MEMORY_CHECK_ALLOCATION_SIZE OFF"
  EXCLUDE_FROM_ALL
  ON
)
cpmaddpackage(
  NAME
  fastcdr
  URL
  "${CPM_GITHUB_URL}/eProsima/Fast-CDR/archive/refs/tags/v1.1.1.zip"
  OPTIONS
  "CMAKE_PROJECT_INCLUDE_BEFORE ${CMAKE_SOURCE_DIR}/cmake/cpm_external.cmake"
  "CMAKE_POSITION_INDEPENDENT_CODE ON"
  "BUILD_SHARED_LIBS OFF"
  EXCLUDE_FROM_ALL
  ON
)
cpmaddpackage(
  NAME
  fastdds
  URL
  "${CPM_GITHUB_URL}/eProsima/Fast-DDS/archive/refs/tags/v2.10.7.zip"
  PATCHES
  "${CMAKE_SOURCE_DIR}/packup/patch/fastdds_2.10.x.patch"
  OPTIONS
  "CMAKE_PROJECT_INCLUDE_BEFORE ${CMAKE_SOURCE_DIR}/cmake/cpm_external.cmake"
  "CMAKE_POSITION_INDEPENDENT_CODE ON"
  "BUILD_SHARED_LIBS OFF"
  "EPROSIMA_BUILD OFF"
  "THIRDPARTY_UPDATE OFF"
  "FORCE_CXX 17"
  EXCLUDE_FROM_ALL
  ON
)
cpmaddpackage(
  NAME
  cyclonedds
  URL
  "${CPM_GITHUB_URL}/eclipse-cyclonedds/cyclonedds/archive/refs/tags/0.10.5.zip"
  PATCHES
  "${CMAKE_SOURCE_DIR}/packup/patch/cyclonedds_0.10.x.patch"
  OPTIONS
  "CMAKE_PROJECT_INCLUDE_BEFORE ${CMAKE_SOURCE_DIR}/cmake/cpm_external.cmake"
  "CMAKE_POSITION_INDEPENDENT_CODE ON"
  "BUILD_SHARED_LIBS OFF"
  "BUILD_IDLC OFF"
  "BUILD_DDSPERF OFF"
  "ENABLE_SECURITY OFF"
  "ENABLE_SSL OFF"
  "CYCLONEDDS_DISABLE_SSL ON"
  EXCLUDE_FROM_ALL
  ON
)
if(cyclonedds_ADDED AND TARGET ddsc)
  set(ddsi_INCLUDE_DIR "${cyclonedds_SOURCE_DIR}/src/core/ddsi/include")
endif()
cpmaddpackage(
  NAME
  iceoryx
  URL
  "${CPM_GITHUB_URL}/eclipse-iceoryx/iceoryx/archive/refs/tags/v2.0.8.zip"
  PATCHES
  "${CMAKE_SOURCE_DIR}/packup/patch/iceoryx_2.0.x.patch"
  SYSTEM
  ON
  SOURCE_SUBDIR
  "iceoryx_meta"
  OPTIONS
  "CMAKE_PROJECT_INCLUDE_BEFORE ${CMAKE_SOURCE_DIR}/cmake/cpm_external.cmake"
  "CMAKE_POSITION_INDEPENDENT_CODE ON"
  "BUILD_SHARED_LIBS OFF"
  "BINDING_C OFF"
  "INTROSPECTION OFF"
  "BUILD_DOC OFF"
  "CCACHE OFF"
  EXCLUDE_FROM_ALL
  ON
)
if(iceoryx_ADDED
   AND TARGET iceoryx_posh
   AND NOT TARGET iceoryx_posh::iceoryx_posh
)
  add_library(iceoryx_posh::iceoryx_posh ALIAS iceoryx_posh)
endif()
if(iceoryx_ADDED
   AND TARGET iceoryx_posh_config
   AND NOT TARGET iceoryx_posh::iceoryx_posh_config
)
  add_library(iceoryx_posh::iceoryx_posh_config ALIAS iceoryx_posh_config)
endif()
if(iceoryx_ADDED
   AND TARGET iceoryx_posh_roudi
   AND NOT TARGET iceoryx_posh::iceoryx_posh_roudi
)
  add_library(iceoryx_posh::iceoryx_posh_roudi ALIAS iceoryx_posh_roudi)
endif()
if(iceoryx_ADDED
   AND TARGET iox-roudi
   AND NOT TARGET iceoryx_posh::iox-roudi
)
  add_executable(iceoryx_posh::iox-roudi ALIAS iox-roudi)
endif()
unset(CMAKE_PROJECT_INCLUDE_BEFORE)
