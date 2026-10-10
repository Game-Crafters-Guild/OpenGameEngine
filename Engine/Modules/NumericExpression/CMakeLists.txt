cmake_minimum_required(VERSION 3.20)
project(NumericExpression LANGUAGES C CXX)

set(NUMERIC_EXPRESSION_SOURCES
    Source/NumericExpression.cpp
)

add_library(NumericExpression OBJECT ${NUMERIC_EXPRESSION_SOURCES})

find_package(unofficial-tinyexpr CONFIG REQUIRED)

target_include_directories(NumericExpression
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
)

target_link_libraries(NumericExpression
    PRIVATE
        unofficial::tinyexpr::tinyexpr
)

set_target_properties(NumericExpression PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    FOLDER "NumericExpression"
)

if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Tests/CMakeLists.txt")
    add_subdirectory(Tests)
endif()
