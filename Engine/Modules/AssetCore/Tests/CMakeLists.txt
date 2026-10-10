# AssetCore module tests: GUID::Derive golden/stability vectors and
# GUID::Generate layout and concurrent uniqueness.

find_package(GTest QUIET)
if(NOT GTest_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GTEST gtest_main)
    endif()
    if(NOT GTEST_FOUND)
        message(STATUS "GTest not found, skipping AssetCore tests")
        return()
    endif()
endif()

add_executable(GUIDTests
    GUIDDeriveTests.cpp
    GUIDGenerateTests.cpp
)

# AssetCore is an OBJECT library; linking it directly splices GUID.cpp's
# object into the test exe (same pattern as TypesTests linking Types).
target_link_libraries(GUIDTests
    PRIVATE
        AssetCore
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(GUIDTests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

ge_set_output_tests(GUIDTests)
ge_stage_runtime_dependencies(GUIDTests)

add_test(NAME GUIDTests COMMAND GUIDTests)
set_tests_properties(GUIDTests PROPERTIES
    TIMEOUT 30
    LABELS "AssetCore;Unit"
)
