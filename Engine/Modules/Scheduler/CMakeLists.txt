cmake_minimum_required(VERSION 3.20)

project(Scheduler LANGUAGES CXX)

add_library(Scheduler OBJECT
    Source/Scheduler.cpp
)

target_include_directories(Scheduler
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
)

target_compile_features(Scheduler PUBLIC cxx_std_17)

# Tests
if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_executable(SchedulerTests
        Tests/SchedulerTests.cpp
    )
    target_link_libraries(SchedulerTests
        PRIVATE
            Scheduler
            GTest::gtest
            GTest::gtest_main
    )
    ge_set_output_tests(SchedulerTests)
    ge_stage_runtime_dependencies(SchedulerTests)
    add_test(NAME SchedulerTests COMMAND SchedulerTests)
endif()

