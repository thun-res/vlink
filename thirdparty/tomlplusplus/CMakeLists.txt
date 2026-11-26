project(tomlplusplus VERSION 3.4.0)

add_library(${PROJECT_NAME} INTERFACE)
add_library("thirdparty::${PROJECT_NAME}" ALIAS ${PROJECT_NAME})
target_include_directories(${PROJECT_NAME} SYSTEM INTERFACE ${PROJECT_SOURCE_DIR}/include)
