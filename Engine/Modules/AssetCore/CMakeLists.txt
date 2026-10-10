cmake_minimum_required(VERSION 3.20)

# AssetCore Library - Core asset interfaces and types
project(AssetCore LANGUAGES CXX)

# Source files
set(ASSETCORE_SOURCES
    Source/Asset.cpp
    Source/AssetIgnoreRules.cpp
    Source/EditorOnlyAssetPath.cpp
    Source/AssetRegistry.cpp
    Source/GUID.cpp
    Source/AssetEvents.cpp
    Source/AssetTypes.cpp
    Source/FoldedPathIndex.cpp
    Source/PathNormalization.cpp
    Source/Result.cpp
    Source/DepEdge.cpp
    Source/SharedFileRead.cpp
)

# Header files
set(ASSETCORE_HEADERS
    Include/AssetCore/Asset.h
    Include/AssetCore/AssetIgnoreRules.h
    Include/AssetCore/EditorOnlyAssetPath.h
    Include/AssetCore/AssetRegistry.h
    Include/AssetCore/GUID.h
    Include/AssetCore/AssetEvents.h
    Include/AssetCore/AssetTypes.h
    Include/AssetCore/FoldedPathIndex.h
    Include/AssetCore/PathNormalization.h
    Include/AssetCore/Result.h
    Include/AssetCore/DepEdge.h
    Include/AssetCore/NineSlice.h
    Include/AssetCore/SharedFileRead.h
    Include/AssetCore/SnapshotFingerprint.h
    Include/AssetCore/Types.h
)

# Create static library
add_library(AssetCore OBJECT ${ASSETCORE_SOURCES} ${ASSETCORE_HEADERS})

# Set C++20 standard
target_compile_features(AssetCore PUBLIC cxx_std_20)

# Include directories
target_include_directories(AssetCore PUBLIC Include)
target_include_directories(AssetCore PRIVATE Source)

# Link to core Types module for StringId and shared aliases
if(TARGET Types)
    target_link_libraries(AssetCore PUBLIC Types)
endif()

# utf8proc — Unicode normalization (NFC) and locale-independent case folding,
# used by PathNormalization for cross-platform deterministic path-to-GUID
# derivation. PRIVATE because the .cpp uses utf8proc but the public headers
# don't expose it.
find_package(utf8proc CONFIG REQUIRED)
target_link_libraries(AssetCore PRIVATE utf8proc::utf8proc)

# Platform-specific definitions
if(WIN32)
    target_compile_definitions(AssetCore PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# Compiler-specific options
if(MSVC)
    target_compile_options(AssetCore PRIVATE /W4)
else()
    target_compile_options(AssetCore PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Export compile commands for IDEs
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Installation
install(TARGETS AssetCore
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY Include/AssetCore
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

# Package configuration (simplified for internal use)

if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Tests/CMakeLists.txt")
    add_subdirectory(Tests)
endif()

# Debug information
message(STATUS "AssetCore: Configured as static library")
message(STATUS "AssetCore: Include directory: ${CMAKE_CURRENT_SOURCE_DIR}/Include")
message(STATUS "AssetCore: Source files: ${ASSETCORE_SOURCES}")
message(STATUS "AssetCore: Header files: ${ASSETCORE_HEADERS}")
