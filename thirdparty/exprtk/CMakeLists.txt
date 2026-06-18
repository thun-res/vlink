project(exprtk VERSION 0.0.3)

add_library(${PROJECT_NAME} INTERFACE)
add_library("thirdparty::${PROJECT_NAME}" ALIAS ${PROJECT_NAME})
target_include_directories(${PROJECT_NAME} INTERFACE ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(
  ${PROJECT_NAME}
  INTERFACE "exprtk_disable_comments"
            "exprtk_disable_rtl_io"
            "exprtk_disable_rtl_io_file"
            "exprtk_disable_rtl_vecops"
            "exprtk_disable_string_capabilities"
            "exprtk_disable_break_continue"
            "exprtk_disable_return_statement"
            "exprtk_disable_caseinsensitivity"
            "exprtk_disable_superscalar_unroll")
