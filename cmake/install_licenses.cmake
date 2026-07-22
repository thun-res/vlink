#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

set(VLINK_LICENSES_DIR ${INSTALL_CONFIG_DIR}/licenses)

set(_vlink_license_candidates
    LICENSE
    LICENSE.txt
    LICENSE.md
    LICENSE.MIT
    LICENSE_1_0.txt
    NOTICE
    NOTICE.txt
    NOTICE.md
    COPYING
    COPYING.txt
    COPYING.MPL2
    COPYING.BSD
    COPYING.APACHE
    COPYING.README
    COPYRIGHT
    license.txt
)

function(vlink_install_license_dir name src_dir)
  if(NOT EXISTS ${src_dir})
    return()
  endif()
  set(_files)
  foreach(_f IN LISTS _vlink_license_candidates)
    if(EXISTS ${src_dir}/${_f})
      list(APPEND _files ${src_dir}/${_f})
    endif()
  endforeach()
  if(_files)
    install(FILES ${_files} DESTINATION ${VLINK_LICENSES_DIR}/${name})
  endif()
endfunction()

function(vlink_install_license_files name src_dir)
  set(_files)
  foreach(_f IN LISTS ARGN)
    if(EXISTS ${src_dir}/${_f})
      list(APPEND _files ${src_dir}/${_f})
    endif()
  endforeach()
  if(_files)
    install(FILES ${_files} DESTINATION ${VLINK_LICENSES_DIR}/${name})
  endif()
endfunction()

function(vlink_install_thirdparty_license name)
  vlink_install_license_dir(${name} ${CMAKE_SOURCE_DIR}/thirdparty/${name})
endfunction()

set(_VLINK_USE_ARGPARSE OFF)
foreach(
  _cli
  INFO
  BAG
  EPROTO
  EFBS
  LIST
  MONITOR
  PARSE
  CHECK
  BENCH
  TRIGGER
)
  if(ENABLE_CLI_${_cli})
    set(_VLINK_USE_ARGPARSE ON)
  endif()
endforeach()
if(ENABLE_PROXY OR ENABLE_WEBVIZ)
  set(_VLINK_USE_ARGPARSE ON)
endif()

set(_VLINK_USE_EXPRTK OFF)
if(ENABLE_EXPRTK
   AND (ENABLE_CLI_PARSE
        OR ENABLE_VIEWER
        OR ENABLE_WEBVIZ)
)
  set(_VLINK_USE_EXPRTK ON)
endif()

set(_VLINK_USE_WEBVIZ_FOXGLOVE OFF)
if(ENABLE_WEBVIZ AND ENABLE_WEBVIZ_FOXGLOVE)
  set(_VLINK_USE_WEBVIZ_FOXGLOVE ON)
endif()

install(FILES ${CMAKE_SOURCE_DIR}/LICENSE DESTINATION ${VLINK_LICENSES_DIR}/vlink)

set(_vlink_core_thirdparty lzav mcap json robin-map)
foreach(_pkg IN LISTS _vlink_core_thirdparty)
  vlink_install_thirdparty_license(${_pkg})
endforeach()

set(_vlink_optional_thirdparty
    spdlog:ENABLE_LOG_SPD
    quill:ENABLE_LOG_QUI
    argparse:_VLINK_USE_ARGPARSE
    bitsery:ENABLE_PROXY
    exprtk:_VLINK_USE_EXPRTK
    websocketpp:_VLINK_USE_WEBVIZ_FOXGLOVE
    asio:_VLINK_USE_WEBVIZ_FOXGLOVE
)
foreach(_entry IN LISTS _vlink_optional_thirdparty)
  string(REPLACE ":" ";" _pair "${_entry}")
  list(GET _pair 0 _name)
  list(GET _pair 1 _guard)
  if(${_guard})
    vlink_install_thirdparty_license(${_name})
  endif()
endforeach()

if(ENABLE_VIEWER)
  vlink_install_license_files(eigen3 ${CMAKE_SOURCE_DIR}/thirdparty/eigen3 COPYING.MPL2 COPYING.README)
  vlink_install_license_files(qcustomplot ${CMAKE_SOURCE_DIR}/viewer/thirdparty/qcustomplot GPL.txt changelog.txt)
endif()

if(ENABLE_CPM OR ENABLE_CPM_ALL)
  set(_vlink_cpm_packages
      tinyxml2
      cpptoml
      foonathan_memory
      fastcdr
      fastdds
      cyclonedds
      iceoryx
      protobuf
      flatbuffers
      sqlite3
      openssl
  )
  foreach(_pkg IN LISTS _vlink_cpm_packages)
    if(DEFINED ${_pkg}_SOURCE_DIR)
      vlink_install_license_dir(${_pkg} ${${_pkg}_SOURCE_DIR})
    endif()
  endforeach()
  if(DEFINED zstd_SOURCE_DIR)
    vlink_install_license_files(zstd ${zstd_SOURCE_DIR} LICENSE)
  endif()

  file(GLOB _notice_files "${CMAKE_SOURCE_DIR}/packup/patch/*.NOTICE.md")
  foreach(_f IN LISTS _notice_files)
    get_filename_component(_basename "${_f}" NAME)
    string(REGEX REPLACE "\\.NOTICE\\.md$" "" _versioned "${_basename}")
    string(REGEX REPLACE "_[0-9]+\\.[0-9.]+x$" "" _id "${_versioned}")
    if(DEFINED ${_id}_SOURCE_DIR)
      install(
        FILES ${_f}
        DESTINATION ${VLINK_LICENSES_DIR}/${_id}
        RENAME MODIFICATIONS.md
      )
    endif()
  endforeach()
endif()

if(IS_ABSOLUTE "${VLINK_LICENSES_DIR}")
  set(VLINK_LICENSES_DISPLAY_PATH "${VLINK_LICENSES_DIR}")
else()
  set(VLINK_LICENSES_DISPLAY_PATH "${CMAKE_INSTALL_PREFIX}/${VLINK_LICENSES_DIR}")
endif()
configure_file(${CMAKE_SOURCE_DIR}/cmake/LICENSE_NOTICES.md.in ${CMAKE_BINARY_DIR}/LICENSE_NOTICES.md @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/LICENSE_NOTICES.md DESTINATION ${VLINK_LICENSES_DIR})

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  if(IS_ABSOLUTE "${VLINK_LICENSES_DIR}")
    set(_vlink_lic_link_target "${VLINK_LICENSES_DIR}")
  else()
    file(RELATIVE_PATH _vlink_lic_link_target "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATAROOTDIR}/licenses"
         "${CMAKE_INSTALL_PREFIX}/${VLINK_LICENSES_DIR}"
    )
  endif()
  vlink_install_symlink(
    ${CMAKE_PROJECT_NAME} SYMLINK ${_vlink_lic_link_target} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/licenses
  )
endif()
