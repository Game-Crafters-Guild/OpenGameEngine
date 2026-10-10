cmake_minimum_required(VERSION 3.20)

project(VolumetricFog LANGUAGES CXX)

add_library(VolumetricFog OBJECT
    Include/Engine/Rendering/VolumetricFogRenderer.h
    Include/Engine/Rendering/VolumetricFogSettings.h
    Include/Engine/Rendering/Pipeline/Nodes/VolumetricFogNode.h
    Source/VolumetricFogRenderer.cpp
    Source/Pipeline/Nodes/VolumetricFogNode.cpp
)

target_compile_features(VolumetricFog PUBLIC cxx_std_20)

target_include_directories(VolumetricFog
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
        ${CMAKE_SOURCE_DIR}/Engine/Include
        # PipelineVariantCache.h (via RenderServices.h) exposes Event<const GUID&>.
        ${CMAKE_SOURCE_DIR}/Engine/Modules/Events/Include
)

target_link_libraries(VolumetricFog
    PUBLIC
        GameEngineRendering
        Logger
        Mathematics
    PRIVATE
        AssetCore
        AssetDatabase
        AssetRuntime
        # RenderPipelineNodes.h (node base class) carries the C12 module
        # registration stamp, which lives in the ECS module.
        ECS
        FileWatcher
        JobSystem
        Types
)

if(MSVC)
    target_compile_options(VolumetricFog PRIVATE /W4 /bigobj)
else()
    target_compile_options(VolumetricFog PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
    add_executable(VolumetricFogGpuTests Tests/VolumetricFogGpuTests.cpp)
    # GameEngineRendering links Logger and FileSystem PRIVATE, so an executable
    # that calls into them needs their objects named here, as the standalone
    # rendering tests do. FileSystemObjects is an INTERFACE carrier: link it.
    target_link_libraries(VolumetricFogGpuTests PRIVATE GameEngineRendering
        FileSystemObjects GTest::gtest_main)
    target_sources(VolumetricFogGpuTests PRIVATE $<TARGET_OBJECTS:Logger>
        ${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/stb_image_impl.cpp)
    target_compile_definitions(VolumetricFogGpuTests PRIVATE GE_HAVE_STB)
    target_compile_features(VolumetricFogGpuTests PRIVATE cxx_std_20)
    target_compile_definitions(VolumetricFogGpuTests PRIVATE
        GE_FOG_SHADER_DIR="${CMAKE_BINARY_DIR}/Shaders")
    add_dependencies(VolumetricFogGpuTests CompileShaders)
    ge_set_output_tests(VolumetricFogGpuTests)
    ge_stage_runtime_dependencies(VolumetricFogGpuTests)
    add_test(NAME VolumetricFogGpuTests COMMAND VolumetricFogGpuTests)
    set_tests_properties(VolumetricFogGpuTests PROPERTIES LABELS "Headless;GPU" TIMEOUT 120)
endif()
