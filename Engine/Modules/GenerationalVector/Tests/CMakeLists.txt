# GenerationalVector Tests

# Find or build gtest
find_package(GTest QUIET)
if(NOT GTest_FOUND)
    # Try to find gtest via vcpkg or system
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GTEST gtest_main)
    endif()
    
    if(NOT GTEST_FOUND)
        message(STATUS "GTest not found, skipping GenerationalVector tests")
        return()
    endif()
endif()

# Test executable
add_executable(GenerationalVectorTests
    BasicTests.cpp
    LifetimeTests.cpp
    PerformanceTests.cpp
    TestMain.cpp
)

# Link libraries. Modules are OBJECT libs aggregated into Engine.dll; tests
# that need module symbols link Engine (the SHARED aggregator) so the import
# lib resolves them, rather than re-splicing .obj files into the test exe.
# OBJECT-library transitive propagation through CMake's link graph doesn't
# reliably reach executables on the VS generator.
target_link_libraries(GenerationalVectorTests
    PRIVATE
        Engine
        GTest::gtest
        GTest::gtest_main
)

# Include directories
target_include_directories(GenerationalVectorTests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

ge_set_output_tests(GenerationalVectorTests)
ge_stage_runtime_dependencies(GenerationalVectorTests)

# Add to test suite
add_test(NAME GenerationalVectorTests COMMAND GenerationalVectorTests)

# Set test properties
set_tests_properties(GenerationalVectorTests PROPERTIES
    TIMEOUT 30
    LABELS "GenerationalVector;Unit"
)
