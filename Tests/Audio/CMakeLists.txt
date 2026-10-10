# Audio ECS tests — AudioEmitterSystem lifecycle-consumer coverage
# (P3 #389 lifecycle events + S1 #406 SwapGenerationGuard gap recovery)
# plus AudioListener +Z-forward convention pins.

find_package(GTest REQUIRED)

add_executable(AudioECSTests
    AudioEmitterSystemTests.cpp
    AudioListenerSystemTests.cpp
)

# Modules are OBJECT libs aggregated into Engine.dll; link Engine alone and
# resolve module symbols across the import lib (Tests/Rendering precedent).
set_target_properties(AudioECSTests PROPERTIES UNITY_BUILD OFF)
target_compile_features(AudioECSTests PUBLIC cxx_std_20)

target_link_libraries(AudioECSTests
    PRIVATE
    Engine
    GTest::gtest
    GTest::gtest_main
)

ge_set_output_tests(AudioECSTests)
ge_stage_runtime_dependencies(AudioECSTests)

include(GoogleTest)
gtest_discover_tests(AudioECSTests)
