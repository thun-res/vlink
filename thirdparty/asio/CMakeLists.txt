project(asio VERSION 1.28.2)

add_library(${PROJECT_NAME} INTERFACE)
add_library("thirdparty::${PROJECT_NAME}" ALIAS ${PROJECT_NAME})
target_include_directories(${PROJECT_NAME} INTERFACE ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(${PROJECT_NAME} INTERFACE "ASIO_STANDALONE")
set(Asio_INCLUDE_DIR ${PROJECT_SOURCE_DIR}/include PARENT_SCOPE)
