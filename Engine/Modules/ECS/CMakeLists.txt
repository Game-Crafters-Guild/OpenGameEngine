cmake_minimum_required(VERSION 3.20)

# ECS Library - High-Performance Entity Component System
project(ECS LANGUAGES CXX)

# Core ECS source files
set(ECS_SOURCES
    Source/World.cpp
    Source/Entity.cpp
    Source/ComponentRegistry.cpp
    Source/ComponentHandler.cpp
    Source/ModuleRegistration.cpp
    Source/ComponentFieldRegistry.cpp
    Source/ComponentFactory.cpp
    Source/ComponentLayoutReloadMigrator.cpp
    Source/BlobComponent.cpp
    Source/CachedQuery.cpp
    Source/UnresolvedComponentStore.cpp
    Source/OrderedHandleSet.cpp
)

# Core ECS header files
set(ECS_HEADERS
    Include/ECS/ArchetypeTable.h
    Include/ECS/ECS.h
    Include/ECS/Types.h
    Include/ECS/World.h
    Include/ECS/Entity.h
    Include/ECS/Query.h
    Include/ECS/ComponentFlags.h
    Include/ECS/Components.h
    Include/ECS/ComponentRegistry.h
    Include/ECS/IComponentHandler.h
    Include/ECS/ComponentHandler.h
    Include/ECS/ComponentHook.h
    Include/ECS/ComponentFieldRegistry.h
    Include/ECS/ComponentFactory.h
    Include/ECS/ComponentLayoutReloadMigrator.h
    Include/ECS/ComponentBase.h
    Include/ECS/BlobComponent.h
    Include/ECS/CachedQuery.h
    Include/ECS/ChangeFilter.h
    Include/ECS/ComponentConcepts.h
    Include/ECS/QueryAccessProbe.h
    Include/ECS/AutoRegistration.h
    Include/ECS/ECSTemplates.h
    Include/ECS/Systems.h
    Include/ECS/WorldTemplateImplementations.inl
    Include/ECS/OrderedHandleSet.h
)

# Create static library
add_library(ECS OBJECT ${ECS_SOURCES} ${ECS_HEADERS})

# Set C++20 standard (required for auto-registration system)
target_compile_features(ECS PUBLIC cxx_std_20)

# Include directories
# ECS depends on Types/Types.h via the Types module; avoid direct Engine includes.
target_include_directories(ECS PUBLIC Include)
target_include_directories(ECS PRIVATE Source)

# Dependencies
# ECS module dependencies (target-based; avoid hardcoded include paths)
target_link_libraries(ECS PUBLIC Logger JobSystem Types)

# Platform-specific definitions
if(WIN32)
    target_compile_definitions(ECS PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# Compiler-specific options
if(MSVC)
    target_compile_options(ECS PRIVATE /W4)
    # Enable safe multi-proc PDB access and disable specific warnings
    target_compile_options(ECS PRIVATE /FS)
    target_compile_options(ECS PRIVATE /wd4100) # Unreferenced formal parameter
else()
    target_compile_options(ECS PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ECS SIMD support detection with ARM64 compatibility
if(MSVC)
    # Detect architecture for SIMD support based on *target* architecture.
    # For MSVC/Visual Studio, prefer CMAKE_GENERATOR_PLATFORM so an ARM64 host
    # building x64 binaries does not accidentally enable NEON.
    if(CMAKE_GENERATOR_PLATFORM MATCHES "ARM64|arm64")
        # ARM64 NEON support
        target_compile_definitions(ECS PUBLIC ECS_SIMD_ENABLED=1 ECS_SIMD_ARM64=1)
        message(STATUS "ECS: Enabling ARM64 NEON SIMD support (MSVC, generator ARM64)")
    elseif(NOT CMAKE_GENERATOR_PLATFORM AND CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
        # Fallback for single-arch generators (e.g., Ninja with MSVC) on ARM64
        target_compile_definitions(ECS PUBLIC ECS_SIMD_ENABLED=1 ECS_SIMD_ARM64=1)
        message(STATUS "ECS: Enabling ARM64 NEON SIMD support (MSVC, system processor ARM64)")
    else()
        # x64 AVX2 support
        target_compile_options(ECS PRIVATE /arch:AVX2)
        target_compile_definitions(ECS PUBLIC ECS_SIMD_ENABLED=1 ECS_SIMD_X64=1)
        message(STATUS "ECS: Enabling x64 AVX2 SIMD support (MSVC, assuming x64 target)")
    endif()
elseif(EMSCRIPTEN)
    # Scalar path on wasm for now. wasm SIMD128 is a deliberate later opt-in:
    # it needs -msimd128 plus emscripten's emulated x86 intrinsic headers, and
    # the emulation quality should be measured before it becomes the default.
    message(STATUS "ECS: SIMD disabled on Emscripten (scalar path)")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # Check architecture for SIMD support
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
        # ARM64 NEON support
        target_compile_definitions(ECS PUBLIC ECS_SIMD_ENABLED=1 ECS_SIMD_ARM64=1)
        message(STATUS "ECS: Enabling ARM64 NEON SIMD support (GCC/Clang)")
    else()
        # Check for AVX2 support on x64
        include(CheckCXXCompilerFlag)
        check_cxx_compiler_flag("-mavx2" COMPILER_SUPPORTS_AVX2)
        if(COMPILER_SUPPORTS_AVX2)
            target_compile_options(ECS PRIVATE -mavx2)
            target_compile_definitions(ECS PUBLIC ECS_SIMD_ENABLED=1 ECS_SIMD_X64=1)
            message(STATUS "ECS: Enabling x64 AVX2 SIMD support (GCC/Clang)")
        endif()
    endif()
endif()

# Export compile commands for IDEs
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Installation
install(TARGETS ECS
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY Include/ECS
    DESTINATION include
    FILES_MATCHING PATTERN "*.h" PATTERN "*.inl"
)

# Debug information
message(STATUS "ECS: Configured as static library")
message(STATUS "ECS: Include directory: ${CMAKE_CURRENT_SOURCE_DIR}/Include")
message(STATUS "ECS: Source files: ${ECS_SOURCES}")
message(STATUS "ECS: Header files: ${ECS_HEADERS}")
