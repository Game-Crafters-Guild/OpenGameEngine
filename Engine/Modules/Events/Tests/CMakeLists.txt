# Events module tests

find_package(GTest QUIET)
if(NOT GTest_FOUND)
    message(STATUS "GTest not found, skipping Events tests")
    return()
endif()

add_executable(EventTests
    EventTests.cpp
)

target_link_libraries(EventTests
    PRIVATE
        Events
        GTest::gtest
        GTest::gtest_main
)

ge_set_output_tests(EventTests)
ge_stage_runtime_dependencies(EventTests)

add_test(NAME EventTests COMMAND EventTests)

set_tests_properties(EventTests PROPERTIES
    TIMEOUT 60
    LABELS "Events;Unit")
