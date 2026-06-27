#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

#[=======================================================================[.rst:
fastdds:
  example 1:
  vlink_generate_cpp (
    DDS
    sample.idl
  )
  add_executable(example ${VLINK_GEN_HDRS} ${VLINK_GEN_SRCS})
  example 2:
  vlink_generate_cpp (
    TARGET sample_target
    DDS
    sample.idl
    OUT_DIR "${CMAKE_BINARY_DIR}/out"
    FLAGS ""
  )
  target_link_libraries(example sample_target)

protobuf:
  example 1:
  vlink_generate_cpp (
    PROTO
    sample.proto
  )
  add_executable(example ${VLINK_GEN_HDRS} ${VLINK_GEN_SRCS})
  example 2:
  vlink_generate_cpp (
    TARGET sample_target
    PROTO
    sample.proto
    OUT_DIR "${CMAKE_BINARY_DIR}/out"
    FLAGS ""
  )
  target_link_libraries(example sample_target)

flatbuffers:
  example 1:
  vlink_generate_cpp (
    FBS
    sample.fbs
  )
  add_executable(example ${VLINK_GEN_HDRS} ${VLINK_GEN_SRCS})
  example 2:
  vlink_generate_cpp (
    TARGET sample_target
    FBS
    OUT_DIR "${CMAKE_BINARY_DIR}/out"
    sample.fbs
    FLAGS ""
  )
  target_link_libraries(example sample_target)

flatbuffers registry:
  example 1:
  vlink_generate_flatbuffers_registry_cpp(
    foo.fbs
    bar.fbs
  )
  # Requires the corresponding vlink_generate_cpp(FBS ...) step, which now
  # emits both foo.fbs.hpp and foo_bfbs.fbs.hpp (BFBS embedded helper header).
  target_sources(my_schema_plugin PRIVATE ${VLINK_GEN_REGISTRY_SRCS})

  example 2:
  vlink_generate_flatbuffers_registry_cpp(
    TARGET fb_registry
    OUT_DIR "${CMAKE_BINARY_DIR}/generated"
    OUTPUT  "${CMAKE_BINARY_DIR}/generated/fb_registry.cc"
    foo.fbs
    bar.fbs
  )
  target_link_libraries(my_schema_plugin PRIVATE fb_registry)
#]=======================================================================]

function(
  _vlink_generate_fastdds_cpp
  inputs
  in_dir
  out_dir
  flags
  out_hdrs
  out_srcs
)
  set(_TARGET_GEN_EXE fastddsgen)
  if(DEFINED ENV{VLINK_DDSGEN_PROGRAM})
    set(_TARGET_GEN_EXE $ENV{VLINK_DDSGEN_PROGRAM})
  elseif(DEFINED FASTDDS_GEN_EXECUTABLE)
    set(_TARGET_GEN_EXE ${FASTDDS_GEN_EXECUTABLE})
  elseif(DEFINED DDSGEN_PROGRAM)
    set(_TARGET_GEN_EXE ${DDSGEN_PROGRAM})
  elseif(WIN32)
    set(_TARGET_GEN_EXE fastddsgen.bat)
  endif()
  foreach(it ${inputs})
    get_filename_component(itdir ${it} DIRECTORY)
    get_filename_component(outfilename ${it} NAME_WE)
    if(in_dir)
      get_filename_component(filedir "${in_dir}" ABSOLUTE)
      file(RELATIVE_PATH dir_suffix "${filedir}" "${itdir}")
      set(dir_suffix /${dir_suffix})
    else()
      set(filedir ${itdir})
      set(dir_suffix)
    endif()
    if(TARGET fastdds)
      separate_arguments(
        command
        NATIVE_COMMAND
        "${_TARGET_GEN_EXE} -I ${filedir} -d ${out_dir}${dir_suffix} ${it} -flat-output-dir -t ${CMAKE_CURRENT_BINARY_DIR}/.fastddsgen -replace ${flags}"
      )
      set(gen_hdr
          "${out_dir}${dir_suffix}/${outfilename}.hpp;${out_dir}${dir_suffix}/${outfilename}PubSubTypes.hpp;${out_dir}${dir_suffix}/${outfilename}CdrAux.hpp;${out_dir}${dir_suffix}/${outfilename}CdrAux.ipp;${out_dir}${dir_suffix}/${outfilename}TypeObjectSupport.hpp"
      )
      set(gen_src
          "${out_dir}${dir_suffix}/${outfilename}.cxx;${out_dir}${dir_suffix}/${outfilename}PubSubTypes.cxx;${out_dir}${dir_suffix}/${outfilename}TypeObjectSupport.cxx"
      )
    else()
      separate_arguments(
        command
        NATIVE_COMMAND
        "${_TARGET_GEN_EXE} -I ${filedir} -d ${out_dir}${dir_suffix} ${it} -t ${CMAKE_CURRENT_BINARY_DIR}/.fastddsgen -replace ${flags}"
      )
      set(gen_hdr "${out_dir}${dir_suffix}/${outfilename}.h;${out_dir}${dir_suffix}/${outfilename}PubSubTypes.h")
      set(gen_src "${out_dir}${dir_suffix}/${outfilename}.cxx;${out_dir}${dir_suffix}/${outfilename}PubSubTypes.cxx")
    endif()
    add_custom_command(
      OUTPUT ${gen_hdr} ${gen_src}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}${dir_suffix}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/.fastddsgen
      COMMAND ${command}
      COMMAND ${CMAKE_COMMAND} -E touch ${gen_hdr} ${gen_src}
      DEPENDS ${it}
      VERBATIM
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    list(APPEND gen_hdrs ${gen_hdr})
    list(APPEND gen_srcs ${gen_src})
  endforeach()
  set(out_hdrs
      ${gen_hdrs}
      PARENT_SCOPE
  )
  set(out_srcs
      ${gen_srcs}
      PARENT_SCOPE
  )
endfunction()

function(
  _vlink_generate_protobuf_cpp
  inputs
  in_dir
  out_dir
  flags
  out_hdrs
  out_srcs
)
  if(NOT Protobuf_PROTOC_EXECUTABLE AND TARGET protobuf::protoc)
    get_target_property(Protobuf_PROTOC_EXECUTABLE protobuf::protoc IMPORTED_LOCATION)
    if(NOT EXISTS "${Protobuf_PROTOC_EXECUTABLE}")
      get_target_property(Protobuf_PROTOC_EXECUTABLE protobuf::protoc IMPORTED_LOCATION_RELEASE)
    endif()
    if(NOT EXISTS "${Protobuf_PROTOC_EXECUTABLE}")
      get_target_property(Protobuf_PROTOC_EXECUTABLE protobuf::protoc IMPORTED_LOCATION_RELWITHDEBINFO)
    endif()
    if(NOT EXISTS "${Protobuf_PROTOC_EXECUTABLE}")
      get_target_property(Protobuf_PROTOC_EXECUTABLE protobuf::protoc IMPORTED_LOCATION_MINSIZEREL)
    endif()
    if(NOT EXISTS "${Protobuf_PROTOC_EXECUTABLE}")
      get_target_property(Protobuf_PROTOC_EXECUTABLE protobuf::protoc IMPORTED_LOCATION_DEBUG)
    endif()
    if(NOT EXISTS "${Protobuf_PROTOC_EXECUTABLE}")
      get_target_property(Protobuf_PROTOC_EXECUTABLE protobuf::protoc IMPORTED_LOCATION_NOCONFIG)
    endif()
  endif()
  set(_TARGET_GEN_EXE protoc${CMAKE_EXECUTABLE_SUFFIX})
  if(DEFINED ENV{VLINK_PROTOC_PROGRAM})
    set(_TARGET_GEN_EXE $ENV{VLINK_PROTOC_PROGRAM})
  elseif(DEFINED VLINK_PROTOC_PROGRAM)
    set(_TARGET_GEN_EXE ${VLINK_PROTOC_PROGRAM})
  elseif(protobuf_PACKAGE_FOLDER_RELEASE)
    set(_TARGET_GEN_EXE ${protobuf_PACKAGE_FOLDER_RELEASE}/bin/protoc${CMAKE_EXECUTABLE_SUFFIX})
  elseif(protobuf_PACKAGE_FOLDER_DEBUG)
    set(_TARGET_GEN_EXE ${protobuf_PACKAGE_FOLDER_DEBUG}/bin/protoc${CMAKE_EXECUTABLE_SUFFIX})
  elseif(Protobuf_PROTOC_EXECUTABLE)
    set(_TARGET_GEN_EXE ${Protobuf_PROTOC_EXECUTABLE})
  endif()
  foreach(it ${inputs})
    get_filename_component(itdir ${it} DIRECTORY)
    get_filename_component(outfilename ${it} NAME_WE)
    if(in_dir)
      get_filename_component(filedir "${in_dir}" ABSOLUTE)
      file(RELATIVE_PATH dir_suffix "${filedir}" "${itdir}")
      set(dir_suffix /${dir_suffix})
    else()
      set(filedir ${itdir})
      set(dir_suffix)
    endif()
    separate_arguments(command NATIVE_COMMAND "${_TARGET_GEN_EXE} -I${filedir} --cpp_out=${out_dir} ${it} ${flags}")
    set(gen_hdr ${out_dir}${dir_suffix}/${outfilename}.pb.h)
    set(gen_src ${out_dir}${dir_suffix}/${outfilename}.pb.cc)
    add_custom_command(
      OUTPUT ${gen_hdr} ${gen_src}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}${dir_suffix}
      COMMAND ${command}
      COMMAND ${CMAKE_COMMAND} -E touch ${gen_hdr} ${gen_src}
      DEPENDS ${it}
      VERBATIM
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    list(APPEND gen_hdrs ${gen_hdr})
    list(APPEND gen_srcs ${gen_src})
  endforeach()
  set(out_hdrs
      ${gen_hdrs}
      PARENT_SCOPE
  )
  set(out_srcs
      ${gen_srcs}
      PARENT_SCOPE
  )
endfunction()

function(
  _vlink_generate_flatbuffers_cpp
  inputs
  in_dir
  out_dir
  flags
  out_hdrs
  out_srcs
)
  if(NOT Flatbuffers_FLATC_EXECUTABLE AND TARGET flatbuffers::flatc)
    get_target_property(Flatbuffers_FLATC_EXECUTABLE flatbuffers::flatc IMPORTED_LOCATION)
    if(NOT EXISTS "${Flatbuffers_FLATC_EXECUTABLE}")
      get_target_property(Flatbuffers_FLATC_EXECUTABLE flatbuffers::flatc IMPORTED_LOCATION_RELEASE)
    endif()
    if(NOT EXISTS "${Flatbuffers_FLATC_EXECUTABLE}")
      get_target_property(Flatbuffers_FLATC_EXECUTABLE flatbuffers::flatc IMPORTED_LOCATION_RELWITHDEBINFO)
    endif()
    if(NOT EXISTS "${Flatbuffers_FLATC_EXECUTABLE}")
      get_target_property(Flatbuffers_FLATC_EXECUTABLE flatbuffers::flatc IMPORTED_LOCATION_MINSIZEREL)
    endif()
    if(NOT EXISTS "${Flatbuffers_FLATC_EXECUTABLE}")
      get_target_property(Flatbuffers_FLATC_EXECUTABLE flatbuffers::flatc IMPORTED_LOCATION_DEBUG)
    endif()
    if(NOT EXISTS "${Flatbuffers_FLATC_EXECUTABLE}")
      get_target_property(Flatbuffers_FLATC_EXECUTABLE flatbuffers::flatc IMPORTED_LOCATION_NOCONFIG)
    endif()
  endif()
  set(_TARGET_GEN_EXE flatc${CMAKE_EXECUTABLE_SUFFIX})
  if(DEFINED ENV{VLINK_FLATC_PROGRAM})
    set(_TARGET_GEN_EXE $ENV{VLINK_FLATC_PROGRAM})
  elseif(DEFINED VLINK_FLATC_PROGRAM)
    set(_TARGET_GEN_EXE ${VLINK_FLATC_PROGRAM})
  elseif(flatbuffers_PACKAGE_FOLDER_RELEASE)
    set(_TARGET_GEN_EXE ${flatbuffers_PACKAGE_FOLDER_RELEASE}/bin/flatc${CMAKE_EXECUTABLE_SUFFIX})
  elseif(flatbuffers_PACKAGE_FOLDER_DEBUG)
    set(_TARGET_GEN_EXE ${flatbuffers_PACKAGE_FOLDER_DEBUG}/bin/flatc${CMAKE_EXECUTABLE_SUFFIX})
  elseif(Flatbuffers_FLATC_EXECUTABLE)
    set(_TARGET_GEN_EXE ${Flatbuffers_FLATC_EXECUTABLE})
  endif()
  foreach(it ${inputs})
    get_filename_component(itdir ${it} DIRECTORY)
    get_filename_component(outfilename ${it} NAME_WE)
    if(in_dir)
      get_filename_component(filedir "${in_dir}" ABSOLUTE)
      file(RELATIVE_PATH dir_suffix "${filedir}" "${itdir}")
      set(dir_suffix /${dir_suffix})
    else()
      set(filedir ${itdir})
      set(dir_suffix)
    endif()
    separate_arguments(
      command
      NATIVE_COMMAND
      "${_TARGET_GEN_EXE} -c ${flags} -I ${filedir} -o ${out_dir}${dir_suffix} ${it} --filename-suffix .fbs --filename-ext hpp --cpp-std=c++17 --gen-object-api --gen-name-strings --bfbs-gen-embed"
    )
    set(gen_hdr ${out_dir}${dir_suffix}/${outfilename}.fbs.hpp)
    set(gen_bfbs_hdr ${out_dir}${dir_suffix}/${outfilename}_bfbs.fbs.hpp)
    add_custom_command(
      OUTPUT ${gen_hdr} ${gen_bfbs_hdr}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}${dir_suffix}
      COMMAND ${command}
      COMMAND ${CMAKE_COMMAND} -E touch ${gen_hdr}
      COMMAND ${CMAKE_COMMAND} -E touch ${gen_bfbs_hdr}
      DEPENDS ${it}
      VERBATIM
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    list(APPEND gen_hdrs ${gen_hdr} ${gen_bfbs_hdr})
  endforeach()
  set(out_hdrs
      ${gen_hdrs}
      PARENT_SCOPE
  )
  set(out_srcs
      ""
      PARENT_SCOPE
  )
endfunction()

function(vlink_generate_cpp)
  set(options DDS PROTO FLAT FBS)
  set(one_value_args TARGET FLAGS IN_DIR OUT_DIR)
  set(multi_value_args)
  set(VLINK_GEN_HEADER_ONLY FALSE)
  cmake_parse_arguments(VLINK_GEN "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})
  if(NOT VLINK_GEN_IN_DIR)
    set(VLINK_GEN_IN_DIR)
  endif()
  if(NOT VLINK_GEN_OUT_DIR)
    set(VLINK_GEN_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR})
  endif()
  set(VLINK_GEN_INPUTS ${VLINK_GEN_UNPARSED_ARGUMENTS})
  if(NOT VLINK_GEN_INPUTS)
    message(FATAL_ERROR "Error: vlink_generate_cpp called without any input files")
  endif()
  foreach(it ${VLINK_GEN_INPUTS})
    get_filename_component(filepath ${it} ABSOLUTE)
    list(APPEND ABS_INPUTS ${filepath})
  endforeach()
  if(VLINK_GEN_DDS)
    if(NOT TARGET fastrtps)
      find_package(fastrtps CONFIG QUIET)
    endif()
    if(NOT TARGET fastdds)
      find_package(fastdds CONFIG QUIET)
    endif()
    if(NOT TARGET fastcdr)
      find_package(fastcdr CONFIG QUIET)
    endif()
    _vlink_generate_fastdds_cpp(
      "${ABS_INPUTS}" "${VLINK_GEN_IN_DIR}" "${VLINK_GEN_OUT_DIR}" "${VLINK_GEN_FLAGS}" out_hdrs out_srcs
    )
    if(TARGET fastdds)
      set(depend_libs fastcdr fastdds)
    else()
      set(depend_libs fastcdr fastrtps)
    endif()
  elseif(VLINK_GEN_PROTO)
    if(NOT TARGET protobuf::libprotobuf OR NOT TARGET protobuf::libprotobuf-lite)
      find_package(Protobuf CONFIG QUIET)
      if(NOT TARGET protobuf::libprotobuf OR NOT TARGET protobuf::libprotobuf-lite)
        find_package(Protobuf QUIET)
      endif()
    endif()
    _vlink_generate_protobuf_cpp(
      "${ABS_INPUTS}" "${VLINK_GEN_IN_DIR}" "${VLINK_GEN_OUT_DIR}" "${VLINK_GEN_FLAGS}" out_hdrs out_srcs
    )
    if(TARGET protobuf::libprotobuf)
      set(depend_libs protobuf::libprotobuf)
    elseif(TARGET protobuf::libprotobuf-lite)
      set(depend_libs protobuf::libprotobuf-lite)
    endif()
  elseif(VLINK_GEN_FLAT OR VLINK_GEN_FBS)
    set(VLINK_GEN_HEADER_ONLY TRUE)
    if(NOT TARGET flatbuffers::flatbuffers OR NOT TARGET flatbuffers::flatbuffers_shared)
      find_package(flatbuffers CONFIG QUIET)
    endif()
    _vlink_generate_flatbuffers_cpp(
      "${ABS_INPUTS}" "${VLINK_GEN_IN_DIR}" "${VLINK_GEN_OUT_DIR}" "${VLINK_GEN_FLAGS}" out_hdrs out_srcs
    )
    if(TARGET flatbuffers::flatbuffers_shared)
      set(depend_libs flatbuffers::flatbuffers_shared)
    elseif(TARGET flatbuffers::flatbuffers)
      set(depend_libs flatbuffers::flatbuffers)
    endif()
  else()
    message(FATAL_ERROR "Error: vlink_generate_cpp called without any types")
  endif()
  if("${out_hdrs}" STREQUAL "" AND "${out_srcs}" STREQUAL "")
    message(FATAL_ERROR "Error: vlink_generate_cpp called without any output files")
  endif()
  set(VLINK_GEN_HDRS
      ${out_hdrs}
      PARENT_SCOPE
  )
  set(VLINK_GEN_SRCS
      ${out_srcs}
      PARENT_SCOPE
  )
  if(VLINK_GEN_TARGET)
    if(VLINK_GEN_HEADER_ONLY)
      if(${CMAKE_VERSION} VERSION_LESS "3.20.0")
        add_library(${VLINK_GEN_TARGET} INTERFACE)
        target_include_directories(${VLINK_GEN_TARGET} INTERFACE ${VLINK_GEN_OUT_DIR})
        set_source_files_properties(${out_hdrs} PROPERTIES SKIP_LINTING ON)
        add_custom_target(${VLINK_GEN_TARGET}_outgen DEPENDS ${out_hdrs})
        add_dependencies(${VLINK_GEN_TARGET} ${VLINK_GEN_TARGET}_outgen)
      else()
        add_library(${VLINK_GEN_TARGET} INTERFACE ${out_hdrs})
        target_include_directories(${VLINK_GEN_TARGET} INTERFACE ${VLINK_GEN_OUT_DIR})
        set_source_files_properties(${out_hdrs} PROPERTIES SKIP_LINTING ON)
        set_target_properties(${VLINK_GEN_TARGET} PROPERTIES POSITION_INDEPENDENT_CODE ON)
        set_target_properties(${VLINK_GEN_TARGET} PROPERTIES CXX_CLANG_TIDY "")
      endif()
      target_sources(${VLINK_GEN_TARGET} INTERFACE ${ABS_INPUTS})
      if(depend_libs)
        target_link_libraries(${VLINK_GEN_TARGET} INTERFACE ${depend_libs})
      endif()
    else()
      add_library(${VLINK_GEN_TARGET} OBJECT ${out_hdrs} ${out_srcs})
      target_include_directories(${VLINK_GEN_TARGET} PUBLIC ${VLINK_GEN_OUT_DIR})
      set_source_files_properties(${out_hdrs} ${out_srcs} PROPERTIES SKIP_LINTING ON)
      set_target_properties(${VLINK_GEN_TARGET} PROPERTIES POSITION_INDEPENDENT_CODE ON)
      set_target_properties(${VLINK_GEN_TARGET} PROPERTIES CXX_CLANG_TIDY "")
      target_sources(${VLINK_GEN_TARGET} INTERFACE ${ABS_INPUTS})
      if(depend_libs)
        target_link_libraries(${VLINK_GEN_TARGET} PUBLIC ${depend_libs})
      endif()
    endif()
  endif()
endfunction()

function(vlink_generate_flatbuffers_registry_cpp)
  set(options)
  set(one_value_args TARGET IN_DIR OUT_DIR OUTPUT)
  set(multi_value_args)
  set(VLINK_GEN_HEADER_ONLY TRUE)
  cmake_parse_arguments(VLINK_GEN "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})
  if(NOT VLINK_GEN_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "Error: vlink_generate_flatbuffers_registry_cpp called without any input files")
  endif()
  if(NOT VLINK_GEN_OUTPUT)
    set(VLINK_GEN_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vlink_flatbuffers_registry.cc")
  endif()
  if(NOT VLINK_GEN_OUT_DIR)
    set(VLINK_GEN_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
  endif()
  file(MAKE_DIRECTORY "${VLINK_GEN_OUT_DIR}")
  set(inc_lines "")
  set(reg_lines "")
  foreach(it ${VLINK_GEN_UNPARSED_ARGUMENTS})
    get_filename_component(abs_fbs ${it} ABSOLUTE)
    get_filename_component(abs_fbs_dir ${abs_fbs} DIRECTORY)
    get_filename_component(base ${abs_fbs} NAME_WE)
    if(VLINK_GEN_IN_DIR)
      get_filename_component(filedir "${VLINK_GEN_IN_DIR}" ABSOLUTE)
      file(RELATIVE_PATH dir_suffix "${filedir}" "${abs_fbs_dir}")
      set(dir_suffix "/${dir_suffix}")
      if(dir_suffix STREQUAL "/.")
        set(dir_suffix "")
      endif()
      set(include_dir_suffix "${dir_suffix}")
      if(include_dir_suffix)
        string(REGEX REPLACE "^/" "" include_dir_suffix "${include_dir_suffix}")
      endif()
    else()
      set(dir_suffix "")
      set(include_dir_suffix "")
    endif()
    file(READ "${abs_fbs}" _content)
    string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" "" _content "${_content}")
    string(REGEX REPLACE "//[^\n]*" "" _content "${_content}")
    string(REGEX MATCH "root_type[ \t\r\n]+([A-Za-z_][A-Za-z0-9_]*)[ \t\r\n]*;" _root_match "${_content}")
    if(NOT _root_match)
      continue()
    endif()
    set(_root "${CMAKE_MATCH_1}")
    string(FIND "${_content}" "${_root_match}" _root_pos)
    if(_root_pos LESS 0)
      set(_prefix "${_content}")
    else()
      string(SUBSTRING "${_content}" 0 ${_root_pos} _prefix)
    endif()
    set(_prefix_guarded "\n${_prefix}")
    string(REGEX MATCHALL "[^A-Za-z0-9_]namespace[ \t\r\n]+[A-Za-z_][A-Za-z0-9_.]*" _ns_stmts "${_prefix_guarded}")
    set(_namespace "")
    list(LENGTH _ns_stmts _ns_count)
    if(_ns_count GREATER 0)
      math(EXPR _last_idx "${_ns_count} - 1")
      list(GET _ns_stmts ${_last_idx} _last_stmt)
      string(REGEX MATCH "namespace[ \t\r\n]+([A-Za-z_][A-Za-z0-9_.]*)" _discard "${_last_stmt}")
      set(_namespace "${CMAKE_MATCH_1}")
    endif()
    if(_namespace)
      string(REPLACE "." "::" cppns "${_namespace}")
      set(_cpp_type "${cppns}::${_root}BinarySchema")
      set(_flat_name "${_namespace}.${_root}")
    else()
      set(_cpp_type "::${_root}BinarySchema")
      set(_flat_name "${_root}")
    endif()
    if(include_dir_suffix)
      set(_include_path "${include_dir_suffix}/${base}_bfbs.fbs.hpp")
    else()
      set(_include_path "${base}_bfbs.fbs.hpp")
    endif()
    string(APPEND inc_lines "#include \"${_include_path}\"\n")
    string(APPEND reg_lines "VLINK_REGISTER_FLATBUFFERS(\"${_flat_name}\", ${_cpp_type});\n")
  endforeach()
  file(
    WRITE "${VLINK_GEN_OUTPUT}"
    "// This file is auto-generated by vlink_generate_flatbuffers_registry_cpp.\n\n#include <vlink/extension/flatbuffers_registry.h>\n"
  )
  file(APPEND "${VLINK_GEN_OUTPUT}" "${inc_lines}\n")
  file(APPEND "${VLINK_GEN_OUTPUT}" "${reg_lines}\n")
  set(VLINK_GEN_REGISTRY_SRCS
      ${VLINK_GEN_OUTPUT}
      PARENT_SCOPE
  )
  if(NOT TARGET flatbuffers::flatbuffers OR NOT TARGET flatbuffers::flatbuffers_shared)
    find_package(flatbuffers CONFIG QUIET)
  endif()
  if(TARGET flatbuffers::flatbuffers_shared)
    set(depend_libs flatbuffers::flatbuffers_shared)
  elseif(TARGET flatbuffers::flatbuffers)
    set(depend_libs flatbuffers::flatbuffers)
  endif()
  if(VLINK_GEN_TARGET)
    if(VLINK_GEN_HEADER_ONLY)
      if(${CMAKE_VERSION} VERSION_LESS "3.20.0")
        add_library(${VLINK_GEN_TARGET} INTERFACE)
        target_include_directories(${VLINK_GEN_TARGET} INTERFACE ${VLINK_GEN_OUT_DIR})
        set_source_files_properties(${out_hdrs} PROPERTIES SKIP_LINTING ON)
        add_custom_target(${VLINK_GEN_TARGET}_outgen DEPENDS ${out_hdrs})
        add_dependencies(${VLINK_GEN_TARGET} ${VLINK_GEN_TARGET}_outgen)
      else()
        add_library(${VLINK_GEN_TARGET} INTERFACE ${out_hdrs})
        target_include_directories(${VLINK_GEN_TARGET} INTERFACE ${VLINK_GEN_OUT_DIR})
        set_source_files_properties(${out_hdrs} PROPERTIES SKIP_LINTING ON)
        set_target_properties(${VLINK_GEN_TARGET} PROPERTIES POSITION_INDEPENDENT_CODE ON)
        set_target_properties(${VLINK_GEN_TARGET} PROPERTIES CXX_CLANG_TIDY "")
      endif()
      target_sources(${VLINK_GEN_TARGET} INTERFACE ${ABS_INPUTS})
      if(depend_libs)
        target_link_libraries(${VLINK_GEN_TARGET} INTERFACE ${depend_libs})
      endif()
    else()
      add_library(${VLINK_GEN_TARGET} OBJECT ${out_hdrs} ${out_srcs})
      target_include_directories(${VLINK_GEN_TARGET} PUBLIC ${VLINK_GEN_OUT_DIR})
      set_source_files_properties(${out_hdrs} ${out_srcs} PROPERTIES SKIP_LINTING ON)
      set_target_properties(${VLINK_GEN_TARGET} PROPERTIES POSITION_INDEPENDENT_CODE ON)
      set_target_properties(${VLINK_GEN_TARGET} PROPERTIES CXX_CLANG_TIDY "")
      target_sources(${VLINK_GEN_TARGET} INTERFACE ${ABS_INPUTS})
      if(depend_libs)
        target_link_libraries(${VLINK_GEN_TARGET} PUBLIC ${depend_libs})
      endif()
    endif()
  endif()
endfunction()
