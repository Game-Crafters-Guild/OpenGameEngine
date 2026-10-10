#pragma once

#include "ECS/ChangeFilter.h"
#include "ECS/Systems.h"
#include "Core/Engine.h"
#include "ECSModules/Rendering/Systems/RegisterRenderingSystems.h"
#include "ECS/SystemScheduling.h"
#include "Engine/Rendering/SceneResolveService.h"
#include "Engine/Rendering/SkeletonResolveState.h"

#include <chrono>

namespace GameEngine { namespace Engine::Renderer {

class RenderServices;

class RenderingLoop {
public:
    // The main-thread time the engine's resolve service may spend each frame
    // binding models and materials that have landed (SceneResolveService::Step).
    static constexpr std::chrono::microseconds kResolveFrameSlice{2000};

    RenderingLoop() = default;
    ~RenderingLoop() = default;

    // Initialize the shared schedule and contribute rendering systems.
    bool Initialize(RenderServices* renderServices);

    // Modules add systems to the shared schedule before building it.
    // The schedule is built lazily on the first Update() if not built explicitly.
    // The builder outlives the build (it is the persistent schedule model), so
    // late registrants — package DLLs loaded at project open / player init —
    // add their systems here too, then call IntegrateLateSystems().
    ECS::SystemScheduleBuilder& GetScheduleBuilder() { return *m_Schedule; }
    void BuildSchedule();

    // Re-solve the wave plan after systems were added to the schedule builder
    // post-build. Late systems land in exactly the wave a from-scratch build
    // would give them (declared phase/order/dependencies honored).
    // Main thread, between Update ticks.
    void IntegrateLateSystems();

    // Step all registered systems for the given world
    void Update(ECS::World& world, float32 deltaTime);

    // Access to underlying SystemManager for advanced customization
    ECS::SystemManager* GetSystemManager() { return m_Systems.get(); }

    // The engine's resolve service for the session: every unresolved
    // MeshRenderer of the world this loop updates goes through it. A host
    // hands it a freshly loaded scene (EnqueueWorld) and reads its batch.
    SceneResolveService& GetSceneResolveService() { return m_SceneResolves; }

    // Convenience: configure frequency by system name (Every N frames)
    bool SetEveryNFramesByName(const std::string& systemName, uint32 n);

private:
    std::unique_ptr<ECS::SystemManager> m_Systems;
    std::unique_ptr<ECS::SystemScheduleBuilder> m_Schedule;
    RenderServices* m_RenderServices = nullptr; // non-owning, for per-frame setup
    bool m_Initialized = false;
    bool m_ScheduleBuilt = false;
    // Q6 slice 3b: last observed device rebuild generation. When the device's
    // generation moves, the ECS component-handle recovery pass runs once on the
    // world before extraction resumes (see Update).
    uint64 m_LastDeviceRebuildGeneration = 0;
    // Q6 slice 4: whether the previous tick suppressed the render-services frame
    // while the device was rebuilding, and when suppression began — so the first
    // resumed tick can log the observed recovery wall-time (loss -> first frame).
    bool m_RenderSuppressed = false;
    std::chrono::steady_clock::time_point m_SuppressionStart{};
    // Main-thread bind of Changed MeshRenderers (play spawn). Must not live
    // inside RenderExtractionSystem — that Update runs on a job worker and
    // RegisterAndPrewarmMaterial asserts the owner thread.
    ECS::ChangeGate m_MeshBindGate{};
    SceneResolveService m_SceneResolves;
    SkeletonResolveState m_SkeletonResolveState{};
};

} } // namespace GameEngine::Engine::Renderer
