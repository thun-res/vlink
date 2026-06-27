cmake_minimum_required(VERSION 3.16)

project(qcustomplot LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)

set(QT_MAJOR Qt${QT_VERSION_MAJOR})

find_package(${QT_MAJOR} REQUIRED COMPONENTS Widgets)

set(QCUSTOMPLOT_SRC
    qcustomplot.cpp
)

set(QCUSTOMPLOT_MOC_HDR
    qcustomplot.h
)

add_library(${PROJECT_NAME} STATIC ${QCUSTOMPLOT_SRC} ${QCUSTOMPLOT_MOC_HDR} ${QCUSTOMPLOT_MOC})

target_include_directories(${PROJECT_NAME} PUBLIC ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(${PROJECT_NAME} PUBLIC ${QT_MAJOR}::Widgets)
target_compile_definitions(${PROJECT_NAME} PRIVATE "QT_STATIC_BUILD" "QT_NO_PRINTER")

add_library(QCustomPlot::QCustomPlot ALIAS ${PROJECT_NAME})
