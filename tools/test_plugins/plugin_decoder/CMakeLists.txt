#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

cmake_minimum_required(VERSION 3.14...3.27)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# project
project(
  vlink-ffmpeg
  VERSION 1.1.1
  DESCRIPTION "Decoding library based on FFmpeg"
  LANGUAGES CXX
)
set(PROJECT_NAMESPACE vlink::)

# required
find_package(vlink CONFIG REQUIRED)
find_package(FFmpeg CONFIG QUIET)
if(NOT FFmpeg_FOUND)
  list(PREPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
  find_package(FFmpeg MODULE REQUIRED)
endif()

# common
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output/lib)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release")
endif()

# options
option(BUILD_SHARED_LIBS "Build shared libraries" ON)

# src
file(GLOB_RECURSE PROJECT_HDRS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/include/*.h)
file(GLOB_RECURSE PROJECT_SRCS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/*.cc)
add_library(${PROJECT_NAME} ${PROJECT_HDRS} ${PROJECT_SRCS})
target_include_directories(${PROJECT_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/include/${PROJECT_NAME})
target_compile_definitions(${PROJECT_NAME} PRIVATE "VLINK_ENABLE_VIEWER_FFMPEG")
target_link_libraries(${PROJECT_NAME} PUBLIC vlink::vlink)
if(TARGET ffmpeg::ffmpeg)
  target_link_libraries(${PROJECT_NAME} PRIVATE ffmpeg::ffmpeg)
else()
  target_include_directories(${PROJECT_NAME} PRIVATE ${FFMPEG_INCLUDE_DIRS} ${SWSCALE_INCLUDE_DIRS})
  target_link_libraries(${PROJECT_NAME} PRIVATE ${FFMPEG_LIBRARIES} ${SWSCALE_LIBRARIES})
endif()

# install
install(
  TARGETS ${PROJECT_NAME}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${PROJECT_SOURCE_DIR}/README.md ${PROJECT_SOURCE_DIR}/CHANGELOG.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/${CMAKE_PROJECT_NAME}
)

# export
vlink_export(${CMAKE_PROJECT_NAME} ${PROJECT_NAME} ${PROJECT_NAMESPACE} ${CMAKE_PROJECT_VERSION})
