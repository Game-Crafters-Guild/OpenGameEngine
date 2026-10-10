cmake_minimum_required(VERSION 3.20)

project(NativeScripting LANGUAGES CXX)

# Native C++ scripting / hot-reload module: the manager, the file watcher, the RAII
# module handle and the user ABI header.
set(NATIVE_SCRIPTING_SOURCES
    Source/BuildCacheRecord.cpp
    Source/EngineBuildIdentity.cpp
    Source/NativeScriptManager.cpp
    Source/NativeSourceTree.cpp
    Source/ToolchainFingerprint.cpp
    Source/PrebuiltModuleBinaries.cpp
    Source/UserModuleHandle.cpp
    Source/UserProjectGenerator.cpp
    Source/CMakeInvoker.cpp
    Source/SdkManifest.cpp
    Source/MsvcToolchain.cpp
    Source/UserSystemRegistry.cpp
)
# SEH crash-isolation for user systems is Windows-only (__try/__except); the header provides a
# passthrough elsewhere, so the TU is compiled only on Windows.
if(WIN32)
    list(APPEND NATIVE_SCRIPTING_SOURCES Source/GuardedUserInvoke.cpp)
endif()
# NOTE: GameSDK/Source/UserModuleEntry.cpp is deliberately NOT compiled into the
# engine — it defines the user-module ABI EXPORTS and is compiled into each user
# DLL by the generated user-project build (UserProjectGenerator adds it as a source).

add_library(NativeScripting OBJECT ${NATIVE_SCRIPTING_SOURCES})

target_compile_features(NativeScripting PUBLIC cxx_std_20)

target_include_directories(NativeScripting
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        Source
)

if(MSVC)
    target_compile_options(NativeScripting PRIVATE /W4)
else()
    target_compile_options(NativeScripting PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Types: UniquePtr/String in the public header.
# Logger: diagnostics (cpp only).
# FileWatcher: exports its own FileWatcher/FileWatcher.h and re-exports
#   Engine/Include, so the manager cpp resolves Assets/FileWatchingService.h
#   and the Engine/Build headers. The FileWatchingService
#   symbols themselves live in the AssetRuntime objects already inside Engine.dll.
# AssetCore: AssetIgnoreRules — the directories a source walk and the generated
#   user project both skip (cpp only; the objects reach the binary via the splice).
# CMAKE_DL_LIBS: dlopen/dlsym on POSIX (empty on Windows).
target_link_libraries(NativeScripting
    PUBLIC
        Types
    PRIVATE
        AssetCore
        Logger
        FileWatcher
        JobSystem      # WorkStealingThreadPool.h — async build dispatch (cpp only)
        # C12 reload protocol, include-path only (objects reach the binary via
        # the Engine module splice): ECS for the per-load registration stamp,
        # PluginAPI for the registration hold + load-abort purge.
        ECS
        PluginAPI
        ${CMAKE_DL_LIBS}
)

message(STATUS "NativeScripting: Configured as OBJECT library")

# GePrebuiltStamp — engine-build helper that stamps prebuilt engine-package
# module binaries (Packages.cmake): prints the host toolchain's prebuilt
# platform dir name, the module file name the loader probes in it, and a staged
# SDK's engine-ABI digest. Compiled from the standalone NativeScripting TUs —
# never the whole OBJECT module, whose manager objects need Engine.dll-resident
# symbols — and with the engine's global flags, so its HostToolchainFingerprint
# matches the editor's.
add_executable(GePrebuiltStamp
    Tools/GePrebuiltStampMain.cpp
    Source/BuildCacheRecord.cpp
    Source/PrebuiltModuleBinaries.cpp
    Source/SdkManifest.cpp
    Source/ToolchainFingerprint.cpp)
set_target_properties(GePrebuiltStamp PROPERTIES UNITY_BUILD OFF FOLDER "Tools")
target_include_directories(GePrebuiltStamp PRIVATE Include Source)
target_link_libraries(GePrebuiltStamp PRIVATE Types Logger)
