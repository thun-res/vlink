#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

cmake_minimum_required(VERSION 3.15...3.31)

project(vlink-viewer)

# requires
find_package(QT NAMES Qt6 Qt5 QUIET)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Sql Network)

option(ENABLE_VIEWER_FFMPEG "Enable FFmpeg for viewer" OFF)
option(ENABLE_VIEWER_OSG "Enable OSG for viewer" ON)

if(NOT TARGET vlink)
  find_package(
    vlink
    COMPONENTS ALL
    REQUIRED
  )
endif()

if(NOT TARGET vlink-proxy_api)
  find_package(vlink-proxy_api REQUIRED)
endif()

if(NOT TARGET protobuf::libprotobuf)
  find_package(Protobuf CONFIG QUIET)
  if(NOT TARGET protobuf::libprotobuf)
    find_package(Protobuf REQUIRED)
  endif()
endif()

if(NOT TARGET flatbuffers::flatbuffers_shared AND NOT TARGET flatbuffers::flatbuffers)
  find_package(flatbuffers CONFIG REQUIRED)
endif()

if(QT_VERSION_MAJOR EQUAL 6)
  find_package(Qt6 REQUIRED COMPONENTS OpenGLWidgets)
endif()

if(ENABLE_VIEWER_FFMPEG)
  message(STATUS "Use ffmpeg for viewer.")
  find_package(FFmpeg CONFIG QUIET)
  if(NOT FFmpeg_FOUND)
    find_package(FFmpeg MODULE REQUIRED)
  endif()
endif()

if(ENABLE_VIEWER_OSG)
  message(STATUS "Use osg for viewer.")
  find_package(
    OpenSceneGraph REQUIRED
    COMPONENTS osg
               osgManipulator
               osgViewer
               osgGA
               osgDB
               osgUtil
               osgText
               OpenThreads
  )
endif()

# src
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

file(GLOB PROJECT_HDRS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
file(GLOB PROJECT_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cc)
file(GLOB PROJECT_UIS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.ui)
file(GLOB PROJECT_RCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.qrc)
file(GLOB PROJECT_OSG_HDRS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/osg/*.h)
file(GLOB PROJECT_OSG_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/osg/*.cc)
file(GLOB PROJECT_PERCEPTION_HDRS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/perception/*.h)
file(GLOB PROJECT_PERCEPTION_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/perception/*.cc)
file(GLOB PROJECT_PERCEPTION_UIS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/perception/*.ui)

add_executable(
  ${PROJECT_NAME}
  ../proxy/proxy_common.h
  ${PROJECT_HDRS}
  ${PROJECT_SRCS}
  ${PROJECT_UIS}
  ${PROJECT_RCS}
  ${PROJECT_OSG_HDRS}
  ${PROJECT_OSG_SRCS}
  ${PROJECT_PERCEPTION_HDRS}
  ${PROJECT_PERCEPTION_SRCS}
  ${PROJECT_PERCEPTION_UIS}
)

if(ENABLE_VIEWER_FFMPEG)
  if(TARGET ffmpeg::ffmpeg)
    target_link_libraries(${PROJECT_NAME} PRIVATE ffmpeg::ffmpeg)
  else()
    target_include_directories(${PROJECT_NAME} PRIVATE ${FFMPEG_INCLUDE_DIRS} ${SWSCALE_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${FFMPEG_LIBRARIES} ${SWSCALE_LIBRARIES})
  endif()
  target_compile_definitions(${PROJECT_NAME} PRIVATE "VLINK_ENABLE_VIEWER_FFMPEG")
endif()

if(ENABLE_VIEWER_OSG)
  if(DEFINED ENV{OSG_DIR})
    set(OSG_DIR $ENV{OSG_DIR})
  endif()
  if(NOT DEFINED OPENSCENEGRAPH_INCLUDE_DIRS AND DEFINED OpenSceneGraph_INCLUDE_DIRS)
    set(OPENSCENEGRAPH_INCLUDE_DIRS ${OpenSceneGraph_INCLUDE_DIRS})
  endif()
  if(NOT DEFINED OPENSCENEGRAPH_LIBRARIES AND DEFINED OpenSceneGraph_LIBRARIES)
    set(OPENSCENEGRAPH_LIBRARIES ${OpenSceneGraph_LIBRARIES})
  endif()
  target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE ${OPENSCENEGRAPH_INCLUDE_DIRS})
  target_link_libraries(${PROJECT_NAME} PRIVATE ${OPENSCENEGRAPH_LIBRARIES})
  target_compile_definitions(${PROJECT_NAME} PRIVATE "VLINK_ENABLE_VIEWER_OSG")
endif()

if(WIN32)
  if(EXISTS ${CMAKE_SOURCE_DIR}/packup/ico/vlink-viewer.rc)
    target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/packup/ico/vlink-viewer.rc)
  endif()
  set_property(TARGET ${PROJECT_NAME} PROPERTY WIN32_EXECUTABLE TRUE)
elseif(APPLE)
  target_compile_options(${PROJECT_NAME} PRIVATE "-Wno-deprecated-declarations")
endif()

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(TARGET thirdparty::json)
  target_link_libraries(${PROJECT_NAME} PRIVATE thirdparty::json)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/json/include/nlohmann/json.hpp)
  target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/json/include)
endif()

if(NOT TARGET vlink-exprtk_api)
  find_package(vlink-exprtk_api QUIET)
endif()

if(TARGET vlink::exprtk_api)
  target_link_libraries(${PROJECT_NAME} PRIVATE vlink::exprtk_api)
endif()

target_link_libraries(
  ${PROJECT_NAME} PRIVATE vlink::proxy_api protobuf::libprotobuf Qt${QT_VERSION_MAJOR}::Widgets
                          Qt${QT_VERSION_MAJOR}::Sql Qt${QT_VERSION_MAJOR}::Network thirdparty::eigen3
)

if(TARGET flatbuffers::flatbuffers_shared)
  target_link_libraries(${PROJECT_NAME} PRIVATE flatbuffers::flatbuffers_shared)
elseif(TARGET flatbuffers::flatbuffers)
  target_link_libraries(${PROJECT_NAME} PRIVATE flatbuffers::flatbuffers)
endif()

if(QT_VERSION_MAJOR EQUAL 6)
  target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::OpenGLWidgets)
endif()

# player
add_subdirectory(player)

# analyzer
add_subdirectory(thirdparty/qcustomplot)
add_subdirectory(analyzer)

# install
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# etc
if(EXISTS ${PROJECT_SOURCE_DIR}/etc)
  add_custom_command(
    TARGET ${PROJECT_NAME}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/etc/
            ${CMAKE_BINARY_DIR}/output/${BUILD_CONFIG_DIR}/vlink-viewer/
  )
  install(
    DIRECTORY ${PROJECT_SOURCE_DIR}/etc/
    DESTINATION ${INSTALL_CONFIG_DIR}/vlink-viewer/
    REGEX "completions" EXCLUDE
  )
endif()
