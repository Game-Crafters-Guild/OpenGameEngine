#pragma once

#include "Engine/Rendering/IRenderFeature.h"
#include "Engine/Rendering/RetargetFullPass.h"
#include "Engine/Rendering/RetargetGPUDataStore.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace GameEngine
{
namespace Rendering { class IDevice; }

namespace Engine::Renderer
{

// Phase 14 — RenderFeature that owns the per-character RetargetGPUDataStore
// and hands the Phase 4-5 compute passes to the render graph each frame.
//
// Lifecycle (single-threaded — render thread only):
//   1) Initialize(device) at engine boot. Creates the data store + loads the
//      11 retarget compute SPIR-V binaries from the staged shader cache.
//   2) HumanoidRetargetSystem::Update calls SubmitCharacter(...) for every
//      live HumanoidRetargeter entity each frame (gated by r.RetargetGPU=1).
//   3) The render graph builder calls BuildPasses(rg) once per frame; the
//      feature appends the canonical 6-stage pass sequence per character.
//   4) EndFrame() clears the per-frame submission set and increments the
//      frame counter; called immediately after BuildPasses.
//
// Default state (env var GE_RETARGET_GPU/r.RetargetGPU not set or not "1")
// is OFF: SubmitCharacter is a no-op so the CPU fallback continues.
//
// IMPORTANT — math drift vs CPU pipeline (as of Phase 25.3):
//   The compute shaders here implement the OLD Mode A formulation
//       qDelta = bake * srcKey * inv(parent)
//       qTgt   = qRetarget * qDelta  (chain-mode-dependent)
//   The CPU `RetargetNode::TransportStage2` was rewritten in Phase 25.2/25.3
//   to do world-space delta transport with chain-aware Q values
//       canonical_world[b]  = qWorld[b] * authored_world[b]   (built at bake)
//       src_clip_world[b]   = FK over source's authored clip locals (no Q!)
//       delta_world[b]      = src_clip_world * inv(src_canonical_world)
//       target_world[b]     = delta_world * tgt_canonical_world
//       target_local[b]     = inv(parent_world) * target_world
//   Those changes are NOT mirrored on the GPU. The chain-aware Q values
//   from BakeRetargetPoseFromAPose are not uploaded as `bakeRewriteQuatXYZW`,
//   the per-character data store has no slot for canonical world binds, and
//   the encode/FK shaders apply Q to clip locals — exactly the operation
//   25.3 removed because it swallowed the source-vs-target authored-bind
//   offset.
//
//   Until the GPU compute path is rewritten to match the new CPU pipeline,
//   enabling r.RetargetGPU=1 will produce visually wrong cross-rig results
//   (T-pose-y arms, body chain inconsistencies). Same-rig retarget may
//   accidentally render correctly because Q reduces to identity. The CPU
//   path always runs in parallel and is what the renderer actually consumes,
//   so leaving the GPU disabled is correct for now.
//
//   The rewrite scope: encode + FK transport shaders, RetargetGPUDataStore
//   data layout (add canonical world bind arrays + chain-aware Q), parity
//   tests (RetargetGPUTests).
class RetargetRenderFeature final : public IRenderFeature
{
public:
    ~RetargetRenderFeature() override;

    // Lazily initializes the data store + loads compute SPIR-V from the staged
    // shader cache. Returns true when the feature is ready; false (logged
    // once) leaves the CPU fallback in charge. Runs at most once: callers poll
    // it every frame, and neither failure mode — an absent staged shader or a
    // data store the device refused — resolves by asking again.
    bool Initialize(GameEngine::Rendering::IDevice* device);

    bool IsInitialized() const { return m_Initialized; }
    bool IsGpuEnabled() const  { return m_GpuEnabled; }

    RetargetGPUDataStore& GetDataStore() { return m_DataStore; }
    const RetargetGPUDataStore& GetDataStore() const { return m_DataStore; }

    // Q6 slice 4 (§8-completion): forward to the owned data store so its dead
    // clip/rig/character SSBOs are recreated after a device rebuild.
    void OnDeviceRebuilt(GameEngine::Rendering::IDevice* /*device*/) override
    {
        if (m_Initialized)
            m_DataStore.ReprovisionAfterDeviceRebuild();
    }

    // Per-frame begin: clears the data store's per-frame state (handled-runtime
    // set, dispatch-scheduled guard). Called once per frame from
    // RenderingLoop::Update before any system runs.
    void BeginFrame(uint32_t frameIndex);

    // Per-frame end: reserved for future per-character bookkeeping. Currently
    // a no-op; the data store's dirty-bit handling is in FlushIfDirty.
    void EndFrame();

    // Build the fused retarget compute pass onto the render graph if there
    // are active characters this frame. Run once per frame from
    // RenderGraphBuildSystem BEFORE any graphics pass that consumes the
    // SkinPaletteAtlas (the pass uses kEarlySetup to enforce that ordering).
    //
    // Declares on the SAME atlas RGBuffer value the skinning pass writes
    // (handle-dedup = one resource id; RenderGraph orders the writers).
    void BuildPasses(GameEngine::Rendering::RenderGraph::RGFrame& frame,
                     GameEngine::Rendering::RenderGraph::RGBuffer skinPaletteAtlas,
                     float deltaTime);

private:
    // Reads GE_RETARGET_GPU / r.RetargetGPU env var on first call; cached.
    static bool ReadEnvFlag();

    GameEngine::Rendering::IDevice* m_Device = nullptr;
    RetargetGPUDataStore            m_DataStore;
    bool                            m_Initialized = false;
    bool                            m_GpuEnabled  = false;
    // Claimed by the one thread that runs the attempt. Initialize is reached
    // from the frame's worker threads, so without it a build missing the
    // shader re-probes and re-logs from every worker, every frame.
    std::atomic<bool>               m_InitializeAttempted{false};

    // Fused retarget compute SPIR-V (retarget_full.comp). Loaded once at
    // Initialize.
    std::vector<uint8_t>            m_FullSpv;

    // Persistent pass instance. Allocated once in Initialize and reused every
    // frame via CreatePass(name, m_Pass.get(), version). Allocating a fresh
    // instance per frame would race with RG's version-token short-circuit
    // (which keeps the OLD lambda capture) and produce a dangling pointer
    // dereference inside RG::Execute.
    std::unique_ptr<RetargetFullPass> m_Pass;

    uint32_t m_FrameIndex = 0;
};

} // namespace Engine::Renderer
} // namespace GameEngine
