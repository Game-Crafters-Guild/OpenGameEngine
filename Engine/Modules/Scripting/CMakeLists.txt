cmake_minimum_required(VERSION 3.20)

# Scripting Library - CoreCLR hosting, script management, and path resolution
project(Scripting LANGUAGES CXX)

set(SCRIPTING_SOURCES
    Source/CoreCLRHost.cpp
    Source/ScriptManager.cpp
    Source/PackageScriptCompile.cpp
    Source/PathResolver.cpp
    Source/CompileServerClient.cpp
    Source/PipeTransport.cpp
    Source/JsonCompileTransport.cpp
    Source/HotReloadPipeline.cpp
    Source/HotReloadTasks.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Jobs/WorkspaceId.cpp
)

add_library(Scripting OBJECT ${SCRIPTING_SOURCES})

target_compile_features(Scripting PUBLIC cxx_std_20)
# Build Scripting as a static user of scripting ABI: do not dllimport C ABI symbols
# so that Engine's HotReloadNativeStubs can satisfy references when CLR is disabled
# or GameEngine.Native is not linked.
target_compile_definitions(Scripting PRIVATE GE_SCRIPTING_STATIC)

# Expose Engine/Include so includes like "Scripting/ScriptManager.h" continue to work
# without duplicating headers. Also include the CMake-generated header directory so
# the wrapped Jobs-based CompileServerClient can see Jobs/CompileServerVersion.h.
target_include_directories(Scripting
	PUBLIC
	    ${CMAKE_SOURCE_DIR}/Engine/Include
	PRIVATE
	    Source
	    ${CMAKE_BINARY_DIR}/generated
	)

# CompileServerClient prunes stale host pid files with Platform::IsProcessRunning:
# header only here; Platform objects link once into Engine.
target_include_directories(Scripting PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Modules/Platform/Include)

# Dependencies
# Note: Keep deps target-based; no global include paths.
target_link_libraries(Scripting
    PUBLIC
        Logger
        AssetCore
        JobSystem
        Types
    PRIVATE
        FileWatcher
)

# .NET hosting headers availability for CoreCLRHost (cross-platform)
if(NETHOST_LIBRARY)
	target_include_directories(Scripting PRIVATE ${NETHOST_INCLUDE_DIR})
	target_compile_definitions(Scripting PRIVATE NETHOST_AVAILABLE)
endif()

# Warnings
if(MSVC)
    target_compile_options(Scripting PRIVATE /W4)
else()
    target_compile_options(Scripting PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
message(STATUS "Scripting: Configured as static library")

