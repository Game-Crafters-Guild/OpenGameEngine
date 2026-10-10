cmake_minimum_required(VERSION 3.20)

project(Scene LANGUAGES CXX)

add_library(Scene OBJECT
    Source/SceneTlas.cpp
    Source/TlasBuilder.cpp
    Source/TlasRefit.cpp
    Source/TlasTraverse.cpp
    Source/SceneTlasRequest.cpp
)

target_compile_features(Scene PUBLIC cxx_std_20)

target_include_directories(Scene
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
        ${CMAKE_SOURCE_DIR}/Engine/Include
        # PipelineVariantCache.h (via RenderServices.h) exposes Event<const GUID&>.
        ${CMAKE_SOURCE_DIR}/Engine/Modules/Events/Include
        ${CMAKE_SOURCE_DIR}/Engine/ECSModules/Rendering/Include
        # The ECSModules Rendering header transitively pulls in
        # WorldDrawTypes -> MeshGPURegistry -> Rendering/Core/Handle.h.
        # We need the include path but NOT a link dep on Rendering —
        # Scene only inspects POD types from those headers.
        ${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Include
        # RenderServices.h -> AssetManager.h -> FileWatcher/FileWatcher.h.
        # Headers only: the FileWatcher objects reach the binary through the
        # Engine module splice.
        $<TARGET_PROPERTY:FileWatcher,INTERFACE_INCLUDE_DIRECTORIES>
        # RenderExtractionSystem.h holds a Particles::ParticleExtraction by value.
        ${CMAKE_SOURCE_DIR}/Engine/Modules/Particles/Include
)

target_link_libraries(Scene
    PUBLIC
        Types
        Mathematics
        ECS
    PRIVATE
        Logger
        AssetCore
)

# SceneTlas.cpp / SceneTlasRequest.cpp include Core/CpuProfiler.h, whose
# ScopedCpuProfile layout is gated on GE_ENABLE_CPU_PROFILING and defaults to 1
# when undefined. Scene cannot link Engine (cycle) but is spliced into
# Engine.dll, so without replaying Engine's INTERFACE definitions a
# -DENABLE_CPU_PROFILING=OFF build would put two different ScopedCpuProfile
# layouts inside one binary. Same pattern as TerrainECS.
target_compile_definitions(Scene PRIVATE
    $<TARGET_PROPERTY:Engine,INTERFACE_COMPILE_DEFINITIONS>
)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_executable(SceneTests
        Tests/SceneTlasTests.cpp
    )
    target_include_directories(SceneTests PRIVATE
        ${CMAKE_SOURCE_DIR}/Engine/Include
        ${CMAKE_SOURCE_DIR}/Engine/ECSModules/Rendering/Include
    )
    # Engine ONLY — Scene is an OBJECT module already spliced into Engine.dll;
    # linking it here as well would give the exe a second copy of the module's
    # file-scope state (issue #356, same disease as #340). Scene headers
    # propagate via Engine's re-exported INTERFACE, and Engine.dll exports the
    # GE_* hot-reload dispatcher, so no local stub compile is needed either.
    target_link_libraries(SceneTests PRIVATE
        Engine
        GTest::gtest
        GTest::gtest_main
    )
    ge_set_output_tests(SceneTests)
    ge_stage_runtime_dependencies(SceneTests)
    add_test(NAME SceneTests COMMAND SceneTests)
endif()
