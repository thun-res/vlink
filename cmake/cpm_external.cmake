#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output/external/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output/external/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output/external/lib)
# set(CMAKE_SKIP_INSTALL_RULES ON)

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

function(export)

endfunction()
