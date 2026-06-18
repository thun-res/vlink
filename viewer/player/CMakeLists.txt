#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(
  vlink-player
  VERSION 0.1
  LANGUAGES CXX
)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# src
file(GLOB PROJECT_HDRS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/*.h)
file(GLOB PROJECT_SRCS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/*.cc)
file(GLOB PROJECT_UIS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/*.ui)
file(GLOB PROJECT_RCS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/*.qrc)

add_executable(${PROJECT_NAME} ${PROJECT_HDRS} ${PROJECT_SRCS} ${PROJECT_UIS} ${PROJECT_RCS})

if(WIN32)
  if(EXISTS ${CMAKE_SOURCE_DIR}/packup/ico/vlink-player.rc)
    target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/packup/ico/vlink-player.rc)
  endif()
  set_property(TARGET ${PROJECT_NAME} PROPERTY WIN32_EXECUTABLE TRUE)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE ${PROJECT_SOURCE_DIR})

target_link_libraries(${PROJECT_NAME} PRIVATE vlink::vlink vlink::all Qt${QT_VERSION_MAJOR}::Widgets)

# install
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
