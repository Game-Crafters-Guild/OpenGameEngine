# AssetCore Architecture Tests
cmake_minimum_required(VERSION 3.20)

# Find required packages
find_package(GTest REQUIRED)

# Create AssetCore test executable
add_executable(AssetCoreTests
    AssetCoreArchitectureTest.cpp
    AssetIgnoreRulesTests.cpp
    AssetPostLoadHookTests.cpp
    AssetReloadFileGuardTests.cpp
    AssetTypesExtensionSetTests.cpp
    ResultTests.cpp
)

# Link against required libraries
target_link_libraries(AssetCoreTests
    AssetCore
    GTest::GTest
    GTest::Main
)

# Include directories
target_include_directories(AssetCoreTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Tests
)

# Set C++ standard
set_target_properties(AssetCoreTests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

# Add to test suite
ge_set_output_tests(AssetCoreTests)
ge_stage_runtime_dependencies(AssetCoreTests)
add_test(NAME AssetCoreArchitectureTests COMMAND $<TARGET_FILE:AssetCoreTests>)

# Platform-specific settings
if(WIN32)
    # Windows-specific settings
    target_compile_definitions(AssetCoreTests PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# Add custom target for running AssetCore tests
add_custom_target(run_assetcore_tests
    COMMAND $<TARGET_FILE:AssetCoreTests>
    DEPENDS AssetCoreTests
    WORKING_DIRECTORY $<TARGET_FILE_DIR:AssetCoreTests>
    COMMENT "Running AssetCore architecture tests"
)
