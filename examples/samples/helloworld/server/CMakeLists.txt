project(sample_helloworld_server LANGUAGES CXX)

# src
add_executable(${PROJECT_NAME} ../helloworld_common.h helloworld_server.cc)
target_link_libraries(${PROJECT_NAME} PRIVATE vlink::all helloworld_gen)
target_include_directories(${PROJECT_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/..)

# install
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
