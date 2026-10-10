#include "ECSModules/Rendering/RenderingLoop.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "Engine/Rendering/DeviceLostEcsRecovery.h"
#include "Engine/Rendering/MeshReloadBoundsRefresh.h"
#include "Engine/Rendering/ModelRenderSetup.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/RetargetRenderFeature.h"
#include "Rendering/Core/Device.h"

#include "Logger/Logger.h"

namespace GameEngine { namespace Engine::Renderer {

bool RenderingLoop::Initialize(RenderServices* renderServices) {
    if (m_Initialized) return true;
    auto& js = EngineCore::GetInstance().GetJobSystem();
    m_Systems = std::make_unique<ECS::SystemManager>(&js);

    // Rendering systems reach for the device inside their update — a feature
    // that initialises itself on first use, a pipeline compiled on demand. Where
    // GPU objects belong to the thread that made them, a wave dispatched onto a
    // worker kills that worker on the first such call, so waves run inline
    // there. This is the only place that has both the schedule and the device.
    if (renderServices != nullptr)
    {
        const Rendering::IDevice* device = renderServices->GetDevice();
        if (device != nullptr && !device->GetCapabilities().supportsMultithreadedResourceCreation)
        {
            m_Systems->SetParallelWavesEnabled(false);
            Logger::Log::Info("RenderingLoop: ECS waves pinned to the update thread — this "
                              "device confines GPU objects to their creating thread");
        }
    }

    m_Schedule = std::make_unique<ECS::SystemScheduleBuilder>();
    AddRenderingSystemsToSchedule(*m_Schedule, renderServices);
    m_ScheduleBuilt = false;

    m_RenderServices = renderServices;
    m_Initialized = true;
    return true;
}

void RenderingLoop::BuildSchedule()
{
    if (m_ScheduleBuilt)
        return;
    if (!m_Systems)
        return;

    if (m_Schedule)
    {
        m_Schedule->BuildAndRegisterWithWaves(*m_Systems);
        // The builder stays alive as the schedule model: package DLLs loaded
        // at project open / player init contribute systems after this point,
        // and IntegrateLateSystems() re-solves the plan with them included.
    }
    m_ScheduleBuilt = true;
}

void RenderingLoop::IntegrateLateSystems()
{
    if (!m_Schedule || !m_Systems || !m_ScheduleBuilt)
        return;
    m_Schedule->BuildAndRegisterWithWaves(*m_Systems);
}

void RenderingLoop::Update(ECS::World& world, float32 deltaTime) {
    if (!m_Initialized || !m_Systems) return;
    if (!m_ScheduleBuilt)
    {
        // If the Engine didn’t explicitly build the schedule, do it now with
        // whatever has been added so far.
        BuildSchedule();
    }
    // Q6 slice 3b: run the ECS component-handle recovery pass exactly once per
    // in-place device rebuild, BEFORE the suppression gate below. Slice 3a
    // re-provisioned the RenderServices-owned GPU layer inside the rebuild callback,
    // but handles cached on ECS components (morph runtime meshes) and the
    // change-gated extraction it drives are unreachable from that fan-out. This
    // pass runs on THIS thread (which owns the world) while the device is still
    // AwaitingReprovision — after 3a, before extraction resumes at slice-4 resume —
    // zeroing the component caches and force-dirtying the render columns. The stamp
    // persists through the suppressed frames and the first Healthy extraction sees
    // it. Polling the generation (not a callback) keeps this on the world-owning
    // thread and needs no world-capturing device callback.
    if (m_RenderServices)
    {
        if (auto* device = m_RenderServices->GetDevice())
        {
            const uint64 gen = device->GetDeviceRebuildGeneration();
            if (gen != m_LastDeviceRebuildGeneration)
            {
                m_LastDeviceRebuildGeneration = gen;
                if (gen != 0)
                    RecoverEcsComponentHandlesAfterDeviceRebuild(world, *m_RenderServices);
            }
        }
    }

    // Q6 slice 2: while the device is lost / rebuilding / awaiting re-provision,
    // its GPU-scoped resources (skin-palette atlas mapped buffers, per-frame write
    // pool, GPU animation stores, ...) are dead or being replaced in place. This
    // render-prep path runs independent of the window BeginFrame gate, so without
    // this skip it would dereference dangling handles the rebuild teardown freed
    // (e.g. SkinPaletteAtlas::Upload into a stale mapped pointer). Suppress the
    // whole render-services frame until re-provision (slice 3a) returns the device
    // to Healthy. Hung keeps running (its buffers are alive — nothing was torn
    // down). This is suppression, not re-provision: slice 3a owns rebuilding these
    // systems and calling NotifyReprovisionComplete().
    if (m_RenderServices)
    {
        if (auto* device = m_RenderServices->GetDevice())
        {
            const Rendering::DeviceHealth health = device->GetDeviceHealth();
            if (health != Rendering::DeviceHealth::Healthy && health != Rendering::DeviceHealth::Hung)
            {
                // Stamp the first suppressed tick so the resume path can report the
                // observed recovery wall-time (Q6 slice 4).
                if (!m_RenderSuppressed)
                {
                    m_RenderSuppressed = true;
                    m_SuppressionStart = std::chrono::steady_clock::now();
                }
                return;
            }
            // Healthy/Hung again after a suppression episode: this is the first tick
            // that resumes the render-services frame (the picture returns as
            // extraction rebuilds records from slice-3b's force-dirty). Report the
            // wall-time from first-suppressed to first-resumed.
            if (m_RenderSuppressed)
            {
                const double resumeMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - m_SuppressionStart).count();
                Logger::Log::Warning(
                    "Q6 slice 4: render-services frame RESUMED after {:.1f}ms suppressed (device recovery); "
                    "first full-lane extraction rebuilds all records this tick.",
                    resumeMs);
                m_RenderSuppressed = false;
            }
        }
    }

    // Per-frame reset of world draw builder state. This must happen BEFORE any
    // extraction system runs (terrain, render, particles, etc.) because multiple
    // systems submit draws independently.
    if (m_RenderServices)
    {
        m_RenderServices->Textures().ObserveCpuTextureWorld(
            {world.GetWorldId(), world.GetLifecycleResetGeneration()});
        m_RenderServices->BeginWorldDrawFrame();

        // Reset per-frame GPU buffer pools BEFORE systems run. AnimationSystem
        // and SkinningUploadSystem allocate from PerFrameWritePool via
        // SkinPaletteAtlas (Reserve / Upload) — those must see the fresh slot.
        if (auto* device = m_RenderServices->GetDevice())
        {
            const uint32_t frameIdx = device->GetFrameIndex();
            m_RenderServices->GetPerFrameWritePool().BeginFrame(frameIdx);
            m_RenderServices->GetSkinPaletteAtlas().BeginFrame(m_RenderServices->GetPerFrameWritePool());
            // Age each runtime's palette offset into its previous-frame field.
            // Must sit between the atlas's new frame and the producers below:
            // at this point AtlasPaletteOffsetBones still names LAST frame's
            // atlas slot, which is what the TAA skinned motion-vector pass
            // pairs with the previous-frame palette buffer.
            SkeletonStore::Instance().RollPaletteOffsets();
            // Clear previous frame's animation instances. Instances added during
            // this frame's AnimationSystem update will survive until the compute
            // pass executes (end of frame). The next frame's BeginFrame clears.
            m_RenderServices->GetGPUAnimationDataStore().BeginFrame(frameIdx);
            // Same contract for the GPU retarget data store: clears
            // m_HandledRuntimes + m_DispatchScheduledThisFrame so the per-
            // frame collectors start clean before HumanoidRetargetSystem
            // populates them.
            if (auto* retargetFeat = m_RenderServices->GetFeature<RetargetRenderFeature>())
            {
                if (retargetFeat->IsInitialized())
                    retargetFeat->BeginFrame(frameIdx);
            }
        }

        // An in-place model reload (drained inside BeginWorldDrawFrame above)
        // swapped geometry under live mesh handles, so every LocalBounds derived
        // from those meshes still describes the old geometry. Re-derive it here:
        // after the drain that performed the reload, and BEFORE the schedule, so
        // no wave can observe a refreshed GPUMesh row against a stale bound.
        //
        // Ahead of the schedule rather than as a system in it, for the same
        // reason the device-rebuild recovery above runs here: waves are solved
        // purely from declared dependency edges, and this column's readers are
        // spread across modules and packages (OceanExtraction and
        // EZTreeExtraction both read it and declare only {TransformHierarchy,
        // Camera}). A scheduled writer would need an edge from every reader,
        // including ones a package adds later; running before any wave needs
        // none and cannot be widened by a future reader.
        const std::vector<GUID> reloadedModels = m_RenderServices->TakeModelsReloadedInPlace();
        if (!reloadedModels.empty())
        {
            m_SkeletonResolveState = {};
            const MeshReloadBoundsRefreshReport boundsReport =
                RefreshLocalBoundsAfterMeshReload(world, *m_RenderServices, reloadedModels);
            if (boundsReport.ChangedAnything())
            {
                Logger::Log::Info(
                    "[AssetReload] LocalBounds re-derived after in-place mesh reload: "
                    "{} of {} renderable(s) updated ({} unresolved handle(s))",
                    boundsReport.BoundsRefreshed, boundsReport.EntitiesVisited,
                    boundsReport.HandlesUnresolved);
            }
        }

        // A model the drain reloaded or released invalidates the registration
        // the resolve service keeps for its later spawns.
        m_SceneResolves.EvictModels(m_RenderServices->TakeModelsInvalidated());

        // Main thread: play-spawned MeshRenderers only store asset GUIDs.
        // Extraction's Update runs on a job worker; RegisterAndPrewarmMaterial
        // asserts the owner thread. Bind here, before any wave: hand what
        // changed to the resolve service (an entity of a registered model binds
        // in that call), bind what has landed for one frame slice (no frame
        // waits for a load), then bind the skeletons of the entities the
        // service has completed.
        BindChangedMeshRenderers(world, *m_RenderServices, m_MeshBindGate, m_SceneResolves);
        m_SceneResolves.Step(world, *m_RenderServices, kResolveFrameSlice);
        BindChangedSkinnedMeshAnimation(world, m_SkeletonResolveState, m_SceneResolves);
    }

    m_Systems->Update(world, deltaTime);
}

bool RenderingLoop::SetEveryNFramesByName(const std::string& systemName, uint32 n) {
    if (!m_Systems) return false;
    return m_Systems->SetSystemEveryNFramesByName(systemName, n);
}

} } // namespace GameEngine::Engine::Renderer
