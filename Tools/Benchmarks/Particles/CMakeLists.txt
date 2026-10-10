# ParticleRuntimeBenchmark: CPU time of the particle runtime per frame for a fixed stack at several
# particle counts and emitter splits. A diagnostic, not a test; its numbers mean something only from
# a Release build on a quiet machine. EXCLUDE_FROM_ALL keeps it out of every default build:
#   cmake --build --preset vs2026-x64-local --config Release --target ParticleRuntimeBenchmark

# The processor registry's module stamp is the one ECS source the particle module needs beyond the
# logger, compiled in.
add_executable(ParticleRuntimeBenchmark EXCLUDE_FROM_ALL ParticleRuntimeBenchmark.cpp
               ${CMAKE_SOURCE_DIR}/Engine/Modules/ECS/Source/ModuleRegistration.cpp)
target_compile_features(ParticleRuntimeBenchmark PRIVATE cxx_std_20)
target_include_directories(ParticleRuntimeBenchmark PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Include)
target_link_libraries(ParticleRuntimeBenchmark PRIVATE Particles AssetCore Logger)
ge_set_output_tools(ParticleRuntimeBenchmark)
