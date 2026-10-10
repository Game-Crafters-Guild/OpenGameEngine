cmake_minimum_required(VERSION 3.20)
project(Input LANGUAGES CXX)

# Public headers and sources
file(GLOB_RECURSE INPUT_PUBLIC_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/Include/Input/*.h)
set(INPUT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/InputSystem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/GamepadPolling.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/GamepadEdgeTracker.cpp
)

add_library(Input OBJECT ${INPUT_PUBLIC_HEADERS} ${INPUT_SOURCES})

# Public includes
target_include_directories(Input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Include)

# Link to core engine modules used by InputSystem
# Types: basic math/types; Mathematics: Vector2 in the public API;
# Platform: window/input events
if(TARGET Types)
    target_link_libraries(Input PUBLIC Types)
endif()
if(TARGET Mathematics)
    target_link_libraries(Input PUBLIC Mathematics)
endif()
if(TARGET Platform)
    target_link_libraries(Input PUBLIC Platform)
endif()

set_target_properties(Input PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    FOLDER "Input"
)

# Unit tests for Input module (optional)
if(BUILD_TESTING)
    find_package(GTest REQUIRED)

    add_executable(InputSystemTests
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/InputSystemTests.cpp
    )

    # Modules are OBJECT libs aggregated into Engine.dll; link Engine to
    # resolve module symbols via the import lib. See GenerationalVector/Tests
    # for the same fix pattern.
    target_link_libraries(InputSystemTests PRIVATE
        Engine
        GTest::gtest
        GTest::gtest_main
    )

    set_target_properties(InputSystemTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Input/Tests"
    )

    ge_set_output_tests(InputSystemTests)
    ge_stage_runtime_dependencies(InputSystemTests)

    add_test(NAME InputSystemTests COMMAND InputSystemTests)
endif()

