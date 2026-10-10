cmake_minimum_required(VERSION 3.20)

project(FileWatcher LANGUAGES CXX)

set(FILEWATCHER_SOURCES
    Source/FileIdentity.cpp
    Source/FileWatcher.cpp
)

add_library(FileWatcher OBJECT ${FILEWATCHER_SOURCES})

target_compile_features(FileWatcher PUBLIC cxx_std_20)

# Public headers: FileWatcher/FileWatcher.h and FileWatcher/FileIdentity.h.
# Engine/Include is PUBLIC as well: FileWatcher.cpp reads Platform/Thread.h
# from it, and NativeScripting reaches its Engine headers through this
# re-export. Engine's public headers include FileWatcher/FileWatcher.h
# (AssetManager.h, ScriptManager.h, FileWatchingService.h), so Engine
# re-exports this module's include dirs to every consumer.
target_include_directories(FileWatcher
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
        ${CMAKE_SOURCE_DIR}/Engine/Include
    PRIVATE
        Source
)
if(MSVC)
    target_compile_options(FileWatcher PRIVATE /W4)
else()
    target_compile_options(FileWatcher PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(FileWatcher
    PUBLIC Logger Types
)

if(APPLE)
    # FSEvents wakes the polling loop the moment the filesystem reports a change
    # instead of leaving detection latency to the poll interval alone.
    target_link_libraries(FileWatcher PUBLIC "-framework CoreServices")
endif()

message(STATUS "FileWatcher: Configured as OBJECT library (linked into Engine)")

