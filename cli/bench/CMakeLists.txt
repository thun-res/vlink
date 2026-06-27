#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

project(vlink-bench LANGUAGES CXX)

# src
add_executable(
  ${PROJECT_NAME}
  report.h
  report_helpers.h
  report_internal.h
  report_score.h
  report_html_css.h
  report_html_i18n.h
  report_html_en_us.h
  report_html_zh_cn.h
  bench.h
  main.cc
  bench.cc
  report_helpers.cc
  report_internal.cc
  report.cc
  report_html.cc
)
target_link_libraries(${PROJECT_NAME} PRIVATE vlink::vlink ${VLINK_LIBRARIES})
target_link_libraries(${PROJECT_NAME} PRIVATE thirdparty::argparse thirdparty::json)

if(WIN32)
  target_link_libraries(${PROJECT_NAME} PRIVATE Psapi)
  if(EXISTS ${CMAKE_SOURCE_DIR}/packup/ico/vlink-cmd.rc)
    target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/packup/ico/vlink-cmd.rc)
  endif()
endif()

# install
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# symlinks
if(ENABLE_SYMLINKS)
  vlink_install_symlink(bench SYMLINK ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# completions
if(ENABLE_COMPLETIONS)
  if(ENABLE_SYMLINKS)
    vlink_install_completions(${PROJECT_NAME} ALIAS bench)
  else()
    vlink_install_completions(${PROJECT_NAME})
  endif()
endif()
