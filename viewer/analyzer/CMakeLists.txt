#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(
  vlink-analyzer
  VERSION 0.1
  LANGUAGES CXX
)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(NOT TARGET protobuf::libprotobuf)
  find_package(Protobuf CONFIG QUIET)
  if(NOT TARGET protobuf::libprotobuf)
    find_package(Protobuf REQUIRED)
  endif()
endif()

if(NOT TARGET flatbuffers::flatbuffers_shared AND NOT TARGET flatbuffers::flatbuffers)
  find_package(flatbuffers CONFIG QUIET)
endif()

# src
file(GLOB PROJECT_HDRS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/*.h)
file(GLOB PROJECT_SRCS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/*.cc)
file(GLOB PROJECT_UIS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/*.ui)
file(GLOB PROJECT_RCS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/*.qrc)

add_executable(${PROJECT_NAME} ${PROJECT_HDRS} ${PROJECT_SRCS} ${PROJECT_UIS} ${PROJECT_RCS})

if(WIN32)
  if(EXISTS ${CMAKE_SOURCE_DIR}/packup/ico/vlink-analyzer.rc)
    target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/packup/ico/vlink-analyzer.rc)
  endif()
  set_property(TARGET ${PROJECT_NAME} PROPERTY WIN32_EXECUTABLE TRUE)
endif()

target_sources(${PROJECT_NAME} PRIVATE ../flatbuffers_runtime.cc ../flatbuffers_runtime.h)

target_include_directories(${PROJECT_NAME} PRIVATE ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/..)

target_link_libraries(${PROJECT_NAME} PRIVATE vlink::vlink vlink::all Qt${QT_VERSION_MAJOR}::Widgets)
target_link_libraries(${PROJECT_NAME} PRIVATE thirdparty::json protobuf::libprotobuf QCustomPlot::QCustomPlot)

if(NOT TARGET vlink-exprtk_api)
  find_package(vlink-exprtk_api QUIET)
endif()

if(TARGET vlink::exprtk_api)
  target_link_libraries(${PROJECT_NAME} PRIVATE vlink::exprtk_api)
endif()

if(TARGET flatbuffers::flatbuffers_shared)
  target_link_libraries(${PROJECT_NAME} PRIVATE flatbuffers::flatbuffers_shared)
elseif(TARGET flatbuffers::flatbuffers)
  target_link_libraries(${PROJECT_NAME} PRIVATE flatbuffers::flatbuffers)
endif()
if(MSVC)
  target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
endif()

# install
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
