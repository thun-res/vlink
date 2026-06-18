project(spdlog VERSION 1.17.0)

add_library(${PROJECT_NAME} INTERFACE)
add_library("thirdparty::${PROJECT_NAME}" ALIAS ${PROJECT_NAME})
target_include_directories(${PROJECT_NAME} INTERFACE ${PROJECT_SOURCE_DIR}/include)
if(MSVC)
  target_compile_options(${PROJECT_NAME} INTERFACE /Zc:__cplusplus /MP /utf-8)
endif()
