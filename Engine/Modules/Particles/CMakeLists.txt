cmake_minimum_required(VERSION 3.20)

project(Particles LANGUAGES CXX)

add_library(Particles OBJECT
    Include/Components/Rendering/Particles.h
    Include/Components/Rendering/ParticleRenderer.h
    Source/Particles.cpp
    Source/ParticleChannels.cpp
    Source/ParticleValue.cpp
    Source/ParticleProcessorRegistry.cpp
    Source/ParticleProcessorContext.cpp
    Source/ParticleParameterAccess.cpp
    Source/ParticleStackDocument.cpp
    Source/ParticleStackValidation.cpp
    Source/ParticleStackCompiler.cpp
    Source/ParticleStackBinary.cpp
    Source/ParticleTrailStore.cpp
    Source/ParticleRuntime.cpp
    Source/Processors/ParticleBuiltInProcessors.cpp
    Source/Processors/ParticlePropertyProcessor.cpp
    Source/Processors/ParticleAccelerationProcessor.cpp
    Source/Processors/ParticleDragProcessor.cpp
    Source/Processors/ParticleLimitSpeedProcessor.cpp
    Source/Processors/ParticleNoiseProcessor.cpp
    Source/Processors/ParticleCollisionProcessor.cpp
    Source/Processors/ParticleLightProcessor.cpp
    Source/Processors/ParticleShapeProcessor.cpp
    Source/Processors/ParticleVelocityConeProcessor.cpp
    Source/Processors/ParticleOrbitProcessor.cpp
    Source/Processors/ParticleEmitRateProcessor.cpp
    Source/Processors/ParticleEmitBurstProcessor.cpp
    Source/Processors/ParticleEventProcessor.cpp
    Source/Processors/ParticleTrailProcessor.cpp
)

target_compile_features(Particles PUBLIC cxx_std_20)

target_include_directories(Particles
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
        ${CMAKE_SOURCE_DIR}/Engine/Include
)

target_link_libraries(Particles
    PUBLIC
        AssetCore
        ECS
        Mathematics
        Types
    PRIVATE
        Noise
)

# JSON conversion is used by scene ingestion and editor authoring, not simulation.
add_library(ParticlesAuthoring OBJECT
    Source/ParticleStackAuthoring.cpp
    Include/Particles/ParticleStackAuthoring.h
)
target_compile_features(ParticlesAuthoring PUBLIC cxx_std_20)
target_link_libraries(ParticlesAuthoring PUBLIC Particles Serialization)

# The particle stack asset and its parser. Engine supplies the asset and parser interfaces.
add_library(ParticlesAssets OBJECT
    Source/Assets/ParticleStackAsset.cpp
    Source/Assets/ParticleStackParser.cpp
    Include/Particles/Assets/ParticleStackAsset.h
    Include/Particles/Assets/ParticleStackParser.h
)
target_compile_features(ParticlesAssets PUBLIC cxx_std_20)
target_include_directories(ParticlesAssets
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        $<TARGET_PROPERTY:Engine,INTERFACE_INCLUDE_DIRECTORIES>
)
target_link_libraries(ParticlesAssets PUBLIC ParticlesAuthoring AssetCore)

# ECS scheduling + physics sweep stay module-owned but separate from the
# headless runtime/stack library so ParticleRuntimeTests stay Engine-free.
add_library(ParticlesSystems OBJECT
    Source/Systems/ParticleSimulationSystem.cpp
    Include/Particles/Systems/ParticleSimulationSystem.h
)
target_compile_features(ParticlesSystems PUBLIC cxx_std_20)
target_include_directories(ParticlesSystems
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
        $<TARGET_PROPERTY:Engine,INTERFACE_INCLUDE_DIRECTORIES>
)
target_link_libraries(ParticlesSystems
    PUBLIC
        Particles
    PRIVATE
        ParticlesAssets
        PhysicsECS
)

# Extraction + materials. Like Ocean: PUBLIC Particles, do NOT link Engine
# (SHARED cycle). Reach Engine headers via INTERFACE include directories.
add_library(ParticlesRendering OBJECT
    Source/Rendering/ParticleExtraction.cpp
    Source/Rendering/ParticleMaterials.cpp
    Include/Particles/Rendering/ParticleExtraction.h
    Include/Particles/Rendering/ParticleMaterials.h
    Include/Particles/Rendering/ParticleRenderData.h
)
target_compile_features(ParticlesRendering PUBLIC cxx_std_20)
target_include_directories(ParticlesRendering
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
        $<TARGET_PROPERTY:Engine,INTERFACE_INCLUDE_DIRECTORIES>
)
target_link_libraries(ParticlesRendering
    PUBLIC
        Particles
    PRIVATE
        PhysicsECS
        GPUFogParticles
)

if(MSVC)
    target_compile_options(Particles PRIVATE /W4)
    target_compile_options(ParticlesAuthoring PRIVATE /W4)
    target_compile_options(ParticlesAssets PRIVATE /W4)
    target_compile_options(ParticlesSystems PRIVATE /W4)
    target_compile_options(ParticlesRendering PRIVATE /W4)
else()
    target_compile_options(Particles PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(ParticlesAuthoring PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(ParticlesAssets PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(ParticlesSystems PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(ParticlesRendering PRIVATE -Wall -Wextra -Wpedantic)
endif()

message(STATUS "Particles: Configured as object module (Particles / ParticlesAuthoring / ParticlesAssets / ParticlesSystems / ParticlesRendering)")
