cmake_minimum_required(VERSION 3.15...3.31)

if(TARGET doc)
  project(vlink-doc)
else()
  project(doc)
endif()

find_package(Doxygen QUIET COMPONENTS dot)
if(NOT Doxygen_FOUND)
  message(STATUS "No Doxygen found. Documentation won't be built")
  if(ENABLE_DOC)
    set(ENABLE_DOC
        OFF
        PARENT_SCOPE
    )
  endif()
  return()
endif()

if(NOT TARGET Doxygen::dot)
  message(STATUS "No Graphviz found. Documentation won't be built")
  if(ENABLE_DOC)
    set(ENABLE_DOC
        OFF
        PARENT_SCOPE
    )
  endif()
  return()
endif()

# config
if(DEFINED DOC_PROJECT_LOGO)
  set(DOC_LOGO ${DOC_PROJECT_LOGO})
endif()
if(DEFINED DOC_PROJECT_NAME)
  set(DOC_NAME ${DOC_PROJECT_NAME})
else()
  set(DOC_NAME ${CMAKE_PROJECT_NAME})
endif()
set(DOC_VERSION ${CMAKE_PROJECT_VERSION})
set(DOC_BRIEF "${CMAKE_PROJECT_DESCRIPTION}")
set(DOC_OUTPUT_DIR ${CMAKE_BINARY_DIR}/output/doc)

# en_us
if(EXISTS ${PROJECT_SOURCE_DIR}/Doxyfile.en_us.in)
  message(STATUS "Generate en_us doxygen.")
  configure_file(${PROJECT_SOURCE_DIR}/Doxyfile.en_us.in ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile.en_us @ONLY)
  add_custom_command(
    OUTPUT ${PROJECT_NAME}_doc-en_us
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DOC_OUTPUT_DIR}/en_us
    COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile.en_us
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile.en_us
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  )
  list(APPEND _doc_targets ${PROJECT_NAME}_doc-en_us)
endif()

# zh_cn
if(EXISTS ${PROJECT_SOURCE_DIR}/Doxyfile.zh_cn.in)
  message(STATUS "Generate zh_cn doxygen.")
  configure_file(${PROJECT_SOURCE_DIR}/Doxyfile.zh_cn.in ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile.zh_cn @ONLY)
  add_custom_command(
    OUTPUT ${PROJECT_NAME}_doc-zh_cn
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DOC_OUTPUT_DIR}/zh_cn
    COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile.zh_cn
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile.zh_cn
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  )
  list(APPEND _doc_targets ${PROJECT_NAME}_doc-zh_cn)
endif()

# install
add_custom_target(${PROJECT_NAME} ALL DEPENDS ${_doc_targets})
set_property(
  TARGET ${PROJECT_NAME}
  APPEND
  PROPERTY ADDITIONAL_CLEAN_FILES ${DOC_OUTPUT_DIR}
)
install(DIRECTORY ${DOC_OUTPUT_DIR} DESTINATION ${CMAKE_INSTALL_DATADIR}/${CMAKE_PROJECT_NAME})
