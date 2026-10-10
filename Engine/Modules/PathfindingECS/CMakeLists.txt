cmake_minimum_required(VERSION 3.20)

project(PathfindingECS LANGUAGES CXX)

add_library(PathfindingECS OBJECT
    Source/NavigationComponentInstantiations.cpp
    Source/NavigationService.cpp
    Source/NavigationGridRuntime.cpp
    Source/SceneGeometryCollector.cpp
    Source/Systems/NavigationBuildSystem.cpp
    Source/Systems/NavigationPathfindingSystem.cpp
    Source/Systems/NavigationMovementSystem.cpp
    Source/Systems/NavigationDebugSystem.cpp
    Source/Systems/RegisterPathfindingSystems.cpp
    Source/Systems/NavigationWorldHooks.cpp
    Source/Scene/NavigationSceneSchemas.cpp
)

target_compile_features(PathfindingECS PUBLIC cxx_std_20)

target_include_directories(PathfindingECS
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
        # NavigationBuildSystem.h includes Assets/FileWatchingService.h, which
        # includes FileWatcher/FileWatcher.h. Headers only: the FileWatcher
        # objects reach the binary through the Engine module splice.
        $<TARGET_PROPERTY:FileWatcher,INTERFACE_INCLUDE_DIRECTORIES>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

target_link_libraries(PathfindingECS
    PUBLIC
        Pathfinding
        ECS
        ECSComponents
        Types
        Mathematics
        Logger
    PRIVATE
        # Engine intentionally NOT listed: Engine is now SHARED (Phase 1) and
        # PRIVATE-links PathfindingECS itself. A direct PathfindingECS -> Engine
        # link would create a SHARED↔STATIC cycle, which CMake forbids. Engine's
        # symbols get resolved when Engine.dll links PathfindingECS.lib. The
        # Engine/Include headers the sources use arrive through ECSComponents.
        PhysicsECS
)

# Tests
if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_executable(PathfindingECSTests
        Tests/NavigationServiceTests.cpp
        Tests/NavSystemIntegrationTests.cpp
        Tests/NavigationComprehensiveTests.cpp
        Tests/NavigationRepathingTests.cpp
        Tests/NavigationRequestReplacementTests.cpp
        Tests/NavigationInitialOccupancyTests.cpp
        Tests/NavigationOccupiedStartTests.cpp
        Tests/AssetSerializationTests.cpp
        Tests/PathfindingScheduleTests.cpp
        Tests/NavigationBuildGatingTests.cpp
        Tests/NavigationTerrainReadinessTests.cpp
        Tests/NavigationGridOwnershipTests.cpp
        ${CMAKE_SOURCE_DIR}/Tests/EngineShutdownTestEnvironment.cpp
    )
    # Test files each declare anonymous-namespace constants (kDeltaTime,
    # kDefaultCellSize, ...) that collide across TUs when unity-merged.
    set_target_properties(PathfindingECSTests PROPERTIES UNITY_BUILD OFF)
    # Engine ONLY — PathfindingECS is an OBJECT module already spliced into
    # Engine.dll; linking it here as well would give the exe a second copy of
    # the module's file-scope state (issue #356, same disease as #340).
    # Headers propagate via Engine's re-exported INTERFACE, and Engine.dll
    # exports the GE_* hot-reload dispatcher, so no local stub compile is
    # needed either.
    target_link_libraries(PathfindingECSTests PRIVATE Engine GTest::gtest GTest::gtest_main)
    target_include_directories(PathfindingECSTests PRIVATE ${CMAKE_SOURCE_DIR}/Tests) # TestTempDir.h
    ge_set_output_tests(PathfindingECSTests)
    ge_stage_runtime_dependencies(PathfindingECSTests)
    add_test(NAME PathfindingECSTests COMMAND PathfindingECSTests)
endif()
