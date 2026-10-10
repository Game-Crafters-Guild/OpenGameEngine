#pragma once

#include "Physics/PhysicsWorld.h"

#include <cstdint>
#include <memory>

namespace GameEngine::PhysicsECS
{
// Simple singleton service for the physics world.
// This keeps PhysicsECS modular while still providing a globally accessible world instance.
class PhysicsWorldService
{
public:
    static void Initialize(const Physics::PhysicsWorldSettings& settings = {});
    static void Shutdown();

    static Physics::PhysicsWorld& Get();
    static Physics::PhysicsWorld* TryGet();
    static bool IsInitialized();

    // The settings used to initialize the current world (or defaults if not initialized).
    static const Physics::PhysicsWorldSettings& GetSettings();

    // Fixed-step render interpolation alpha in [0,1].
    // 0 = render exactly at previous physics snapshot, 1 = render at latest physics state.
    static float32 GetRenderInterpolationAlpha();
    static void SetRenderInterpolationAlpha(float32 a);

    // Monotonic generation counter for the physics world instance.
    // Increments on Initialize() and Shutdown(). Useful for resetting per-system
    // cached state (e.g. fixed-step accumulator) when restarting Play Mode.
    static std::uint64_t GetWorldGeneration();

    // Wall-clock cost of the most recent PhysicsStepSystem update (covering all
    // fixed sub-steps taken that frame), in milliseconds. Written by
    // PhysicsStepSystem, read by editor diagnostics (Monitors "Time/Physics").
    // 0 on a frame where no sub-step ran; resets to 0 across world lifetimes.
    static float32 GetLastStepMs();
    static void SetLastStepMs(float32 ms);

    // Entities the most recent writeback updates composed a pose for (WritePhysicsPose); the rest
    // were skipped as unchanged, so a scene at rest reads 0. Bodies are written by
    // PhysicsWritebackSystem, characters by CharacterControllerWritebackSystem; two values, because
    // the two systems may run in the same wave. Read by editor diagnostics (Monitors
    // "Physics/BodiesComposed", "Physics/CharactersComposed"); reset to 0 across world lifetimes.
    static uint32 GetLastBodiesComposed();
    static void SetLastBodiesComposed(uint32 count);
    static uint32 GetLastCharactersComposed();
    static void SetLastCharactersComposed(uint32 count);

private:
    static std::unique_ptr<Physics::PhysicsWorld> s_World;
    static Physics::PhysicsWorldSettings s_Settings;
    static float32 s_RenderInterpolationAlpha;
    static std::uint64_t s_WorldGeneration;
    static float32 s_LastStepMs;
    static uint32 s_LastBodiesComposed;
    static uint32 s_LastCharactersComposed;
};

} // namespace GameEngine::PhysicsECS

