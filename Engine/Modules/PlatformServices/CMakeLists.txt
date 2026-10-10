cmake_minimum_required(VERSION 3.20)
project(PlatformServices LANGUAGES CXX)

file(GLOB_RECURSE PLATFORMSERVICES_PUBLIC_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/Include/PlatformServices/*.h)
set(PLATFORMSERVICES_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/PlatformServices.cpp
)

add_library(PlatformServices OBJECT ${PLATFORMSERVICES_PUBLIC_HEADERS} ${PLATFORMSERVICES_SOURCES})

target_include_directories(PlatformServices PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Include)

set_target_properties(PlatformServices PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    FOLDER "PlatformServices"
)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)

    add_executable(PlatformServicesTests
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/PlatformServicesTests.cpp
    )

    # Modules are OBJECT libs aggregated into Engine.dll; link Engine to
    # resolve module symbols via the import lib (same pattern as Input).
    target_link_libraries(PlatformServicesTests PRIVATE
        Engine
        GTest::gtest
        GTest::gtest_main
    )

    set_target_properties(PlatformServicesTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "PlatformServices/Tests"
    )

    ge_set_output_tests(PlatformServicesTests)
    ge_stage_runtime_dependencies(PlatformServicesTests)

    add_test(NAME PlatformServicesTests COMMAND PlatformServicesTests)
endif()
