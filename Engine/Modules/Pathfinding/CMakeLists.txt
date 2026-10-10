cmake_minimum_required(VERSION 3.20)

project(Pathfinding LANGUAGES CXX)

add_library(Pathfinding OBJECT
    Source/PathBuffer.cpp
    Source/GridMap.cpp
    Source/AStarSolver.cpp
    Source/HexGridHelpers.cpp
    Source/DetourNavMap.cpp
    Source/ObstacleAvoidance.cpp
    Source/NavigationWorld.cpp
    Source/PathSmoother.cpp
    Source/NavCacheManager.cpp
)

target_compile_features(Pathfinding PUBLIC cxx_std_20)

target_include_directories(Pathfinding
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

# Core dependencies
target_link_libraries(Pathfinding
    PUBLIC
        Types
        Mathematics
        Logger
        JobSystem
        AssetCore
    PRIVATE
        FileSystem
)

# Recast/Detour via vcpkg. vcpkg manifest feature: `nav`.
# Auto-detect availability and gracefully degrade to A* / stub fallbacks if
# the user opted out of the `nav` feature in VCPKG_MANIFEST_FEATURES.
# Source files (DetourNavMap.cpp, ObstacleAvoidance.cpp) already guard their
# Recast/Detour usage with `#if GE_PATHFINDING_BACKEND_DETOUR`.
option(GE_PATHFINDING_BACKEND_DETOUR "Enable Recast/Detour backend for Pathfinding module" ON)
if(GE_PATHFINDING_BACKEND_DETOUR)
    find_package(RecastNavigation CONFIG QUIET)
    if(NOT RecastNavigation_FOUND)
        message(STATUS "Pathfinding: RecastNavigation not found (vcpkg manifest feature 'nav' disabled?) — using stub backend")
        set(GE_PATHFINDING_BACKEND_DETOUR OFF CACHE BOOL "Enable Recast/Detour backend for Pathfinding module" FORCE)
    endif()
endif()

if(GE_PATHFINDING_BACKEND_DETOUR)
    target_link_libraries(Pathfinding PRIVATE
        RecastNavigation::Recast
        RecastNavigation::Detour
        RecastNavigation::DetourCrowd
    )
    target_compile_definitions(Pathfinding PUBLIC GE_PATHFINDING_BACKEND_DETOUR=1)
else()
    target_compile_definitions(Pathfinding PUBLIC GE_PATHFINDING_BACKEND_DETOUR=0)
endif()

# Tests
if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_executable(PathfindingTests
        Tests/PathBufferTests.cpp
        Tests/GridMapTests.cpp
        Tests/AStarSolverTests.cpp
        Tests/HexGridHelpersTests.cpp
        Tests/NavigationWorldTests.cpp
        Tests/DetourNavMapTests.cpp
        Tests/PathSmootherTests.cpp
    )
    # CMake contributes an OBJECT library's .obj files only for DIRECT link
    # dependencies, so Pathfinding's own PUBLIC OBJECT deps must be named again
    # here or their definitions never reach the exe.
    target_link_libraries(PathfindingTests PRIVATE
        Pathfinding
        Logger
        JobSystem
        AssetCore
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    target_include_directories(PathfindingTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
    ge_set_output_tests(PathfindingTests)
    ge_stage_runtime_dependencies(PathfindingTests)
    add_test(NAME PathfindingTests COMMAND PathfindingTests)
endif()
