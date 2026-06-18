#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

function(vlink_check)
  if(NOT CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    message(FATAL_ERROR "This project must be built in the top-level directory")
    return()
  endif()
  if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(
      FATAL_ERROR
        "In-source builds not allowed. Please make a new directory (called a build directory) and run CMake from there"
    )
  endif()
endfunction()

function(vlink_test_warn target)
  if(MSVC)
    target_compile_options(${target} PUBLIC /W4 /WX)
  elseif(QNX)
    target_compile_options(${target} PUBLIC -Wall -Werror)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(${target} PUBLIC -Wall -Wpedantic -Wextra -Werror)
  else()
    message(WARNING "Not support vlink_test_warn")
  endif()
endfunction()

function(vlink_test_sanitize target)
  if(NOT WIN32
     AND NOT QNX
     AND (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
  )
    target_compile_options(${target} PUBLIC -fsanitize=address)
    target_link_options(${target} PUBLIC -fsanitize=address)
  else()
    message(WARNING "Not support vlink_test_sanitize")
  endif()
endfunction()

function(vlink_test_coverage_by_gcovr target excludes options)
  execute_process(
    COMMAND gcovr --version
    OUTPUT_VARIABLE GCOVR_VERSION_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
  )
  string(REGEX MATCH "[0-9]+\\.[0-9]+" GCOVR_VERSION "${GCOVR_VERSION_OUTPUT}")
  if(${GCOVR_VERSION} STREQUAL "")
    message(WARNING "Can not find gcovr")
    return()
  else()
    message(STATUS "gcovr version: ${GCOVR_VERSION}")
  endif()
  target_compile_options(${target} PUBLIC -O0 -g -fprofile-arcs -ftest-coverage)
  target_link_options(${target} PUBLIC -fprofile-arcs -ftest-coverage)
  set(GCOVR_EXCLUDE_ARGS)
  foreach(EXCLUDE_PATH ${excludes})
    string(REPLACE "*" "'.*'" EXCLUDE_PATH ${EXCLUDE_PATH})
    set(GCOVR_EXCLUDE_ARGS "${GCOVR_EXCLUDE_ARGS} --exclude ${EXCLUDE_PATH}")
  endforeach()
  if(GCOVR_VERSION AND GCOVR_VERSION VERSION_GREATER_EQUAL "8.0")
    set(GCOVR_COMMANDS "gcovr ${CMAKE_BINARY_DIR} -r ${CMAKE_SOURCE_DIR} -s \\\n")
    set(GCOVR_COMMANDS
        "${GCOVR_COMMANDS} --gcov-ignore-errors --gcov-ignore-parse-errors --exclude-unreachable-branches \\\n"
    )
    set(GCOVR_COMMANDS "${GCOVR_COMMANDS} --html --html-nested --html-syntax-highlighting \\\n")
    set(GCOVR_COMMANDS "${GCOVR_COMMANDS} --html-theme github.dark-green \\\n")
    set(GCOVR_COMMANDS
        "${GCOVR_COMMANDS} --html-title \"Code Coverage Report for Project [${CMAKE_PROJECT_NAME}]\" \\\n"
    )
    set(GCOVR_COMMANDS "${GCOVR_COMMANDS} ${GCOVR_EXCLUDE_ARGS} \\\n")
    set(GCOVR_COMMANDS "${GCOVR_COMMANDS} ${options} \\\n")
    set(GCOVR_COMMANDS "${GCOVR_COMMANDS} -o ${CMAKE_BINARY_DIR}/coverage/index.html")
  else()
    set(GCOVR_COMMANDS "gcovr ${CMAKE_BINARY_DIR} -r ${CMAKE_SOURCE_DIR} -s \\\n")
    set(GCOVR_COMMANDS "${GCOVR_COMMANDS} --gcov-ignore-parse-errors --exclude-unreachable-branches --branches \\\n")
    set(GCOVR_COMMANDS "${GCOVR_COMMANDS} --html --html-details \\\n")
    set(GCOVR_COMMANDS
        "${GCOVR_COMMANDS} --html-title \"Code Coverage Report for Project [${CMAKE_PROJECT_NAME}]\" \\\n"
    )
    set(GCOVR_COMMANDS "${GCOVR_COMMANDS} ${GCOVR_EXCLUDE_ARGS} \\\n")
    set(GCOVR_COMMANDS "${GCOVR_COMMANDS} ${options} \\\n")
    set(GCOVR_COMMANDS "${GCOVR_COMMANDS} -o ${CMAKE_BINARY_DIR}/coverage/index.html")
  endif()
  file(WRITE ${CMAKE_BINARY_DIR}/gcovr_generate.sh "#!/usr/bin/env bash\n\nset -e\n\n")
  file(APPEND ${CMAKE_BINARY_DIR}/gcovr_generate.sh "echo -e \"\\033[32mGenerate coverage by gcovr...\\033[0m\"\n\n")
  file(APPEND ${CMAKE_BINARY_DIR}/gcovr_generate.sh "mkdir -p ${CMAKE_BINARY_DIR}/coverage\n\n")
  file(APPEND ${CMAKE_BINARY_DIR}/gcovr_generate.sh "${GCOVR_COMMANDS}\n")
  file(APPEND ${CMAKE_BINARY_DIR}/gcovr_generate.sh
       "\necho -e \"\\033[32mCoverage report is in: file://${CMAKE_BINARY_DIR}/coverage/index.html\\033[0m\"\n"
  )
  file(APPEND ${CMAKE_BINARY_DIR}/gcovr_generate.sh "\nexit 0\n")
  file(
    CHMOD
    ${CMAKE_BINARY_DIR}/gcovr_generate.sh
    PERMISSIONS
    OWNER_READ
    OWNER_WRITE
    OWNER_EXECUTE
    GROUP_READ
    GROUP_EXECUTE
    WORLD_READ
    WORLD_EXECUTE
  )
  if(APPLE)
    set(_OPEN_EXE open)
  else()
    set(_OPEN_EXE xdg-open)
  endif()
  add_custom_target(
    coverage
    COMMAND ${CMAKE_BINARY_DIR}/gcovr_generate.sh
    COMMAND ${_OPEN_EXE} ${CMAKE_BINARY_DIR}/coverage/index.html >/dev/null 2>&1 || (exit 0)
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )
endfunction()

function(vlink_test_coverage_by_lcov target excludes options)
  execute_process(
    COMMAND lcov --version
    OUTPUT_VARIABLE LCOV_VERSION_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
  )
  string(REGEX MATCH "[0-9]+\\.[0-9]+" LCOV_VERSION "${LCOV_VERSION_OUTPUT}")
  if(${LCOV_VERSION} STREQUAL "")
    message(WARNING "Can not find lcov")
    return()
  else()
    message(STATUS "lcov version: ${LCOV_VERSION}")
  endif()
  if(LCOV_VERSION AND LCOV_VERSION VERSION_GREATER_EQUAL "2.0")
    set(LCOV_EXT_ARGS
        "--rc branch_coverage=1 --quiet --rc check_data_consistency=0 --ignore-errors mismatch,mismatch,inconsistent,inconsistent,negative,negative --filter branch --filter function ${options}"
    )
  else()
    set(LCOV_EXT_ARGS "--rc lcov_branch_coverage=1 ${options}")
  endif()
  target_compile_options(${target} PUBLIC -O0 -g -fprofile-arcs -ftest-coverage)
  target_link_options(${target} PUBLIC -fprofile-arcs -ftest-coverage)
  file(WRITE ${CMAKE_BINARY_DIR}/lcov_generate.sh "#!/usr/bin/env bash\n\nset -e\n\n")
  file(APPEND ${CMAKE_BINARY_DIR}/lcov_generate.sh "echo -e \"\\033[32mGenerate coverage by lcov...\\033[0m\"\n\n")
  file(APPEND ${CMAKE_BINARY_DIR}/lcov_generate.sh "mkdir -p ${CMAKE_BINARY_DIR}/coverage\n\n")
  file(APPEND ${CMAKE_BINARY_DIR}/lcov_generate.sh "LCOV_EXT_ARGS=\"${LCOV_EXT_ARGS}\"\n\n")
  file(APPEND ${CMAKE_BINARY_DIR}/lcov_generate.sh
       "lcov    \$(echo \$LCOV_EXT_ARGS) --capture --directory ./ --output-file cov.info\n"
  )
  file(APPEND ${CMAKE_BINARY_DIR}/lcov_generate.sh
       "lcov    \$(echo \$LCOV_EXT_ARGS) --extract cov.info -o cov.info '${CMAKE_SOURCE_DIR}/*'\n"
  )
  foreach(EXCLUDE_PATH ${excludes})
    file(APPEND ${CMAKE_BINARY_DIR}/lcov_generate.sh
         "lcov    \$(echo \$LCOV_EXT_ARGS) --remove cov.info -o cov.info '${EXCLUDE_PATH}' >/dev/null 2>&1 || true\n"
    )
  endforeach()
  file(APPEND ${CMAKE_BINARY_DIR}/lcov_generate.sh
       "genhtml \$(echo \$LCOV_EXT_ARGS) --title '${target}' cov.info -o coverage\n"
  )
  file(APPEND ${CMAKE_BINARY_DIR}/lcov_generate.sh
       "\necho -e \"\\033[32mCoverage report is in: file://${CMAKE_BINARY_DIR}/coverage/index.html\\033[0m\"\n"
  )
  file(APPEND ${CMAKE_BINARY_DIR}/lcov_generate.sh "\nexit 0\n")
  file(
    CHMOD
    ${CMAKE_BINARY_DIR}/lcov_generate.sh
    PERMISSIONS
    OWNER_READ
    OWNER_WRITE
    OWNER_EXECUTE
    GROUP_READ
    GROUP_EXECUTE
    WORLD_READ
    WORLD_EXECUTE
  )
  if(APPLE)
    set(_OPEN_EXE open)
  else()
    set(_OPEN_EXE xdg-open)
  endif()
  add_custom_target(
    coverage
    COMMAND ${CMAKE_BINARY_DIR}/lcov_generate.sh
    COMMAND ${_OPEN_EXE} ${CMAKE_BINARY_DIR}/coverage/index.html >/dev/null 2>&1 || (exit 0)
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )
endfunction()

function(vlink_test_coverage target)
  if(WIN32)
    message(WARNING "Not support vlink_test_coverage")
    return()
  endif()
  set(options)
  set(one_value_args TYPE OPTION)
  set(multi_value_args EXCLUDES)
  cmake_parse_arguments(VLINK_TC "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})
  if("${VLINK_TC_TYPE}" STREQUAL "")
    find_program(GCOVR_EXECUTABLE gcovr)
    if(GCOVR_EXECUTABLE)
      vlink_test_coverage_by_gcovr(${target} "${VLINK_TC_EXCLUDES}" "${VLINK_TC_OPTION}")
    else()
      vlink_test_coverage_by_lcov(${target} "${VLINK_TC_EXCLUDES}" "${VLINK_TC_OPTION}")
    endif()
  elseif("${VLINK_TC_TYPE}" STREQUAL "gcovr")
    vlink_test_coverage_by_gcovr(${target} "${VLINK_TC_EXCLUDES}" "${VLINK_TC_OPTION}")
  elseif("${VLINK_TC_TYPE}" STREQUAL "lcov")
    vlink_test_coverage_by_lcov(${target} "${VLINK_TC_EXCLUDES}" "${VLINK_TC_OPTION}")
  else()
    message(FATAL_ERROR "Not support coverage type [${VLINK_TC_TYPE}]")
    return()
  endif()
endfunction()

function(vlink_export project_name)
  set(target ${project_name})
  set(namespace "")
  set(version ${CMAKE_PROJECT_VERSION})
  if(ARGC GREATER 1)
    set(target "${ARGV1}")
  endif()
  if(ARGC GREATER 2)
    set(namespace "${ARGV2}")
  endif()
  if(ARGC GREATER 3)
    set(version "${ARGV3}")
  endif()
  if(NOT CMAKE_INSTALL_LIBDIR)
    include(GNUInstallDirs)
  endif()
  if(NOT COMMAND write_basic_package_version_file)
    include(CMakePackageConfigHelpers)
  endif()
  get_target_property(_vlink_export_type ${target} TYPE)
  if(_vlink_export_type MATCHES "^(SHARED_LIBRARY|MODULE_LIBRARY)$")
    set_target_properties(
      ${target} PROPERTIES VERSION ${version} SOVERSION ${vlink_VERSION_MAJOR}.${vlink_VERSION_MINOR}
    )
  endif()
  if(DEFINED VLINK_MODULE_NAME)
    set(in_config_name module.cmake.in)
  else()
    set(in_config_name config.cmake.in)
  endif()
  if(EXISTS ${CMAKE_SOURCE_DIR}/cmake/${in_config_name})
    write_basic_package_version_file(
      ${CMAKE_CURRENT_BINARY_DIR}/${project_name}-config-version.cmake
      VERSION ${version}
      COMPATIBILITY SameMajorVersion
    )
    configure_package_config_file(
      ${CMAKE_SOURCE_DIR}/cmake/${in_config_name} ${CMAKE_CURRENT_BINARY_DIR}/${project_name}-config.cmake
      INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${project_name}
    )
    install(
      FILES ${CMAKE_CURRENT_BINARY_DIR}/${project_name}-config.cmake
            ${CMAKE_CURRENT_BINARY_DIR}/${project_name}-config-version.cmake
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${project_name}
      COMPONENT devel
    )
    install(
      TARGETS ${target}
      EXPORT ${project_name}-targets
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT runtime
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT devel
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
              COMPONENT runtime
              NAMELINK_COMPONENT devel
    )
    install(
      EXPORT ${project_name}-targets
      NAMESPACE "${namespace}"
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${project_name}
      COMPONENT devel
    )
  else()
    install(
      TARGETS ${target}
      EXPORT ${project_name}-config
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT runtime
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT devel
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
              COMPONENT runtime
              NAMELINK_COMPONENT devel
    )
    install(
      EXPORT ${project_name}-config
      NAMESPACE "${namespace}"
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${project_name}
      COMPONENT devel
    )
  endif()
  if("${project_name}" STREQUAL "${CMAKE_PROJECT_NAME}")
    if(EXISTS ${CMAKE_SOURCE_DIR}/cmake/functions)
      install(
        DIRECTORY ${CMAKE_SOURCE_DIR}/cmake/functions
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${CMAKE_PROJECT_NAME}
        COMPONENT devel
      )
    endif()
    if(EXISTS ${CMAKE_SOURCE_DIR}/include/version_config.h.in)
      configure_file(
        ${CMAKE_SOURCE_DIR}/include/version_config.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/config/${CMAKE_PROJECT_NAME}/version_config.h @ONLY
      )
      target_include_directories(${CMAKE_PROJECT_NAME} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/config>)
      install(
        DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/config/${CMAKE_PROJECT_NAME}
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        COMPONENT devel
      )
    endif()
  endif()
endfunction()

function(vlink_install_symlink name)
  set(options ABSOLUTE)
  set(oneValueArgs SYMLINK DESTINATION)
  set(multiValueArgs "")
  cmake_parse_arguments(VLINK_SYM "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
  if(NOT VLINK_SYM_SYMLINK)
    message(FATAL_ERROR "vlink_install_symlink: SYMLINK argument is required")
  endif()
  if(NOT VLINK_SYM_DESTINATION)
    message(FATAL_ERROR "vlink_install_symlink: DESTINATION argument is required")
  endif()
  if(WIN32)
    return()
  endif()
  set(_SYMLINK_MANIFEST "${CMAKE_BINARY_DIR}/symlinks_manifest.txt")
  get_property(_manifest_cleared GLOBAL PROPERTY _VLINK_SYMLINK_MANIFEST_CLEARED)
  if(NOT _manifest_cleared)
    install(CODE "file(REMOVE \"${_SYMLINK_MANIFEST}\")")
    set_property(GLOBAL PROPERTY _VLINK_SYMLINK_MANIFEST_CLEARED TRUE)
  endif()
  install(
    CODE "
      set(_vlink_link_dst \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${VLINK_SYM_DESTINATION}/${name}\")
      set(_vlink_link_log \"\${CMAKE_INSTALL_PREFIX}/${VLINK_SYM_DESTINATION}/${name}\")
      get_filename_component(_vlink_link_dir \"\${_vlink_link_dst}\" DIRECTORY)
      file(MAKE_DIRECTORY \"\${_vlink_link_dir}\")
      execute_process(
        COMMAND \${CMAKE_COMMAND} -E create_symlink \"${VLINK_SYM_SYMLINK}\" \"\${_vlink_link_dst}\"
      )
      file(APPEND \"${_SYMLINK_MANIFEST}\" \"\${_vlink_link_log}\\n\")
      message(STATUS \"Created symlink: \${_vlink_link_log} -> ${VLINK_SYM_SYMLINK}\")
    "
  )
endfunction()

function(vlink_install_completions cmdname)
  set(options)
  set(oneValueArgs INSTALL_DIR)
  set(multiValueArgs ALIAS)
  cmake_parse_arguments(VLINK_COMP "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
  if(WIN32)
    return()
  endif()
  set(_target "${cmdname}")
  set(_src_dir "${CMAKE_CURRENT_SOURCE_DIR}/etc/completions")
  set(_bash_src "${_src_dir}/${_target}.bash")
  set(_zsh_src "${_src_dir}/${_target}.zsh")
  if(NOT EXISTS "${_src_dir}")
    message(WARNING "vlink_install_completions: ${_src_dir} does not exist; skipping")
    return()
  endif()
  if(DEFINED VLINK_COMP_INSTALL_DIR)
    set(_install_dir "${VLINK_COMP_INSTALL_DIR}")
  elseif(DEFINED INSTALL_CONFIG_DIR)
    set(_install_dir "${INSTALL_CONFIG_DIR}")
  else()
    set(_install_dir "etc/vlink")
  endif()
  if(IS_ABSOLUTE "${_install_dir}")
    set(_install_full_dir "${_install_dir}")
    string(REGEX REPLACE "^([A-Za-z]:)?[\\/]+" "" _build_install_dir "${_install_dir}")
  else()
    set(_install_full_dir "${CMAKE_INSTALL_PREFIX}/${_install_dir}")
    set(_build_install_dir "${_install_dir}")
  endif()
  if(TARGET ${_target})
    add_custom_command(
      TARGET ${_target}
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${_src_dir}/"
              "${CMAKE_BINARY_DIR}/output/${_build_install_dir}/vlink-completions/"
      COMMENT "Copying ${_target} completion scripts to build output"
    )
  endif()
  install(DIRECTORY "${_src_dir}/" DESTINATION "${_install_dir}/vlink-completions/")
  set(_stub_dir "${CMAKE_BINARY_DIR}/completions-stubs/${_target}")
  file(MAKE_DIRECTORY "${_stub_dir}")
  set(_compdef_line "#compdef ${_target}")
  foreach(_alias IN LISTS VLINK_COMP_ALIAS)
    if(NOT _alias STREQUAL "${_target}")
      string(APPEND _compdef_line " ${_alias}")
    endif()
  endforeach()
  set(_loader_path "${_install_full_dir}/vlink-completions.sh")
  if(EXISTS "${_bash_src}")
    set(_bash_stub "${_stub_dir}/${_target}")
    file(
      WRITE "${_bash_stub}"
      "# bash completion auto-loader for ${_target}\n"
      "# (auto-generated by CMake; do not edit)\n"
      "\n"
      "if [[ -z \"\${_VLINK_COMPLETIONS_LOADED:-}\" ]]; then\n"
      "    __vlink_loader=\"${_loader_path}\"\n"
      "    if [[ -r \"\$__vlink_loader\" ]]; then\n"
      "        # shellcheck disable=SC1090\n"
      "        source \"\$__vlink_loader\"\n"
      "        _VLINK_COMPLETIONS_LOADED=1\n"
      "    fi\n"
      "    unset __vlink_loader\n"
      "fi\n"
    )
    install(FILES "${_bash_stub}" DESTINATION "${CMAKE_INSTALL_DATADIR}/bash-completion/completions")
    foreach(_alias IN LISTS VLINK_COMP_ALIAS)
      if(NOT _alias STREQUAL "${_target}")
        install(
          FILES "${_bash_stub}"
          DESTINATION "${CMAKE_INSTALL_DATADIR}/bash-completion/completions"
          RENAME "${_alias}"
        )
      endif()
    endforeach()
  endif()
  if(EXISTS "${_zsh_src}")
    set(_zsh_stub "${_stub_dir}/_${_target}")
    file(
      WRITE "${_zsh_stub}"
      "${_compdef_line}\n"
      "# zsh completion auto-loader for ${_target}\n"
      "# (auto-generated by CMake; do not edit)\n"
      "\n"
      "if [[ -z \"\${_VLINK_COMPLETIONS_LOADED:-}\" ]]; then\n"
      "    local __vlink_loader=\"${_loader_path}\"\n"
      "    if [[ -r \"\$__vlink_loader\" ]]; then\n"
      "        source \"\$__vlink_loader\"\n"
      "        typeset -g _VLINK_COMPLETIONS_LOADED=1\n"
      "    fi\n"
      "fi\n"
      "\n"
      "# Dispatch to the real completion function registered by the loader.\n"
      "if (( \$+functions[_${_target}] )); then\n"
      "    _${_target} \"\$@\"\n"
      "fi\n"
    )
    install(FILES "${_zsh_stub}" DESTINATION "${CMAKE_INSTALL_DATADIR}/zsh/site-functions")
  endif()
endfunction()

function(vlink_log)
  if(NOT DEFINED VLINK_INFO_FILE)
    set(VLINK_INFO_FILE ${CMAKE_BINARY_DIR}/vlink-options.txt)
  endif()
  if(${ARGC} EQUAL 0)
    file(WRITE "${VLINK_INFO_FILE}" "")
  else()
    message("${ARGV0}")
    file(APPEND "${VLINK_INFO_FILE}" "${ARGV0}\n")
  endif()
endfunction()
