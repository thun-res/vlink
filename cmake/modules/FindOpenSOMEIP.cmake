# FindOpenSOMEIP.cmake
#
# Locates a host build of OpenSOMEIP and exposes OpenSOMEIP::OpenSOMEIP.
#
# Input variables: OpenSOMEIP_ROOT  OpenSOMEIP installation prefix or built source tree.
#
# Output variables: OpenSOMEIP_FOUND OpenSOMEIP_VERSION OpenSOMEIP_INCLUDE_DIRS OpenSOMEIP_LIBRARIES

include(FindPackageHandleStandardArgs)

set(OpenSOMEIP_ROOT
    ""
    CACHE PATH "OpenSOMEIP installation prefix or built source tree"
)

if(TARGET OpenSOMEIP::OpenSOMEIP)
  set(OpenSOMEIP_LIBRARIES OpenSOMEIP::OpenSOMEIP)
  find_package_handle_standard_args(OpenSOMEIP REQUIRED_VARS OpenSOMEIP_LIBRARIES)
  return()
endif()

if(TARGET opensomeip)
  add_library(OpenSOMEIP::OpenSOMEIP INTERFACE IMPORTED GLOBAL)
  set_property(TARGET OpenSOMEIP::OpenSOMEIP PROPERTY INTERFACE_LINK_LIBRARIES opensomeip)
  set(OpenSOMEIP_LIBRARIES OpenSOMEIP::OpenSOMEIP)
  find_package_handle_standard_args(OpenSOMEIP REQUIRED_VARS OpenSOMEIP_LIBRARIES)
  return()
endif()

set(_OpenSOMEIP_roots "")

if(OpenSOMEIP_ROOT)
  list(APPEND _OpenSOMEIP_roots "${OpenSOMEIP_ROOT}")
endif()

if(DEFINED ENV{OpenSOMEIP_ROOT} AND NOT "$ENV{OpenSOMEIP_ROOT}" STREQUAL "")
  list(APPEND _OpenSOMEIP_roots "$ENV{OpenSOMEIP_ROOT}")
endif()

find_path(
  OpenSOMEIP_INCLUDE_DIR
  NAMES someip/message.h
  HINTS ${_OpenSOMEIP_roots}
  PATH_SUFFIXES include include/someip
)

find_library(
  OpenSOMEIP_LIBRARY
  NAMES opensomeip
  HINTS ${_OpenSOMEIP_roots}
  PATH_SUFFIXES lib lib64 build/lib
)

if(WIN32)
  set(_OpenSOMEIP_threading_backend win32)
  set(_OpenSOMEIP_net_backend win32)
else()
  set(_OpenSOMEIP_threading_backend posix)
  set(_OpenSOMEIP_net_backend posix)
endif()

set(OpenSOMEIP_ALLOC_INCLUDE_DIR "${OpenSOMEIP_INCLUDE_DIR}/platform/dynamic")
set(OpenSOMEIP_THREADING_INCLUDE_DIR "${OpenSOMEIP_INCLUDE_DIR}/platform/${_OpenSOMEIP_threading_backend}")
set(OpenSOMEIP_NET_INCLUDE_DIR "${OpenSOMEIP_INCLUDE_DIR}/platform/${_OpenSOMEIP_net_backend}")

if(NOT EXISTS "${OpenSOMEIP_ALLOC_INCLUDE_DIR}/buffer_pool_impl.h")
  unset(OpenSOMEIP_ALLOC_INCLUDE_DIR)
endif()

if(NOT EXISTS "${OpenSOMEIP_THREADING_INCLUDE_DIR}/thread_impl.h")
  unset(OpenSOMEIP_THREADING_INCLUDE_DIR)
endif()

if(NOT EXISTS "${OpenSOMEIP_NET_INCLUDE_DIR}/net_impl.h")
  unset(OpenSOMEIP_NET_INCLUDE_DIR)
endif()

foreach(_OpenSOMEIP_root IN LISTS _OpenSOMEIP_roots)
  if(EXISTS "${_OpenSOMEIP_root}/VERSION")
    file(STRINGS "${_OpenSOMEIP_root}/VERSION" OpenSOMEIP_VERSION LIMIT_COUNT 1)
    string(STRIP "${OpenSOMEIP_VERSION}" OpenSOMEIP_VERSION)
    break()
  endif()
endforeach()

find_package_handle_standard_args(
  OpenSOMEIP
  REQUIRED_VARS OpenSOMEIP_INCLUDE_DIR OpenSOMEIP_ALLOC_INCLUDE_DIR OpenSOMEIP_THREADING_INCLUDE_DIR
                OpenSOMEIP_NET_INCLUDE_DIR OpenSOMEIP_LIBRARY
  VERSION_VAR OpenSOMEIP_VERSION
)

if(OpenSOMEIP_FOUND)
  set(OpenSOMEIP_INCLUDE_DIRS "${OpenSOMEIP_INCLUDE_DIR}" "${OpenSOMEIP_ALLOC_INCLUDE_DIR}"
                              "${OpenSOMEIP_THREADING_INCLUDE_DIR}" "${OpenSOMEIP_NET_INCLUDE_DIR}"
  )
  set(OpenSOMEIP_LIBRARIES OpenSOMEIP::OpenSOMEIP)

  add_library(OpenSOMEIP::OpenSOMEIP UNKNOWN IMPORTED GLOBAL)
  set_target_properties(
    OpenSOMEIP::OpenSOMEIP PROPERTIES IMPORTED_LOCATION "${OpenSOMEIP_LIBRARY}" INTERFACE_INCLUDE_DIRECTORIES
                                                                                "${OpenSOMEIP_INCLUDE_DIRS}"
  )

  if(WIN32)
    set_property(TARGET OpenSOMEIP::OpenSOMEIP PROPERTY INTERFACE_LINK_LIBRARIES ws2_32)
  else()
    find_package(Threads REQUIRED)
    set_property(TARGET OpenSOMEIP::OpenSOMEIP PROPERTY INTERFACE_LINK_LIBRARIES Threads::Threads)
  endif()
endif()

mark_as_advanced(
  OpenSOMEIP_INCLUDE_DIR OpenSOMEIP_ALLOC_INCLUDE_DIR OpenSOMEIP_THREADING_INCLUDE_DIR OpenSOMEIP_NET_INCLUDE_DIR
  OpenSOMEIP_LIBRARY
)
