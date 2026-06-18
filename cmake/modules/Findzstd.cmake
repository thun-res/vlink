# Findzstd.cmake
#
# Locates zstd and exposes the following targets: zstd::libzstd_shared  - shared library variant (when available)
# zstd::libzstd_static  - static library variant (when available) zstd::libzstd         - alias to one of the above
# (shared preferred) zstd::zstd            - alias to one of the above (shared preferred)
#
# Output variables: zstd_FOUND zstd_VERSION zstd_INCLUDE_DIRS    (may be empty when CONFIG mode resolves via target)
# zstd_LIBRARIES
#
# Note: Call find_package(zstd) at the top-level CMakeLists.txt so the imported/alias targets are visible to all
# subdirectories.

include(FindPackageHandleStandardArgs)

# -----------------------------------------------------------------------------
# Helper: create zstd::libzstd and zstd::zstd as aliases to the available variant CMake forbids creating an ALIAS that
# points to another ALIAS, so resolve the primary target down to its real (non-alias) form first.
# -----------------------------------------------------------------------------
function(_zstd_export_compat_aliases)
  if(TARGET zstd::libzstd_shared)
    set(_primary zstd::libzstd_shared)
  elseif(TARGET zstd::libzstd_static)
    set(_primary zstd::libzstd_static)
  else()
    return()
  endif()

  get_target_property(_aliased ${_primary} ALIASED_TARGET)
  if(_aliased)
    set(_primary ${_aliased})
  endif()

  if(NOT TARGET zstd::libzstd)
    add_library(zstd::libzstd ALIAS ${_primary})
  endif()
  if(NOT TARGET zstd::zstd)
    add_library(zstd::zstd ALIAS ${_primary})
  endif()
endfunction()

# -----------------------------------------------------------------------------
# Helper: populate zstd_LIBRARIES with a target reference when not already set.
# -----------------------------------------------------------------------------
function(_zstd_fill_libraries_from_target tgt)
  if(NOT TARGET ${tgt})
    return()
  endif()
  if(NOT zstd_LIBRARIES)
    set(zstd_LIBRARIES
        "${tgt}"
        PARENT_SCOPE
    )
  endif()
endfunction()

# -----------------------------------------------------------------------------
# Short-circuit: targets already provided by an earlier find_package or by the parent project (e.g. CPM,
# add_subdirectory).
# -----------------------------------------------------------------------------
if(TARGET zstd::libzstd_shared OR TARGET zstd::libzstd_static)
  _zstd_export_compat_aliases()
  if(TARGET zstd::libzstd_shared)
    _zstd_fill_libraries_from_target(zstd::libzstd_shared)
  else()
    _zstd_fill_libraries_from_target(zstd::libzstd_static)
  endif()
  set(zstd_FOUND TRUE)
  find_package_handle_standard_args(
    zstd
    REQUIRED_VARS zstd_LIBRARIES
    VERSION_VAR zstd_VERSION
  )
  return()
endif()

# -----------------------------------------------------------------------------
# 1. CMake package config (zstdConfig.cmake from upstream install)
# -----------------------------------------------------------------------------
find_package(zstd ${zstd_FIND_VERSION} CONFIG QUIET)

if(zstd_FOUND AND (TARGET zstd::libzstd_shared OR TARGET zstd::libzstd_static))
  _zstd_export_compat_aliases()
  if(TARGET zstd::libzstd_shared)
    _zstd_fill_libraries_from_target(zstd::libzstd_shared)
  else()
    _zstd_fill_libraries_from_target(zstd::libzstd_static)
  endif()
  find_package_handle_standard_args(zstd CONFIG_MODE)
  return()
endif()

# Reset any partial state from a failed CONFIG attempt
unset(zstd_FOUND)
unset(zstd_VERSION)
unset(zstd_INCLUDE_DIRS)
unset(zstd_LIBRARIES)

# -----------------------------------------------------------------------------
# 1. pkg-config fallback
# -----------------------------------------------------------------------------
find_package(PkgConfig QUIET)

if(PkgConfig_FOUND)
  pkg_check_modules(pkgcfg_zstd QUIET IMPORTED_TARGET GLOBAL libzstd)

  if(pkgcfg_zstd_FOUND AND TARGET PkgConfig::pkgcfg_zstd)
    add_library(zstd::libzstd_shared ALIAS PkgConfig::pkgcfg_zstd)

    set(zstd_VERSION "${pkgcfg_zstd_VERSION}")
    set(zstd_INCLUDE_DIRS "${pkgcfg_zstd_INCLUDE_DIRS}")
    set(zstd_LIBRARIES "${pkgcfg_zstd_LINK_LIBRARIES}")
  endif()
endif()

# -----------------------------------------------------------------------------
# 1. Manual probe (last resort; does not handle transitive deps like pthread)
# -----------------------------------------------------------------------------
if(NOT TARGET zstd::libzstd_shared AND NOT TARGET zstd::libzstd_static)
  find_path(zstd_INCLUDE_DIR NAMES zstd.h)
  find_library(zstd_LIBRARY NAMES zstd libzstd)

  if(zstd_INCLUDE_DIR AND zstd_LIBRARY)
    add_library(zstd::libzstd_shared UNKNOWN IMPORTED GLOBAL)
    set_target_properties(
      zstd::libzstd_shared PROPERTIES IMPORTED_LOCATION "${zstd_LIBRARY}" INTERFACE_INCLUDE_DIRECTORIES
                                                                          "${zstd_INCLUDE_DIR}"
    )

    set(zstd_INCLUDE_DIRS "${zstd_INCLUDE_DIR}")
    set(zstd_LIBRARIES "${zstd_LIBRARY}")

    # Parse version from zstd.h
    if(EXISTS "${zstd_INCLUDE_DIR}/zstd.h")
      set(_ver_major "")
      set(_ver_minor "")
      set(_ver_release "")
      file(STRINGS "${zstd_INCLUDE_DIR}/zstd.h" _ver_lines
           REGEX "^#[ \t]*define[ \t]+ZSTD_VERSION_(MAJOR|MINOR|RELEASE)[ \t]+[0-9]+"
      )
      foreach(_line IN LISTS _ver_lines)
        if(_line MATCHES "ZSTD_VERSION_MAJOR[ \t]+([0-9]+)")
          set(_ver_major "${CMAKE_MATCH_1}")
        elseif(_line MATCHES "ZSTD_VERSION_MINOR[ \t]+([0-9]+)")
          set(_ver_minor "${CMAKE_MATCH_1}")
        elseif(_line MATCHES "ZSTD_VERSION_RELEASE[ \t]+([0-9]+)")
          set(_ver_release "${CMAKE_MATCH_1}")
        endif()
      endforeach()
      if(_ver_major
         AND _ver_minor
         AND _ver_release
      )
        set(zstd_VERSION "${_ver_major}.${_ver_minor}.${_ver_release}")
      endif()
      unset(_ver_lines)
      unset(_ver_major)
      unset(_ver_minor)
      unset(_ver_release)
    endif()
  endif()

  mark_as_advanced(zstd_INCLUDE_DIR zstd_LIBRARY)
endif()

# -----------------------------------------------------------------------------
# Finalize: export compat aliases and let FPHSA set zstd_FOUND
# -----------------------------------------------------------------------------
_zstd_export_compat_aliases()

find_package_handle_standard_args(
  zstd
  REQUIRED_VARS zstd_LIBRARIES
  VERSION_VAR zstd_VERSION
)
