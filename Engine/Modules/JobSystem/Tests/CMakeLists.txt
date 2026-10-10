# JobSystem Module Tests (parallel algorithms, etc.)

add_test(NAME JobSystemDependencyIncludes
    COMMAND "${CMAKE_COMMAND}"
        "-DJOBSYSTEM_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/.."
        "-DTEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/DependencyIncludes"
        "-DTEST_GENERATOR=${CMAKE_GENERATOR}"
        "-DTEST_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        "-DTEST_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
        "-DTEST_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DTEST_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/DependencyIncludes.cmake"
)

find_package(GTest QUIET)
if(NOT GTest_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GTEST gtest_main)
    endif()
    if(NOT GTEST_FOUND)
        message(STATUS "GTest not found, skipping JobSystem module tests")
        return()
    endif()
endif()

add_executable(ParallelAlgorithmsTests
    ParallelAlgorithmsTests.cpp
)

target_link_libraries(ParallelAlgorithmsTests
    PRIVATE
        JobSystem
        Logger
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(ParallelAlgorithmsTests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

ge_set_output_tests(ParallelAlgorithmsTests)
ge_stage_runtime_dependencies(ParallelAlgorithmsTests)

add_test(NAME ParallelAlgorithmsTests COMMAND ParallelAlgorithmsTests)

set_tests_properties(ParallelAlgorithmsTests PROPERTIES
    TIMEOUT 30
    LABELS "JobSystem;Unit"
)

add_executable(InlineModeTrampolineTests
    InlineModeTrampolineTests.cpp
)

target_link_libraries(InlineModeTrampolineTests
    PRIVATE
        JobSystem
        Logger
        GTest::gtest
        GTest::gtest_main
)

ge_set_output_tests(InlineModeTrampolineTests)
ge_stage_runtime_dependencies(InlineModeTrampolineTests)

add_test(NAME InlineModeTrampolineTests COMMAND InlineModeTrampolineTests)

set_tests_properties(InlineModeTrampolineTests PROPERTIES
    TIMEOUT 60
    LABELS "JobSystem;Unit"
)
