project(sample_pong LANGUAGES CXX)

# src
add_executable(${PROJECT_NAME} ../ping_pong_common.h pong.cc)
target_link_libraries(${PROJECT_NAME} PRIVATE vlink::all)
target_include_directories(${PROJECT_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/..)

# install
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
