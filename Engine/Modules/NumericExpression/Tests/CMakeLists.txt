find_package(GTest QUIET)
if(NOT GTest_FOUND)
    message(STATUS "GTest not found, skipping NumericExpression tests")
    return()
endif()

add_executable(NumericExpressionTests
    NumericExpressionTests.cpp
)

target_link_libraries(NumericExpressionTests
    PRIVATE
        NumericExpression
        GTest::gtest
        GTest::gtest_main
)

set_target_properties(NumericExpressionTests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    FOLDER "NumericExpression/Tests"
)

ge_set_output_tests(NumericExpressionTests)
ge_stage_runtime_dependencies(NumericExpressionTests)

add_test(NAME NumericExpressionTests COMMAND NumericExpressionTests)
set_tests_properties(NumericExpressionTests PROPERTIES
    TIMEOUT 30
    LABELS "NumericExpression;Unit"
)
