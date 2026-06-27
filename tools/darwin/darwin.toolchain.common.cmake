set(CMAKE_SYSTEM_NAME Darwin)

if(NOT DEFINED ENV{DARWIN_INSTALL_PREFIX} AND DEFINED ENV{SYSROOT})
  set(ENV{DARWIN_INSTALL_PREFIX} "$ENV{SYSROOT}/usr")
endif()

# Root
if(DEFINED ENV{DARWIN_INSTALL_PREFIX})
  set(DARWIN_INSTALL_PREFIX "$ENV{DARWIN_INSTALL_PREFIX}")
  set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
  list(PREPEND CMAKE_FIND_ROOT_PATH "${DARWIN_INSTALL_PREFIX}")
  list(PREPEND _pkg_config "${DARWIN_INSTALL_PREFIX}/lib/pkgconfig")
  set(CMAKE_INSTALL_PREFIX
      "${DARWIN_INSTALL_PREFIX}"
      CACHE PATH "Install path"
  )
  set(CMAKE_SKIP_RPATH
      TRUE
      CACHE BOOL "Darwin skip rpath"
  )
  set(CMAKE_EXE_LINKER_FLAGS
      "-Wl,-rpath,${DARWIN_INSTALL_PREFIX}/lib"
      CACHE STRING "Darwin exe link rpath"
  )
  set(CMAKE_SHARED_LINKER_FLAGS
      "-Wl,-rpath,${DARWIN_INSTALL_PREFIX}/lib"
      CACHE STRING "Darwin shared link rpath"
  )
  set(CMAKE_C_FLAGS "-I${DARWIN_INSTALL_PREFIX}/include -L${DARWIN_INSTALL_PREFIX}/lib ${CMAKE_C_FLAGS}")
  set(CMAKE_CXX_FLAGS "-I${DARWIN_INSTALL_PREFIX}/include -L${DARWIN_INSTALL_PREFIX}/lib ${CMAKE_CXX_FLAGS}")
  list(FIND CMAKE_FIND_ROOT_PATH "${CMAKE_INSTALL_PREFIX}" _find_index)
  if(_find_index EQUAL -1 AND NOT "${CMAKE_INSTALL_PREFIX}" STREQUAL "${DARWIN_INSTALL_PREFIX}")
    list(PREPEND CMAKE_FIND_ROOT_PATH "${CMAKE_INSTALL_PREFIX}")
    list(PREPEND _pkg_config "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig")
  endif()
endif()
set(ENV{PKG_CONFIG_PATH} "${_pkg_config}")
unset(_find_index)
unset(_pkg_config)

# Options
add_compile_options(-Qunused-arguments)

# Flags
if(DEFINED ENV{CFLAGS})
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} $ENV{CFLAGS}")
endif()

if(DEFINED ENV{CXXFLAGS})
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} $ENV{CXXFLAGS}")
endif()
