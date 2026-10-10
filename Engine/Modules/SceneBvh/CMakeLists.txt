cmake_minimum_required(VERSION 3.20)

project(SceneBvh LANGUAGES CXX)

add_library(SceneBvh OBJECT
    Source/BinnedSahSplit.cpp
    Source/ThreadedBvhBuilder.cpp
    Source/ThreadedBvhRefit.cpp
    Source/TlasPacker.cpp
    Source/ThreadedBvhPool.cpp
)

target_compile_features(SceneBvh PUBLIC cxx_std_20)

target_include_directories(SceneBvh
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

target_link_libraries(SceneBvh
    PUBLIC
        Types
        Mathematics
    PRIVATE
        JobSystem
        Logger
)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_executable(SceneBvhTests
        Tests/SceneBvhTests.cpp
    )
    target_link_libraries(SceneBvhTests PRIVATE
        SceneBvh
        JobSystem
        Logger
        GTest::gtest
        GTest::gtest_main
    )
    ge_set_output_tests(SceneBvhTests)
    ge_stage_runtime_dependencies(SceneBvhTests)
    add_test(NAME SceneBvhTests COMMAND SceneBvhTests)
endif()
