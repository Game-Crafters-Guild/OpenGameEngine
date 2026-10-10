cmake_minimum_required(VERSION 3.20)

project(Spline LANGUAGES CXX)

add_library(Spline OBJECT
    Source/SplineData.cpp
    Source/SplineEvaluator.cpp
)

target_compile_features(Spline PUBLIC cxx_std_20)

target_include_directories(Spline
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

target_link_libraries(Spline
    PUBLIC
        Types
        Mathematics
)

# Self-find GTest (UI/Input pattern): gating on root-scope GTest state instead
# would silently drop the test target whenever the root skips its find.
if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_executable(SplineTests
        Tests/SplineEffectiveClosedTests.cpp
        Tests/SplineEvaluatorTests.cpp
        Tests/SplinePointRemovalTests.cpp
        Tests/SplineResampleTests.cpp
    )
    target_link_libraries(SplineTests PRIVATE Spline GTest::gtest GTest::gtest_main)
    set_target_properties(SplineTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Engine/Tests/Spline"
    )
    ge_set_output_tests(SplineTests)
    ge_stage_runtime_dependencies(SplineTests)
    add_test(NAME SplineTests COMMAND SplineTests)
endif()
