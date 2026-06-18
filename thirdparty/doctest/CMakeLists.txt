project(doctest VERSION 2.4.12)

add_library(${PROJECT_NAME} INTERFACE)
add_library("thirdparty::${PROJECT_NAME}" ALIAS ${PROJECT_NAME})
target_include_directories(${PROJECT_NAME} INTERFACE ${PROJECT_SOURCE_DIR}/include)
