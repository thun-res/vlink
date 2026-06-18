#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(vlink-rerun LANGUAGES CXX)

# requires
if(NOT TARGET vlink)
  find_package(
    vlink
    COMPONENTS ALL
    REQUIRED
  )
endif()

if(NOT TARGET protobuf::libprotobuf)
  find_package(Protobuf CONFIG QUIET)
  if(NOT TARGET protobuf::libprotobuf)
    find_package(Protobuf REQUIRED)
  endif()
endif()

if(TARGET protobuf::libprotobuf)
  set(protobuf_libs protobuf::libprotobuf)
elseif(TARGET protobuf::libprotobuf-lite)
  set(protobuf_libs protobuf::libprotobuf-lite)
endif()

if(NOT TARGET flatbuffers::flatbuffers_shared AND NOT TARGET flatbuffers::flatbuffers)
  find_package(flatbuffers CONFIG QUIET)
endif()

if(TARGET flatbuffers::flatbuffers_shared)
  set(flatbuffers_libs flatbuffers::flatbuffers_shared)
elseif(TARGET flatbuffers::flatbuffers)
  set(flatbuffers_libs flatbuffers::flatbuffers)
endif()

if(NOT protobuf_libs OR NOT flatbuffers_libs)
  message(WARNING "Can not find proto or flatbuffers, skipped.")
  return()
endif()

if(NOT TARGET rerun_sdk)
  if(DEFINED ENV{RERUN_SDK_DIR})
    set(RERUN_SDK_DIR "$ENV{RERUN_SDK_DIR}")
    message(STATUS "Using RERUN_SDK_DIR from environment: ${RERUN_SDK_DIR}")
    if(WIN32)
      if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(RERUN_SDK_LIB "${RERUN_SDK_DIR}/lib/rerun_c__win_x64.lib")
      else()
        message(FATAL_ERROR "Unsupported Windows architecture: 32-bit")
      endif()
    elseif(APPLE)
      if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(RERUN_SDK_LIB "${RERUN_SDK_DIR}/lib/librerun_c__macos_arm64.a")
      else()
        message(FATAL_ERROR "Unsupported macOS architecture: ${CMAKE_SYSTEM_PROCESSOR}")
      endif()
    elseif(UNIX)
      if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(RERUN_SDK_LIB "${RERUN_SDK_DIR}/lib/librerun_c__linux_arm64.a")
      elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        set(RERUN_SDK_LIB "${RERUN_SDK_DIR}/lib/librerun_c__linux_x64.a")
      else()
        message(FATAL_ERROR "Unsupported Linux architecture: ${CMAKE_SYSTEM_PROCESSOR}")
      endif()
    else()
      message(FATAL_ERROR "Unsupported platform")
    endif()
    if(NOT EXISTS "${RERUN_SDK_LIB}")
      message(FATAL_ERROR "rerun sdk library not found: ${RERUN_SDK_LIB}")
    endif()
    if(EXISTS "${RERUN_SDK_DIR}/include")
      set(RERUN_SDK_INCLUDE "${RERUN_SDK_DIR}/include")
    elseif(EXISTS "${RERUN_SDK_DIR}/src")
      set(RERUN_SDK_INCLUDE "${RERUN_SDK_DIR}/src")
    else()
      message(FATAL_ERROR "Cannot find rerun sdk headers under: ${RERUN_SDK_DIR}")
    endif()
    add_library(rerun_sdk STATIC IMPORTED GLOBAL)
    set_target_properties(
      rerun_sdk PROPERTIES IMPORTED_LOCATION "${RERUN_SDK_LIB}" INTERFACE_INCLUDE_DIRECTORIES "${RERUN_SDK_INCLUDE}"
    )
    if(APPLE)
      target_link_libraries(rerun_sdk INTERFACE "-framework CoreFoundation" "-framework IOKit" "-framework Security")
    elseif(UNIX)
      target_link_libraries(rerun_sdk INTERFACE m dl pthread)
    elseif(WIN32)
      target_link_libraries(
        rerun_sdk
        INTERFACE Crypt32
                  Iphlpapi
                  Ncrypt
                  Netapi32
                  ntdll
                  Pdh
                  PowrProf
                  Psapi
                  Secur32
                  Userenv
                  ws2_32
      )
    endif()
  else()
    find_package(rerun_sdk CONFIG REQUIRED)
  endif()
endif()

# src
file(GLOB PROJECT_HDRS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/*.h)
file(GLOB PROJECT_SRCS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/*.cc)

add_executable(${PROJECT_NAME} ${PROJECT_HDRS} ${PROJECT_SRCS})

target_include_directories(${PROJECT_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(
  ${PROJECT_NAME} PRIVATE vlink-webviz-common ${protobuf_libs} ${flatbuffers_libs} thirdparty::json
                          thirdparty::argparse rerun_sdk
)
vlink_target_system_includes_from(${PROJECT_NAME} ${protobuf_libs})
vlink_target_system_includes_from(${PROJECT_NAME} ${flatbuffers_libs})

if(WIN32)
  target_compile_definitions(${PROJECT_NAME} PRIVATE "WIN32_LEAN_AND_MEAN" "NOMINMAX")
  if(EXISTS ${CMAKE_SOURCE_DIR}/packup/ico/vlink-cmd.rc)
    target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/packup/ico/vlink-cmd.rc)
  endif()
  if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /wd4127)
  endif()
endif()

# install
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# symlinks
if(ENABLE_SYMLINKS)
  vlink_install_symlink(webviz_rerun SYMLINK ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# completions
if(ENABLE_COMPLETIONS)
  if(ENABLE_SYMLINKS)
    vlink_install_completions(${PROJECT_NAME} ALIAS webviz_rerun)
  else()
    vlink_install_completions(${PROJECT_NAME})
  endif()
endif()

# cli
add_subdirectory(cli)

# etc
if(EXISTS ${PROJECT_SOURCE_DIR}/etc)
  add_custom_command(
    TARGET ${PROJECT_NAME}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/etc/
            ${CMAKE_BINARY_DIR}/output/${BUILD_CONFIG_DIR}/vlink-rerun/
  )
  install(
    DIRECTORY ${PROJECT_SOURCE_DIR}/etc/
    DESTINATION ${INSTALL_CONFIG_DIR}/vlink-rerun/
    REGEX "completions" EXCLUDE
  )
endif()
