#pragma once

#include "ECS/Systems.h"
#include "ECS/Entity.h"
#include "Types/Types.h"
#include "AssetCore/GUID.h"
#include "Assets/AnimationClip.h"
#include "Components/Animation/AnimatorRef.h"
#include "Engine/Rendering/AnimationSampling.h"

#include "AssetCore/AssetReloadInvalidator.h"
#include <array>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GameEngine {
namespace Engine::Renderer {

class RenderServices;
class ClipStore;
// SkeletonData is provided by SkeletonStore.h via the Animation module shim.

// Collects animated entities per frame and dispatches animation work to the GPU
// compute skinning pipeline when the shader is loaded. Falls back to CPU
// sampling until the compute pass is ready (first few frames, or missing SPIR-V).
class AnimationSystem : public ECS::ISystem {
public:
    explicit AnimationSystem(RenderServices* rs) : m_RenderServices(rs) {}
    const char* GetName() const override { return "AnimationSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

    // Per-(skeletonId, clipIndex) GPU-safety predicate. Returns true when
    // the skeleton's joint closure fits the kernel's bone and level limits
    // (GPUAnimationDataStore::FitsKernelLimits; a skeleton over either logs
    // one warning naming its model) and every clip channel's stored
    // boneIndex points to a bone whose name hashes to the channel's
    // targetNameId — i.e. the GPU upload's raw boneIndex flow will drive
    // the correct bone. Same-FBX clips satisfy the name test by
    // construction; cross-rig clips fail because positional ordering
    // doesn't carry across rigs. Public so test hooks can probe routing
    // decisions deterministically.
    bool IsClipSkeletonGPUSafe(uint32 skeletonId, uint32 clipIndex,
                               const SkeletonData& skeleton, ClipStore& clipStore);

private:
    RenderServices* m_RenderServices;

    struct WorkItem {
        uint32 SkeletonId;
        uint32 RuntimeId;
        uint32 ClipIndex;
        float32 Time;
        const SkeletonData* CachedSkeleton;
        ECS::EntityHandle Entity; // for cross-rig bootstrap: AddComponent<HumanoidRetargeterComponent>
        uint32 PrevClipIndex = 0;
        float32 PrevTime = 0.0f;
        float32 BlendAlpha = 1.0f;
    };

    std::vector<WorkItem> m_WorkItems;
    std::vector<WorkItem> m_CompactedWorkItems;
    std::unordered_map<uint32, size_t> m_UniqueByRuntimeId;
    PoseSampleWorkspace m_Workspace; // CPU fallback (used until compute shader loads)
    PoseSampleWorkspace m_BlendFrom; // outgoing clip locals during a crossfade

    // Idle-recompute-elision palette signal: the exact inputs every runtime's
    // bone palette was built from last frame. A palette is a pure function of
    // (skeleton, clip, time) on the GPU path, plus (prev clip, blend alpha)
    // on the CPU fade path. The GPU kernel is single-clip; a live fade
    // forces CPU dual-sample and these extra keys keep elision honest.
    // Any delta — time advance, scrub, clip switch, blend weight, runtime
    // added or removed — means palette CONTENT can differ from last
    // frame's, which changes rasterized depth with no other elision key
    // moving (see RenderServices::NotifySkinPaletteContentChanged).
    // Time / blend alpha are compared as raw float bits: exact equality, no
    // epsilons. A paused or run-to-end animator produces byte-identical
    // inputs and stays elidable.
    struct PaletteInputs
    {
        uint32 SkeletonId = 0;
        uint32 ClipIndex = 0;
        uint32 TimeBits = 0;
        uint32 PrevClipIndex = 0;
        uint32 PrevTimeBits = 0;
        uint32 BlendAlphaBits = 0;
    };
    std::unordered_map<uint32, PaletteInputs> m_LastPaletteInputs;

    // Cache of (skeletonId, clipIndex) -> "the GPU compute path produces
    // the correct pose for this pair" — i.e. the skeleton fits the kernel's
    // limits and every channel's targetNameId resolves to the same bone
    // index it has stored. Mismatches force the CPU sample path (which
    // honours name resolution + drop-on-miss). Nothing invalidates it: a
    // model reload refills its skeleton under the same id, so an entry can
    // outlive the data it judged. A reload past the kernel's limits still
    // reaches the CPU path, because GPUAnimationDataStore refuses the
    // re-packed skeleton, and Update logs the limit warning then; a reload
    // back under the limits stays on the CPU path for that pair until the
    // system is rebuilt.
    std::unordered_map<uint64_t, bool> m_GPUSafeCache;

    // Cross-rig humanoid retargeting lives in HumanoidRetargetSystem (Phase
    // 6). It runs immediately after this system in the Animation wave and
    // owns the source-clip sample, RetargetNode dispatch, and GPU prime for
    // entities carrying a HumanoidRetargeterComponent. Same-rig sampling
    // stays here unchanged.

    // Same-rig decisions and failed bootstrap attempts. Successful components
    // stay entity-owned so Play restore can bootstrap them again.
    std::shared_ptr<std::atomic<bool>> m_BootstrapContentDirty = std::make_shared<std::atomic<bool>>(false);
    std::array<AssetReloadInvalidator, 5> m_BootstrapWatchers;
    uint64 m_BootstrapSourceVersion = 0;
    uint64 m_BootstrapWorldId = 0;
    uint64 m_BootstrapWorldReset = 0;
    bool m_BootstrapWatching = false;
    std::unordered_set<uint64_t> m_BootstrapAttempted;
};

// Test hooks: enable unit tests to inject a parsed clip without filesystem/cgltf
namespace TestHooks {
    bool SetClipCacheForTest(const GUID& guid, SharedPtr<AnimationClip> clip);
    void ClearClipCacheForTest();
    // CPU sampling path for tests without ECS or GPU device.
    void SampleByRefsForTest(Components::AnimatorRef& animRef, uint32 skeletonId, float32 deltaTime);
    // Runtime ID most recently created/used by SampleByRefsForTest. Lets tests
    // inspect the sampled pose via SkeletonStore::GetRuntime(). Returns 0 if
    // SampleByRefsForTest has not yet run.
    uint32 GetLastTestRuntimeId();
    // Probe AnimationSystem::IsClipSkeletonGPUSafe without touching the ECS
    // dispatcher — the gate's per-(skel,clip) decision is what tests need to
    // verify. Constructs an internal AnimationSystem on demand; returns true
    // when the GPU compute path can safely consume the pair (the skeleton fits
    // the kernel's limits, and every channel's stored boneIndex points to a
    // bone whose name hashes to its targetNameId, OR the channel is a legacy
    // targetNameId==0 channel).
    bool IsClipSkeletonGPUSafeForTest(uint32 skeletonId, uint32 clipIndex);
}

} } // namespace GameEngine::Engine::Renderer
