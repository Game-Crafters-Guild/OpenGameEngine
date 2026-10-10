# Types module tests (FlatMap, StringId, etc.)

find_package(GTest QUIET)
if(NOT GTest_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GTEST gtest_main)
    endif()
    if(NOT GTEST_FOUND)
        message(STATUS "GTest not found, skipping Types tests")
        return()
    endif()
endif()

add_executable(TypesTests
    ColorTests.cpp
    FalseSharingTests.cpp
    FlatMapTests.cpp
    FormatNumberTests.cpp
    Fnv1aTests.cpp
    NearestNameTests.cpp
    ParseStrictDecimalTests.cpp
    PathUtilsTests.cpp
    StringUtilsTests.cpp
)

target_link_libraries(TypesTests
    PRIVATE
        Types
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(TypesTests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

ge_set_output_tests(TypesTests)
ge_stage_runtime_dependencies(TypesTests)

add_test(NAME TypesTests COMMAND TypesTests)
set_tests_properties(TypesTests PROPERTIES
    TIMEOUT 30
    LABELS "Types;Unit"
)
