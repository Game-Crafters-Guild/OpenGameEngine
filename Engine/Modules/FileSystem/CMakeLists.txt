cmake_minimum_required(VERSION 3.20)

project(FileSystem LANGUAGES CXX)

# File copying and publication: nothing behind them but the standard library
# and Logger plus a header-only Platform process identity. Native copies through
# the platform's own copy_file so permissions
# survive; the browser streams, and publishes without a rename.
set(FILESYSTEM_CORE_SOURCES
    Source/FileSystem.cpp
    Source/RenameWithRetry.cpp
)
if(EMSCRIPTEN)
    list(APPEND FILESYSTEM_CORE_SOURCES Source/FileSystemCore_Web.cpp)
else()
    list(APPEND FILESYSTEM_CORE_SOURCES Source/FileSystemCore_Native.cpp)
endif()

add_library(FileSystemCore OBJECT ${FILESYSTEM_CORE_SOURCES})
target_compile_features(FileSystemCore PUBLIC cxx_std_20)
target_include_directories(FileSystemCore
    PUBLIC ${CMAKE_SOURCE_DIR}/Engine/Modules/FileSystem/Include
    PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Modules/Platform/Include)
target_link_libraries(FileSystemCore PUBLIC Logger)

# Tree removal and the project-library root differ by platform: the browser
# build deletes OPFS entries with the browser's own API and confines projects
# to the persistent mount; native removes on a JobSystem channel's blocking
# thread (TreeRemoval) and lets the user choose where projects live.
if(EMSCRIPTEN)
    set(FILESYSTEM_SOURCES Source/FileSystem_Web.cpp Source/ScopedFileLock_Web.cpp Source/OrphanedTemporaryFiles.cpp Source/TreeRemoval_Web.cpp)
else()
    set(FILESYSTEM_SOURCES Source/FileSystem_Native.cpp Source/ScopedFileLock_Native.cpp Source/OrphanedTemporaryFiles.cpp Source/TreeRemoval_Native.cpp)
endif()

add_library(FileSystem OBJECT ${FILESYSTEM_SOURCES})

# An OBJECT library contributes its .obj files only to DIRECT link
# dependencies, so a test executable that links one of this module's consumers
# — the Rendering, Pathfinding and platform-provider object libraries — resolves
# their headers but not the FileSystem symbols they call. Linking this INTERFACE
# target carries the core objects and their include directory, so the rule lives
# here instead of at every one of those test targets. It deliberately carries
# only the core: the rest needs JobSystem and Platform, which those executables
# do not link. Engine.dll splices both halves itself and must not link this.
add_library(FileSystemObjects INTERFACE)
target_link_libraries(FileSystemObjects INTERFACE FileSystemCore)
target_sources(FileSystemObjects INTERFACE $<TARGET_OBJECTS:FileSystemCore>)

target_compile_features(FileSystem PUBLIC cxx_std_20)

target_include_directories(FileSystem
    PUBLIC
        ${CMAKE_SOURCE_DIR}/Engine/Modules/FileSystem/Include
    PRIVATE
        Source
)

target_link_libraries(FileSystem
    PUBLIC Logger FileSystemCore
    PRIVATE JobSystem Platform
)

if(MSVC)
    target_compile_options(FileSystem PRIVATE /W4)
else()
    target_compile_options(FileSystem PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
    find_package(GTest REQUIRED)

    add_executable(FileSystemTests
        Tests/FileSystemTests.cpp
    )

    # Modules are OBJECT libs aggregated into Engine.dll; link Engine to
    # resolve module symbols via the import lib (same pattern as Platform).
    target_link_libraries(FileSystemTests
        PRIVATE
            Engine
            GTest::gtest
            GTest::gtest_main
    )

    ge_set_output_tests(FileSystemTests)
    ge_stage_runtime_dependencies(FileSystemTests)
    add_test(NAME FileSystemTests COMMAND FileSystemTests)
endif()

message(STATUS "FileSystem: Configured as static library")
