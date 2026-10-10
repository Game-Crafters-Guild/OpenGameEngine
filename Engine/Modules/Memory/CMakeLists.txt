cmake_minimum_required(VERSION 3.20)

project(Memory LANGUAGES CXX)

set(MEMORY_SOURCES
    Source/MemoryManager.cpp
    Source/AllocationCategory.cpp
    Source/AllocationCounter.cpp
)

add_library(Memory OBJECT ${MEMORY_SOURCES})

target_compile_features(Memory PUBLIC cxx_std_20)

# Engine/Include keeps includes like "Memory/MemoryManager.h" working; the module's
# own Include holds the allocation counter's headers.
target_include_directories(Memory
    PUBLIC
        ${CMAKE_SOURCE_DIR}/Engine/Include
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        Source
        Testing
)

# A target that links Memory (or Engine, which re-exports this definition) reaches the
# process's one allocation counter here, so the copy of AllocationCounter.cpp that
# cmake/AllocationHook.cmake adds to test executables compiles empty in it.
target_compile_definitions(Memory INTERFACE GE_ALLOCATION_COUNTER_LINKED)

# Dependencies
# Logger is used by MemoryManager.cpp; Types provides cross-cutting basic types
target_link_libraries(Memory PUBLIC Logger Types)

if(MSVC)
    target_compile_options(Memory PRIVATE /W4)
else()
    target_compile_options(Memory PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING AND NOT EMSCRIPTEN)
    add_subdirectory(Tests)
endif()

message(STATUS "Memory: Configured as static library")
