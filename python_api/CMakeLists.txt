#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

set(VLINK_MODULE_NAME python_api)
set(VLINK_MODULE_IMPORT_ALL TRUE)

project(vlink-${VLINK_MODULE_NAME} LANGUAGES CXX)

find_package(
  Python
  COMPONENTS Interpreter Development
  REQUIRED
)

execute_process(
  COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
  OUTPUT_VARIABLE nanobind_DIR
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE nanobind_dir_result
)
if(NOT nanobind_dir_result EQUAL 0 OR nanobind_DIR STREQUAL "")
  message(FATAL_ERROR "Failed to locate nanobind CMake package directory.")
endif()

set(nanobind_DIR
    "${nanobind_DIR}"
    CACHE PATH "Path to the nanobind CMake package" FORCE
)
find_package(nanobind CONFIG REQUIRED PATHS "${nanobind_DIR}" NO_DEFAULT_PATH)

nanobind_add_module(_vlink_nanobind NB_SUPPRESS_WARNINGS vlink_python.cc)

target_link_libraries(_vlink_nanobind PRIVATE vlink::vlink vlink::all)
set_target_properties(
  _vlink_nanobind
  PROPERTIES CXX_CLANG_TIDY ""
             INTERPROCEDURAL_OPTIMIZATION FALSE
             LIBRARY_OUTPUT_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
)

install(TARGETS _vlink_nanobind LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/python)

install(FILES vlink.py DESTINATION ${CMAKE_INSTALL_LIBDIR}/python)
