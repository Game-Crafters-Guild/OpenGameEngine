# ECSBenchCompare — cross-framework ECS micro-benchmark
# (GameEngine ECS vs EnTT vs flecs vs gaia-ecs), modeled on the case shapes of
# https://github.com/abeimler/ecs_benchmark with vendored frameworks.
#
# Additive tooling target: this directory is only reached when BUILD_TESTING=ON
# (see Tests/CMakeLists.txt), and EXCLUDE_FROM_ALL keeps the target out of every
# default build. Build it explicitly:
#   cmake --build --preset vs2026-x64-local --config Release --target ECSBenchCompare

add_executable(ECSBenchCompare EXCLUDE_FROM_ALL
    src/Main.cpp
    src/BenchOurs.cpp
    src/BenchEntt.cpp
    src/BenchFlecs.cpp
    src/BenchGaia.cpp
    third_party/flecs/flecs.c
)

# Four frameworks' headers in one binary — keep TUs isolated.
set_target_properties(ECSBenchCompare PROPERTIES UNITY_BUILD OFF)

target_compile_features(ECSBenchCompare PUBLIC cxx_std_20)

target_include_directories(ECSBenchCompare PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party        # <entt/entt.hpp>
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/flecs  # <flecs.h>
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/gaia   # <gaia.h>
)

# Linked the way ECSCoreTests links the ECS module (Tests/ECS/CMakeLists.txt).
target_link_libraries(ECSBenchCompare PRIVATE
    Logger
    JobSystem
    ECS
)

if(WIN32)
    target_compile_definitions(ECSBenchCompare PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    # flecs' http/rest addons use winsock.
    target_link_libraries(ECSBenchCompare PRIVATE ws2_32)
endif()

if(MSVC)
    # Same SIMD baseline as the shipped ECS module (/arch:AVX2), applied to ALL
    # framework lanes — one flag set for every TU keeps the comparison fair.
    target_compile_options(ECSBenchCompare PRIVATE /arch:AVX2)
endif()

ge_set_output_tools(ECSBenchCompare)
ge_stage_runtime_dependencies(ECSBenchCompare)
