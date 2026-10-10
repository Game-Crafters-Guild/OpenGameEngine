#pragma once

#include "ECS/Systems.h"
#include "AssetCore/GUID.h"
#include "Animation/AnimationPose.h"
#include "Animation/PoseStack.h"
#include "Animation/RetargetAssetWatcher.h"
#include "Components/Animation/HumanoidRetargeterComponent.h"
#include "Engine/Rendering/AnimationSampling.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace GameEngine
{

class AssetManager;

namespace Animation
{
class AnimationClip;
class HumanoidRig;
class RetargetMap;
class RetargetNode;
struct AnimationPose;
}

namespace Engine::Renderer
{
class RenderServices;
class RetargetGPUDataStore;
}
}

namespace GameEngine { namespace Engine::Renderer {

// Phase 6 ECS bridge. One per-frame iteration over entities carrying a
// HumanoidRetargeterComponent + SkeletonRef; resolves RetargetMap +
// HumanoidRigs, reserves a per-character GPU slot in RetargetGPUDataStore,
// samples the source clip, and either:
//   * GPU path (default): uploads the source-pose snapshot. Render graph
//     passes (encode -> FK -> translate -> world-pose -> IK goal -> IK
//     solve -> op stack -> pose-to-palette) then run on this slot during
//     the rendering wave, writing skin matrices straight into the
//     SkinPaletteAtlas — no CPU pose round-trip.
//   * CPU fallback (r.RetargetCPU=1 / GE_RETARGET_CPU=1, or no GPU device):
//     runs Phase 3 RetargetNode and writes the result to
//     SkeletonRuntimeState::CompactSkinMatrices via BuildPoseToSkinMatrices.
//     SkinningUploadSystem then ships the matrices to the atlas.
//
// Same-rig sampling stays in AnimationSystem. Entities that also carry
// AnimatorRef treat it as the clip / time / blend source of truth; this
// system samples and retargets. AnimationSystem skips those entities.
class HumanoidRetargetSystem : public ECS::ISystem
{
public:
    explicit HumanoidRetargetSystem(RenderServices* renderServices);
    ~HumanoidRetargetSystem();

    const char* GetName() const override { return "HumanoidRetargetSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

    // Test seam: drop all per-character cached state. Called between tests
    // that recycle the global SkeletonStore / ClipStore so stale RetargetNode
    // builds don't leak into a fresh world.
    void ResetForTest();

    // Test seam: read the LOD level the most recent Update() applied for a
    // given target runtimeId. Returns Full when no entry exists. Used by the
    // perf gate test to verify LOD overrides actually take effect.
    ::GameEngine::Components::HumanoidRetargetLOD GetLastLODForRuntime(uint32 runtimeId) const;

private:
    // Retained capacity only: references/results are cleared after each Update.
    // Shared clips resolve their model once per update, including pending misses.
    struct SourceResolution
    {
        std::shared_ptr<Animation::AnimationClip> Clip;
        uint32 SkeletonId = 0;
        // A source model whose load is still in flight, as opposed to one that
        // can never resolve: only the second is worth a diagnostic.
        bool Pending = false;
    };
    std::vector<SourceResolution> m_FrameSources;

    // Everything a character's skin palette is a function of. Time and blend
    // alpha are raw float bits: exact equality, no epsilons, so a paused
    // character produces an identical record every frame. Clip and PrevClip
    // are the identities of the sampled clip objects, compared and never
    // dereferenced: a re-import replaces a ClipStore slot under the same
    // index, and the replacement is built while the slot still holds the
    // previous clip, so one replacement never shares the previous clip's
    // address. Two replacements of one slot between two updates can: the
    // second may reuse the first clip's freed address and compare equal.
    struct PaletteInputs
    {
        uint32 RuntimeId = 0;
        uint32 TargetSkeletonId = 0;
        uint32 SourceSkeletonId = 0;
        uint32 BuildGeneration = 0;
        ::GameEngine::GUID MapGUID;
        const Animation::AnimationClip* Clip = nullptr;
        uint32 ClipIndex = 0;
        uint32 TimeBits = 0;
        const Animation::AnimationClip* PrevClip = nullptr;
        uint32 PrevClipIndex = 0;
        uint32 PrevTimeBits = 0;
        uint32 BlendAlphaBits = 0;
        ::GameEngine::Components::HumanoidRetargetLOD LOD =
            ::GameEngine::Components::HumanoidRetargetLOD::Full;

        bool operator==(const PaletteInputs&) const = default;
    };

    // Per-character cached state. Keyed by SkeletonRef::runtimeId so two
    // submeshes sharing one runtime collapse to one entry.
    struct CharacterEntry
    {
        // Cached resolved assets. Cleared when a hot-reload event fires
        // for the matching GUID.
        ::GameEngine::GUID MapGUID;
        const Animation::RetargetMap* Map = nullptr;
        const Animation::HumanoidRig* SourceRig = nullptr;
        const Animation::HumanoidRig* TargetRig = nullptr;
        // Runtime IDs for the per-entity source skeleton and target
        // skeleton runtimes. Source = the SkeletonData we sample the clip
        // onto; Target = the entity's SkeletonRef::runtimeId (where final
        // skin matrices go).
        uint32 SourceSkeletonId = 0;
        uint32 SourceRuntimeId  = 0;
        uint32 TargetSkeletonId = 0;
        uint32 TargetRuntimeId  = 0;
        // GPU slot in RetargetGPUDataStore (~0u when GPU not available).
        uint32 GPUSlot = static_cast<uint32>(~0u);
        // CPU evaluator for the fallback path. Built lazily on first use.
        std::unique_ptr<Animation::RetargetNode> CpuNode;
        bool CpuNodeBuilt = false;
        bool MapDirty = true; // rebuild GPU map upload + CPU node on next tick
        // Advances on every RetargetNode build (new map, rig or source
        // skeleton, hot reload): the evaluated pose can differ for unchanged
        // clip inputs, so it is part of the palette input record.
        uint32 BuildGeneration = 0;
        // Phase 8: most-recently-applied LOD level. Used for transitions
        // (PoseHold -> Full forces a full evaluate this frame to refresh the
        // cached pose) and exposed for tests.
        ::GameEngine::Components::HumanoidRetargetLOD LastLOD =
            ::GameEngine::Components::HumanoidRetargetLOD::Full;

        // Phase 12 (audit §1.1): hoisted per-frame scratch. The pre-Phase-12
        // path constructed PoseStack + AnimationPose inside the parallel
        // ECS lambda each frame — 6,000 allocs/sec at 100 chars × 60 Hz.
        // Both objects are owned per-character-entry so concurrent ticks
        // on different characters don't collide; both reuse their internal
        // storage across frames once warmed up.
        Animation::PoseStack ScratchStack;
        Animation::AnimationPose OutLocalPose;

        // GE_RETARGET_DIAG=1 one-shot guards. Track whether the per-character
        // build / first-eval log has already fired so we emit a single line
        // per character rather than per-frame spam.
        bool DiagBuildLogged = false;
        bool DiagEvalLogged  = false;

        // One-shot guard for the unresolvable-source warning. Cleared as soon
        // as the clip's source model resolves, so a later failure reports again.
        bool SourceUnusableLogged = false;

        // The inputs of the last evaluated pose. PoseHold reports these, so
        // a held character keeps its record while its clip time advances.
        PaletteInputs EvaluatedInputs;
    };

    // Resolve / load a RetargetMap GUID through AssetManager. Caches the
    // resolved pointer. Returns nullptr if the asset can't be loaded.
    const Animation::RetargetMap* ResolveMap(const ::GameEngine::GUID& mapGuid);
    const Animation::HumanoidRig* ResolveRig(const ::GameEngine::GUID& rigGuid);

    // Drain hot-reload events from the watcher. Called once at the top of
    // Update(), BEFORE the per-character iteration so no entry processes
    // a torn pointer mid-frame.
    void DrainHotReloadEvents();

    // GPU primer: ReserveCharacter + UploadMap (if dirty) + WriteSourcePose.
    // Returns true if the slot is live and the render graph passes can run.
    bool PrimeGPU(CharacterEntry& entry, float32 clipTime);

    RenderServices*            m_RenderServices = nullptr;
    AssetManager*              m_AssetManager   = nullptr;
    RetargetGPUDataStore*      m_GPUStore       = nullptr;

    Animation::RetargetAssetWatcher m_Watcher;
    std::atomic<bool>          m_RigReloadPending{false};
    std::atomic<bool>          m_MapReloadPending{false};
    std::atomic<bool>          m_ProfileReloadPending{false};
    std::vector<::GameEngine::GUID> m_PendingRigReloads;
    std::vector<::GameEngine::GUID> m_PendingMapReloads;
    std::mutex                 m_PendingReloadMutex;

    // Per-runtimeId character entries. Keyed by target runtimeId because
    // that's the stable identity (mesh root); entities with multiple
    // submeshes share one entry.
    std::unordered_map<uint32, CharacterEntry> m_Characters;

    // Resolved-asset caches. Populated on demand via AssetManager.
    std::unordered_map<::GameEngine::GUID, const Animation::RetargetMap*> m_MapCache;
    std::unordered_map<::GameEngine::GUID, const Animation::HumanoidRig*> m_RigCache;

    // Source-clip sample workspace shared across all characters. CPU
    // sampling reuses the buffers; the parallel iteration is sequential
    // for v1 (GPU primer is the heavy lift; the CPU side is microseconds).
    PoseSampleWorkspace m_SourceSampleWorkspace;
    PoseSampleWorkspace m_BlendFromWorkspace;
    PoseSampleWorkspace m_TargetPoseWorkspace;

    // Palette input records of the characters that produced a pose this
    // update, in query order, and of the previous update. Any difference
    // (an input changed, a character joined or left) means palette content
    // can differ and is reported to RenderServices; an identical set is
    // not, so a still scene with paused characters lets the depth-derived
    // passes settle.
    std::vector<PaletteInputs> m_FramePaletteInputs;
    std::vector<PaletteInputs> m_LastPaletteInputs;

    bool m_FirstFrameLogged = false;

    // One-shot guard: ensures we wire the GPU store's asset-unload watchers
    // exactly once after both the AssetManager and the GPU store are live.
    // The store keeps GUID -> headerIdx maps that grow unbounded without an
    // unload subscription; wiring it here (rather than at store init) avoids
    // a layering dependency from the rendering subsystem onto AssetManager.
    bool m_GPUStoreAssetEventsAttached = false;
};

// Test hooks: same shape as AnimationSystem's. Run a single CPU evaluation
// for an entity-equivalent inputs without spinning up a full world.
namespace TestHooks
{
    // Run a single CPU retarget evaluation that mirrors what
    // HumanoidRetargetSystem::Update() would do for one character on the
    // CPU fallback path. Writes CompactSkinMatrices into the target
    // runtime. Returns false if the inputs can't be resolved.
    //
    // map / sourceRig / targetRig must be live for the call. sourceClip is
    // sampled at clipTime onto a freshly created source SkeletonData
    // populated from sourceRig.
    bool RunCPUEvaluateForTest(const Animation::RetargetMap& map,
                               const Animation::HumanoidRig& sourceRig,
                               const Animation::HumanoidRig& targetRig,
                               uint32 sourceClipIndex,
                               uint32 sourceSkeletonId,
                               uint32 targetSkeletonId,
                               uint32 targetRuntimeId,
                               float32 clipTime);

    // Phase 8: LOD-aware variant. lodLevel selects which stages run; the
    // PoseHold case performs no work and returns true immediately.
    bool RunCPUEvaluateAtLODForTest(const Animation::RetargetMap& map,
                                    const Animation::HumanoidRig& sourceRig,
                                    const Animation::HumanoidRig& targetRig,
                                    uint32 sourceClipIndex,
                                    uint32 sourceSkeletonId,
                                    uint32 targetSkeletonId,
                                    uint32 targetRuntimeId,
                                    float32 clipTime,
                                    ::GameEngine::Components::HumanoidRetargetLOD lodLevel);
}

} } // namespace GameEngine::Engine::Renderer
