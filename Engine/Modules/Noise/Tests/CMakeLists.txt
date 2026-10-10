find_package(GTest QUIET)
if(NOT GTest_FOUND)
    message(STATUS "GTest not found, skipping Noise tests")
    return()
endif()

add_executable(NoiseTests
    GradientCurl3DTests.cpp
    NoiseGoldenTests.cpp
)

target_link_libraries(NoiseTests
    PRIVATE
        Noise
        GTest::gtest
        GTest::gtest_main
)

set_target_properties(NoiseTests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    FOLDER "Noise/Tests"
)

ge_set_output_tests(NoiseTests)
ge_stage_runtime_dependencies(NoiseTests)

add_test(NAME NoiseTests COMMAND NoiseTests)
set_tests_properties(NoiseTests PROPERTIES
    TIMEOUT 30
    LABELS "Noise;Unit"
)
