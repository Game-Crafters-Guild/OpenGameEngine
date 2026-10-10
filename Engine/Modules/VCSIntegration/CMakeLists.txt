cmake_minimum_required(VERSION 3.20)

project(VCSIntegration LANGUAGES CXX)

set(VCSINTEGRATION_SOURCES
    Source/VCSCommandExecutor.cpp
    Source/VCSStatusPoller.cpp
)

add_library(VCSIntegration OBJECT ${VCSINTEGRATION_SOURCES})

target_compile_features(VCSIntegration PUBLIC cxx_std_20)

target_include_directories(VCSIntegration
    PUBLIC
        ${CMAKE_SOURCE_DIR}/Engine/Modules/VCSIntegration/Include
        ${CMAKE_SOURCE_DIR}/Engine/Include
    PRIVATE
        Source
)

# Dependencies
target_link_libraries(VCSIntegration
    PUBLIC Logger Types
    PRIVATE Platform
)

if(MSVC)
    target_compile_options(VCSIntegration PRIVATE /W4)
else()
    target_compile_options(VCSIntegration PRIVATE -Wall -Wextra -Wpedantic)
endif()

message(STATUS "VCSIntegration: Configured as static library")
