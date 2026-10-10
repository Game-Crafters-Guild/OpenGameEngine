# Rendering/Animation tests (modern)
# This CMakeLists is consumed from the top-level Tests/CMakeLists, which already calls
# enable_testing() and add_subdirectory(Tests/Rendering).
# Do not call cmake_minimum_required() here; this is a subdirectory.


cmake_minimum_required(VERSION 3.20)

find_package(GTest REQUIRED)

# Phase 1: Engine.dll now exports the GE_* hot-reload stubs (Engine.cpp's
# Source/HotReloadNativeStubs.cpp). Tests linking Engine inherit them via the
# import lib; no per-target compile of the stub source needed.
add_executable(AnimationSystemTests AnimationSystemTests.cpp
    AnimationEventDispatchTests.cpp
    AnimatorClipTimeTests.cpp
    ${CMAKE_SOURCE_DIR}/Tests/EngineShutdownTestEnvironment.cpp)
add_executable(TimelinePlaybackTests TimelinePlaybackTests.cpp)
add_executable(TransformMathTests TransformMathTests.cpp)
add_executable(TextCoreMetricsTests
    TextCoreMetricsTests.cpp
)

add_executable(MaterialLitCubeTests
    ${CMAKE_CURRENT_SOURCE_DIR}/MaterialLitCubeTests.cpp
)

# Modules are OBJECT libs aggregated into Engine.dll. Linking modules
# directly alongside Engine would re-splice their .obj files into the test
# exe, producing duplicate file-scope state vs. the canonical copy inside
# Engine.dll. Link Engine alone and resolve module symbols across the
# import lib.
target_link_libraries(TextCoreMetricsTests
    PRIVATE
    Engine
    GTest::gtest
    GTest::gtest_main
)

# This suite calls FT_* directly. Engine forwards its modules' INTERFACE compile
# definitions (so GE_HAVE_FREETYPE=1 arrives and the FreeType branch compiles)
# but links those modules PRIVATE, so FreeType's import library does not come
# with it. Same probe as the Text module: linked exactly when defined.
find_package(Freetype CONFIG QUIET)
if(TARGET Freetype::Freetype)
    target_link_libraries(TextCoreMetricsTests PRIVATE Freetype::Freetype)
endif()

target_link_libraries(MaterialLitCubeTests
    PRIVATE
    Engine
    GTest::gtest
    GTest::gtest_main
)
if(TARGET GameEngine.Native)
    target_link_libraries(MaterialLitCubeTests PRIVATE GameEngine.Native)
endif()
target_include_directories(MaterialLitCubeTests
    PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Tests
    # StagedTestPaths.h — exe-anchored resolution of the staged adapter shaders.
    ${CMAKE_SOURCE_DIR}/Engine/Tests
)
# The suite composes a material against the adapter GLSL that StageTestAssets
# mirrors into the build root, so the mirror has to exist before it runs.
if(TARGET StageTestAssets)
    add_dependencies(MaterialLitCubeTests StageTestAssets)
endif()
set_target_properties(MaterialLitCubeTests PROPERTIES UNITY_BUILD OFF)
include(GoogleTest)
add_test(NAME TextCoreMetricsTests COMMAND TextCoreMetricsTests)
add_test(NAME MaterialLitCubeTests COMMAND MaterialLitCubeTests)



target_link_libraries(AnimationSystemTests
    PRIVATE
    Engine
    GTest::gtest
    GTest::gtest_main
)
target_include_directories(AnimationSystemTests
    PRIVATE
    # StagedTestPaths.h — exe-anchored resolution of the staged Assets/ root.
    ${CMAKE_SOURCE_DIR}/Engine/Tests
)
# The engine-initialising tests mount the Assets/ mirror StageTestAssets stages
# into the build root as their asset root, so the mirror has to exist before
# they run.
if(TARGET StageTestAssets)
    add_dependencies(AnimationSystemTests StageTestAssets)
endif()

target_link_libraries(TimelinePlaybackTests
    PRIVATE
    Engine
    GTest::gtest
    GTest::gtest_main
)
if(TARGET GameEngine.Native)
    target_link_libraries(TimelinePlaybackTests PRIVATE GameEngine.Native)
endif()

target_link_libraries(TransformMathTests
	    PRIVATE
	    Engine
	    GTest::gtest
	    GTest::gtest_main
	)


set_target_properties(AnimationSystemTests TimelinePlaybackTests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

# Output layout: bin/<Config>/Tests for all test executables, with the runtime
# DLL closure (gtest, Engine.dll and its transitive vcpkg deps) staged beside
# each exe so the binaries run standalone. See cmake/OutputLayout.cmake.
foreach(_ge_t IN ITEMS
    AnimationSystemTests
    TimelinePlaybackTests
    TransformMathTests
    TextCoreMetricsTests
    MaterialLitCubeTests
)
    ge_set_output_tests(${_ge_t})
    ge_stage_runtime_dependencies(${_ge_t})
endforeach()

# AnimationGraphCompileFromModel.LocomotionSampleAnimGraphCompiles compiles the
# shipped sample from the exe-anchored install assets root, so the sample is
# staged beside the executable.
ge_stage_test_asset(AnimationSystemTests
    "${CMAKE_SOURCE_DIR}/Apps/Editor/Assets/Graphs/LocomotionSample.animgraph"
    "Assets/Graphs/LocomotionSample.animgraph")

include(GoogleTest)
add_test(NAME AnimationSystemTests COMMAND AnimationSystemTests)
add_test(NAME TimelinePlaybackTests COMMAND TimelinePlaybackTests)
add_test(NAME TransformMathTests COMMAND TransformMathTests)

# The working directory is the engine's workspace root; the build root, like
# every other engine-initialising suite's. The asset root is exe-anchored.
set_tests_properties(AnimationSystemTests PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
