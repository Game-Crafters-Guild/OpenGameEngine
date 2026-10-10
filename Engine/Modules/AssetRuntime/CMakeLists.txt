cmake_minimum_required(VERSION 3.20)

project(AssetRuntime LANGUAGES CXX)

set(ASSETRUNTIME_SOURCES
    Source/AssetManager.cpp
    Source/AssetRegistry.cpp
    Source/AssetDependencyExtractor.cpp
    Source/ParserRegistry.cpp
    Source/MaterialAsset.cpp
    Source/MaterialXImport.cpp
    Source/TextureAlphaProbe.cpp
    Source/RenderPipelineAsset.cpp
    Source/ShaderProgramAsset.cpp
    Source/ShaderSourceAsset.cpp
    Source/ScriptAsset.cpp
    Source/XmlAsset.cpp
    Source/BinaryAsset.cpp
    Source/FileWatchingService.cpp
    Source/ExpectedWriteLedger.cpp
    Source/ExpectedAssetWrite.cpp
    Source/AssetIOService.cpp
    Source/AssetTasks.cpp
    Source/AsyncRegistryTasks.cpp
    Source/AssetDbProfiler.cpp
    Source/MaterialAssetParser.cpp
    Source/SceneAssetParser.cpp
    Source/AnimationAssetParser.cpp
    Source/UILayoutAssetParser.cpp
    Source/UIStyleAssetParser.cpp
    Source/FlareAtlasAsset.cpp
    Source/LensFlareDefinitionAsset.cpp
    Source/LensFlareImporter.cpp
    Source/LensFlareParsers.cpp
    Source/TerrainMaterialLibraryAsset.cpp
    Source/TerrainMaterialLibraryParser.cpp
)

# Model import (glTF via cgltf; FBX via ufbx; Blend). The glTF decode path is
# header-only C and builds for wasm, so the web runtime imports glTF at load
# time rather than requiring a pre-cook. FBX/Blend ride the same parser; their
# assets simply do not appear in web projects.
list(APPEND ASSETRUNTIME_SOURCES Source/ModelAssetParser.cpp)

add_library(AssetRuntime OBJECT ${ASSETRUNTIME_SOURCES})

# C++ standard
set_target_properties(AssetRuntime PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

target_compile_features(AssetRuntime PUBLIC cxx_std_20)

target_include_directories(AssetRuntime
	    PUBLIC
	        ${CMAKE_SOURCE_DIR}/Engine/Include
	        ${CMAKE_SOURCE_DIR}/Engine/Modules/Animation/Include
	    PRIVATE
	        Source
	        ${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Include
	        ${CMAKE_SOURCE_DIR}/Engine/Modules/UI/Include
	        ${CMAKE_SOURCE_DIR}/Engine/Modules/Mathematics/Include
	        ${CMAKE_SOURCE_DIR}/Engine/Modules/Scheduler/Include
	        ${CMAKE_SOURCE_DIR}/Engine/Modules/Pathfinding/Include
	        ${CMAKE_SOURCE_DIR}/Engine/Modules/Particles/Include
	)

# Link dependencies
# - Logger (logging)
# - AssetCore (core asset types/interfaces)
# - JobSystem (async tasks/dependencies)
# - FileWatcher (file change notifications)
target_link_libraries(AssetRuntime
    PUBLIC
        Logger
        AssetCore
        AssetDatabase
        JobSystem
        FileWatcher
    PRIVATE
        Platform
        FileSystem
)

# Optional JSON dependency (ParserRegistry / tasks)
find_package(nlohmann_json CONFIG QUIET)
if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(AssetRuntime PRIVATE nlohmann_json::nlohmann_json)
endif()

# MaterialX (.mtlx) import uses pugixml (already a vcpkg dependency, shared with the UI XML parser).
find_package(pugixml CONFIG QUIET)
if(TARGET pugixml::pugixml)
    target_link_libraries(AssetRuntime PRIVATE pugixml::pugixml)
endif()

# Unity builds aggregate many TUs and the AssetRegistry forwarder pulls in
# enough headers (registry, dep graph, parsers, dep edge types, snapshot, …)
# that the unity .cxx exceeds the COFF section-count limit on MSVC. Same
# reason the main Engine target uses /bigobj.
if(MSVC)
    target_compile_options(AssetRuntime PRIVATE /bigobj)
endif()

