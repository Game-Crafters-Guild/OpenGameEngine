cmake_minimum_required(VERSION 3.20)

project(Physics LANGUAGES CXX)

add_library(Physics OBJECT
    Source/PhysicsWorld.cpp
)

target_compile_features(Physics PUBLIC cxx_std_20)

target_include_directories(Physics
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
)

# Core dependencies
target_link_libraries(Physics
    PUBLIC
        Types
        Mathematics
        GenerationalVector
        Logger
    PRIVATE
        Platform
)

# Jolt via vcpkg (default backend). vcpkg manifest feature: `physics`.
# Auto-detect availability and gracefully degrade to a stub backend if the
# user opted out of the `physics` feature in VCPKG_MANIFEST_FEATURES.
option(GE_PHYSICS_BACKEND_JOLT "Enable Jolt backend for Physics module" ON)
if(GE_PHYSICS_BACKEND_JOLT)
    # The exported CMake package/targets vary by port version; support both layouts.
    # - Newer ports install a config package named "Jolt" under lib/cmake/Jolt
    # - Some environments expose "unofficial-joltphysics" with target unofficial::joltphysics::Jolt
    find_package(Jolt CONFIG QUIET)
    if(NOT (TARGET Jolt::Jolt OR TARGET Jolt))
        find_package(unofficial-joltphysics CONFIG QUIET)
    endif()
    if(NOT (TARGET Jolt::Jolt OR TARGET Jolt OR TARGET unofficial::joltphysics::Jolt))
        message(STATUS "Physics: Jolt not found (vcpkg manifest feature 'physics' disabled?) — using stub backend")
        set(GE_PHYSICS_BACKEND_JOLT OFF CACHE BOOL "Enable Jolt backend for Physics module" FORCE)
    endif()
endif()

if(GE_PHYSICS_BACKEND_JOLT)
    # JoltPhysicsBackend.cpp includes <Jolt/Jolt.h> unconditionally, so only
    # compile it when Jolt is present.
    target_sources(Physics PRIVATE Source/Backend/JoltPhysicsBackend.cpp)
    if(TARGET Jolt::Jolt)
        target_link_libraries(Physics PRIVATE Jolt::Jolt)
    elseif(TARGET Jolt)
        target_link_libraries(Physics PRIVATE Jolt)
    else()
        target_link_libraries(Physics PRIVATE unofficial::joltphysics::Jolt)
    endif()
    target_compile_definitions(Physics PUBLIC GE_PHYSICS_BACKEND_JOLT=1)
else()
    target_compile_definitions(Physics PUBLIC GE_PHYSICS_BACKEND_JOLT=0)
endif()

