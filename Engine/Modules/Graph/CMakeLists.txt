cmake_minimum_required(VERSION 3.20)

# Graph Library - Graph model, asset, and serialization
project(Graph LANGUAGES CXX)

# Source files
set(GRAPH_SOURCES
    Source/GraphValue.cpp
    Source/GraphTypeRegistry.cpp
    Source/GraphModel.cpp
    Source/GraphGlslAuthoring.cpp
    Source/GraphAsset.cpp
    Source/GameLogicActionCatalog.cpp
    Source/GameLogicGraphCompiler.cpp
    Source/GameLogicRuntime.cpp
    Source/GraphFsmRuntime.cpp
    Source/MaterialGraphCompiler.cpp
    Source/SgGraphModelBridge.cpp
    Source/ShaderGraphTemplate.cpp
)

# Header files
set(GRAPH_HEADERS
    Include/Graph/GraphValue.h
    Include/Graph/GraphTypeRegistry.h
    Include/Graph/GraphModel.h
    Include/Graph/GraphGlslAuthoring.h
    Include/Graph/GraphAsset.h
    Include/Graph/GameLogicActionCatalog.h
    Include/Graph/NodeParamOption.h
    Include/Graph/GameLogicGraphCompiler.h
    Include/Graph/GameLogicRuntime.h
    Include/Graph/GraphFsmRuntime.h
    Include/Graph/MaterialGraphCompiler.h
    Include/Graph/SgGraphModelBridge.h
    Include/Graph/ShaderGraphTemplate.h
)

# Create static library
add_library(Graph OBJECT ${GRAPH_SOURCES} ${GRAPH_HEADERS})

# Set C++20 standard
target_compile_features(Graph PUBLIC cxx_std_20)

# Include directories
target_include_directories(Graph PUBLIC Include)
target_include_directories(Graph PRIVATE Source)

# Dependencies
if(TARGET AssetCore)
    target_link_libraries(Graph PUBLIC AssetCore)
endif()

# nlohmann/json for serialization
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(Graph PRIVATE nlohmann_json::nlohmann_json)

if(TARGET GameEngineRendering)
    target_link_libraries(Graph PUBLIC GameEngineRendering)
    target_include_directories(Graph PUBLIC
        ${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Include)
endif()

# Platform-specific definitions
if(WIN32)
    target_compile_definitions(Graph PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# Compiler-specific options
if(MSVC)
    target_compile_options(Graph PRIVATE /W4)
else()
    target_compile_options(Graph PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Export compile commands for IDEs
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Debug information
message(STATUS "Graph: Configured as static library")
message(STATUS "Graph: Include directory: ${CMAKE_CURRENT_SOURCE_DIR}/Include")

if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    include(GoogleTest)

    add_executable(MaterialGraphCompilerTests Tests/MaterialGraphCompilerTests.cpp)
    target_compile_features(MaterialGraphCompilerTests PUBLIC cxx_std_20)
    target_link_libraries(MaterialGraphCompilerTests PRIVATE
        Graph
        GameEngineRendering
        FileSystemObjects
        AssetCore
        Logger
        GTest::gtest
        GTest::gtest_main
    )
    target_sources(MaterialGraphCompilerTests PRIVATE
        ${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/stb_image_impl.cpp)
    target_compile_definitions(MaterialGraphCompilerTests PRIVATE GE_HAVE_STB)
    # StagedTestPaths.h — staged-fixture resolution shared by all fixture suites.
    target_include_directories(MaterialGraphCompilerTests PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Tests)
    if(COMMAND ge_set_output_tests)
        ge_set_output_tests(MaterialGraphCompilerTests)
    endif()
    if(COMMAND ge_stage_runtime_dependencies)
        ge_stage_runtime_dependencies(MaterialGraphCompilerTests)
    endif()
    if(COMMAND ge_stage_shader_graph_nodes)
        ge_stage_shader_graph_nodes(MaterialGraphCompilerTests)
    endif()
    gtest_discover_tests(MaterialGraphCompilerTests)

    add_executable(GraphSerializationTests
        Tests/GraphSerializationTests.cpp
        Tests/GraphTypeRegistryTests.cpp)
    target_compile_features(GraphSerializationTests PUBLIC cxx_std_20)
    target_link_libraries(GraphSerializationTests PRIVATE
        Graph
        GameEngineRendering
        FileSystemObjects
        AssetCore
        Logger
        GTest::gtest
        GTest::gtest_main
    )
    target_sources(GraphSerializationTests PRIVATE
        ${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/stb_image_impl.cpp)
    target_compile_definitions(GraphSerializationTests PRIVATE GE_HAVE_STB)
    if(COMMAND ge_set_output_tests)
        ge_set_output_tests(GraphSerializationTests)
    endif()
    if(COMMAND ge_stage_runtime_dependencies)
        ge_stage_runtime_dependencies(GraphSerializationTests)
    endif()
    gtest_discover_tests(GraphSerializationTests)
endif()
