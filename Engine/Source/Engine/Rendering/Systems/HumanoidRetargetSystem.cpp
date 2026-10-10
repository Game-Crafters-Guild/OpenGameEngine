#include "ECSModules/Rendering/Systems/HumanoidRetargetSystem.h"

#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "ECS/ECSTemplates.h"

#include "Components/Animation/AnimatorRef.h"
#include "Components/Animation/HumanoidRetargeterComponent.h"
#include "Components/Animation/SkeletonRef.h"

#include "Animation/AnimationPose.h"
#include "Animation/EvaluationContext.h"
#include "Animation/HumanoidRig.h"
#include "Animation/Nodes/RetargetNode.h"
#include "Animation/PoseStack.h"
#include "Animation/PoseToSkinMatrices.h"
#include "Animation/RetargetMap.h"
#include "Animation/SkeletonData.h"
#include "Animation/SkeletonProfile.h"

#include "ECSModules/Rendering/SkeletonStore.h"
#include "ECSModules/Rendering/ClipStore.h"

#include "Engine/Rendering/AnimationSampling.h"
#include "Engine/Rendering/AnimationEventDispatch.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/RetargetGPUDataStore.h"
#include "Engine/Rendering/RetargetRenderFeature.h"
#include "Engine/Rendering/RetargetSubmission.h"
#include "Engine/Rendering/SkinPaletteAtlas.h"

#include "Assets/AnimationClip.h"
#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "AssetCore/GUID.h"
#include "Core/Engine.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <cstring>
#include <string>
#include <unordered_set>

namespace GameEngine { namespace Engine::Renderer {

using namespace GameEngine::Components;
namespace AAnim = ::GameEngine::Animation;

namespace
{

// A source model still loading resolves on a later update; one that is absent,
// is not a model, or imported without a skeleton never will. The caller reports
// only the second case, so a permanently frozen character says why.
struct ResolvedSource
{
    uint32 SkeletonId = 0;
    bool Pending = false;
};

// Clip changes invalidate SourceSkeletonId. A declared model source must be
// resolved again before sampling or publishing a shared GPU rig table; using
// the target as a temporary source permanently seeds that table with the wrong
// bone layout. Source-less procedural / standalone clips retain their explicit
// in-memory source, or the intentional same-rig target fallback.
ResolvedSource ResolveSourceSkeleton(AssetManager* assets, const AnimationClip& clip)
{
    const auto& declared = clip.GetSourcePath();
    const auto& path = declared.empty() ? clip.GetPath() : declared;
    if (!assets || path.empty())
        return {};

    const auto guid = assets->ResolveAssetGuid(path);
    if (guid.IsNull())
        return {};
    auto asset = assets->GetAsset(guid);
    if (!asset)
    {
        // AssetManager owns and coalesces the pending load. Never wait on a
        // decode worker from the simulation wave; the next update retries.
        (void)assets->LoadAssetAsync(guid, AssetLoadPriority::High);
        return {0, true};
    }
    const auto* model = dynamic_cast<const ModelAsset*>(asset.get());
    if (!model)
        return {};
    if (!model->IsLoaded())
        return {0, true};
    return {model->GetSkeletonId(), false};
}

// One-shot per-character diagnostic. Set GE_RETARGET_DIAG=1 to emit a single
// Logger::Log::Info per (runtimeId) that traces:
//   - RetargetNode::Build success and chain count,
//   - first-frame outLocal stats (max rotation off-identity, max translation).
// Used to root-cause cases where the eval reports characters=N but the visible
// mesh stays at bind pose. Off by default; cached after first read.
bool IsRetargetDiagEnabled()
{
    static const bool s_Enabled = []() {
        const char* env = std::getenv("GE_RETARGET_DIAG");
        return env && env[0] != '\0' && env[0] != '0';
    }();
    return s_Enabled;
}

// Trivial AnimGraphNode that just hands a precomputed AnimationPose to its
// caller. Used to feed RetargetNode the per-frame source-clip sample.
class StoredPoseProvider final : public AAnim::AnimGraphNode
{
public:
    AAnim::AnimationPose Pose;
    void Evaluate(AAnim::EvaluationContext& /*ctx*/, AAnim::AnimationPose& outPose) override
    {
        outPose.CopyFrom(Pose);
    }
};

// Convert a PoseSampleWorkspace::TRS array into an AnimationPose. The
// workspace already holds clip-sampled source-skeleton local transforms.
void WorkspaceToAnimationPose(const AAnim::PoseSampleWorkspace& ws,
                              uint32 boneCount,
                              AAnim::AnimationPose& outPose)
{
    outPose.Resize(boneCount);
    if (ws.Pose.size() < boneCount) return;
    for (uint32 i = 0; i < boneCount; ++i)
    {
        const auto& trs = ws.Pose[i];
        outPose.Positions[i] = ::GameEngine::Mathematics::Vector3(trs.t.x, trs.t.y, trs.t.z);
        // glm::quat constructs as (w, x, y, z); GameEngine::Mathematics::Quaternion
        // wraps a glm::quat directly.
        outPose.Rotations[i] = ::GameEngine::Mathematics::Quaternion(trs.r);
        outPose.Scales[i]    = ::GameEngine::Mathematics::Vector3(trs.s.x, trs.s.y, trs.s.z);
    }
}

// Walk the target skeleton's parent table to compose world matrices, then
// emit the compact skin palette via existing BuildPoseToSkinMatrices.
// The retargeted local pose lives in the workspace.Pose array; the helper
// reuses ws's downstream buffers (Local/World/Built/...).
void EmitSkinMatricesFromPose(const AAnim::SkeletonData& skel,
                              const AAnim::AnimationPose& localPose,
                              std::vector<float>& outCompactSkinMatrices,
                              AAnim::PoseSampleWorkspace& ws)
{
    const uint32 boneCount = skel.BoneCount;
    if (boneCount == 0)
    {
        outCompactSkinMatrices.clear();
        return;
    }
    ws.Pose.assign(boneCount, AAnim::PoseSampleWorkspace::TRS{});
    for (uint32 i = 0; i < boneCount && i < localPose.BoneCount; ++i)
    {
        const auto& p = localPose.Positions[i];
        const auto& q = localPose.Rotations[i].GetGLM();
        const auto& s = localPose.Scales[i];
        ws.Pose[i].t = glm::vec3(p.x, p.y, p.z);
        ws.Pose[i].r = q;
        ws.Pose[i].s = glm::vec3(s.x, s.y, s.z);
    }
    // Mark all bones dirty so the world walk recomputes everything.
    ws.ChangedTranslation.assign(boneCount, 1u);
    ws.ChangedRotation.assign(boneCount, 1u);
    ws.ChangedScale.assign(boneCount, 1u);

    AAnim::BuildPoseToSkinMatrices(skel, nullptr, &outCompactSkinMatrices, ws);
}

// Sample clip onto skeleton, write workspace.Pose. Mirrors the relevant
// portion of SampleAnimationPose without going to skin matrices.
void SampleClipOntoSkeleton(const AAnim::SkeletonData& skel,
                            const AnimationClip* clip,
                            float time,
                            AAnim::PoseSampleWorkspace& ws)
{
    SampleAnimationPose(skel, clip, time, nullptr, nullptr, ws);
}

} // namespace

HumanoidRetargetSystem::HumanoidRetargetSystem(RenderServices* renderServices)
    : m_RenderServices(renderServices)
{
    // AssetManager is fetched lazily on first Update so headless test paths
    // that don't initialize EngineCore can still use the TestHooks seam.
}

HumanoidRetargetSystem::~HumanoidRetargetSystem()
{
    m_Watcher.Detach();
}

void HumanoidRetargetSystem::ResetForTest()
{
    m_Characters.clear();
    m_MapCache.clear();
    m_RigCache.clear();
    m_PendingRigReloads.clear();
    m_PendingMapReloads.clear();
    m_RigReloadPending.store(false, std::memory_order_relaxed);
    m_MapReloadPending.store(false, std::memory_order_relaxed);
    m_ProfileReloadPending.store(false, std::memory_order_relaxed);
    m_FirstFrameLogged = false;
}

::GameEngine::Components::HumanoidRetargetLOD
HumanoidRetargetSystem::GetLastLODForRuntime(uint32 runtimeId) const
{
    auto it = m_Characters.find(runtimeId);
    if (it == m_Characters.end())
        return ::GameEngine::Components::HumanoidRetargetLOD::Full;
    return it->second.LastLOD;
}

const AAnim::RetargetMap* HumanoidRetargetSystem::ResolveMap(const ::GameEngine::GUID& mapGuid)
{
    if (mapGuid.IsNull()) return nullptr;
    if (auto it = m_MapCache.find(mapGuid); it != m_MapCache.end())
        return it->second;

    if (!m_AssetManager) return nullptr;
    auto asset = m_AssetManager->GetAsset(mapGuid);
    if (!asset)
    {
        // Async-load issue (Phase 6 doesn't block waiting for IO). The
        // entity will retry on the next tick once the asset arrives in
        // the registry. Asset reload events drop the cache entry too so
        // a re-resolve picks up the fresh pointer.
        m_AssetManager->LoadAsset(mapGuid, ::GameEngine::AssetLoadResultCallback{});
        asset = m_AssetManager->GetAsset(mapGuid);
    }
    if (!asset) return nullptr;
    auto* map = dynamic_cast<const AAnim::RetargetMap*>(asset.get());
    if (!map) return nullptr;
    m_MapCache.emplace(mapGuid, map);
    return map;
}

const AAnim::HumanoidRig* HumanoidRetargetSystem::ResolveRig(const ::GameEngine::GUID& rigGuid)
{
    if (rigGuid.IsNull()) return nullptr;
    if (auto it = m_RigCache.find(rigGuid); it != m_RigCache.end())
        return it->second;

    if (!m_AssetManager) return nullptr;
    auto asset = m_AssetManager->GetAsset(rigGuid);
    if (!asset)
    {
        m_AssetManager->LoadAsset(rigGuid, ::GameEngine::AssetLoadResultCallback{});
        asset = m_AssetManager->GetAsset(rigGuid);
    }
    if (!asset)
    {
        if (IsRetargetDiagEnabled())
        {
            static thread_local std::unordered_set<std::string> s_Logged;
            const std::string key = rigGuid.ToString();
            if (s_Logged.insert(key).second)
                Logger::Log::Warning(
                    "[RetargetDiag] ResolveRig: AssetManager has no asset for guid={}", key);
        }
        return nullptr;
    }
    auto* rig = dynamic_cast<const AAnim::HumanoidRig*>(asset.get());
    if (!rig)
    {
        if (IsRetargetDiagEnabled())
        {
            static thread_local std::unordered_set<std::string> s_Logged;
            const std::string key = rigGuid.ToString();
            if (s_Logged.insert(key).second)
                Logger::Log::Warning(
                    "[RetargetDiag] ResolveRig: dynamic_cast<HumanoidRig*> failed for guid={} (asset type mismatch)",
                    key);
        }
        return nullptr;
    }
    m_RigCache.emplace(rigGuid, rig);
    return rig;
}

void HumanoidRetargetSystem::DrainHotReloadEvents()
{
    if (!m_RigReloadPending.load(std::memory_order_acquire) &&
        !m_MapReloadPending.load(std::memory_order_acquire) &&
        !m_ProfileReloadPending.load(std::memory_order_acquire))
        return;

    std::vector<::GameEngine::GUID> rigs;
    std::vector<::GameEngine::GUID> maps;
    {
        std::lock_guard<std::mutex> lk(m_PendingReloadMutex);
        rigs.swap(m_PendingRigReloads);
        maps.swap(m_PendingMapReloads);
    }

    // Drop cached pointers and force per-character rebuild on the next
    // Update tick. Update runs this drain before its per-character loop, on
    // the same thread, so no entry is read while its pointers change; the
    // event callbacks only queue GUIDs under m_PendingReloadMutex.
    if (m_ProfileReloadPending.exchange(false, std::memory_order_acq_rel))
    {
        // Profile changes invalidate every rig that referenced it; we
        // can't tell from here which rig pointed at which profile, so
        // drop the rig cache wholesale and force every character to
        // rebuild on next access.
        m_RigCache.clear();
        for (auto& [_, entry] : m_Characters)
            entry.MapDirty = true;
    }

    for (const auto& g : rigs) m_RigCache.erase(g);
    for (const auto& g : maps) m_MapCache.erase(g);

    if (m_RigReloadPending.exchange(false, std::memory_order_acq_rel) ||
        m_MapReloadPending.exchange(false, std::memory_order_acq_rel) ||
        !rigs.empty() || !maps.empty())
    {
        // The events also cover an asset leaving memory (unloaded or
        // destroyed); one that comes back is a new object under the same
        // GUID. The cached pointers are dropped with the caches, so the next
        // tick re-resolves them instead of reading the object that left.
        for (auto& [_, entry] : m_Characters)
        {
            if (!entry.MapGUID.IsNull() &&
                std::find(maps.begin(), maps.end(), entry.MapGUID) != maps.end())
            {
                entry.Map = nullptr;
                entry.SourceRig = nullptr;
                entry.TargetRig = nullptr;
                entry.MapDirty = true;
            }
            // Rig reloads invalidate any character whose source/target rig
            // references match. We don't track per-character rig GUIDs
            // separately yet; drop every character's rigs when ANY rig reload
            // arrived.
            if (!rigs.empty())
            {
                entry.SourceRig = nullptr;
                entry.TargetRig = nullptr;
                entry.MapDirty = true;
            }
        }
    }
}

bool HumanoidRetargetSystem::PrimeGPU(CharacterEntry& /*entry*/, float32 /*clipTime*/)
{
    // GPU dispatch is dormant during the rewrite (Phase 1c). The old
    // ReserveCharacter / WriteSourcePose API is being replaced by
    // EnsureClipUploaded / EnsureRigPairUploaded + per-frame characterParams
    // in Phase 1c.2. For now: no-op; CPU eval below is the only producer.
    return false;
}

void HumanoidRetargetSystem::Update(ECS::World& world, float32 deltaTime)
{
    // Resolve AssetManager + GPU store lazily on the first tick. The lookup
    // is cheap on subsequent frames (single load).
    if (!m_AssetManager)
    {
        if (auto& core = ::GameEngine::EngineCore::GetInstance(); core.IsInitialized())
            m_AssetManager = &core.GetAssetManager();
    }

    // Phase 11: subscribe the RetargetAssetWatcher to the AssetManager's
    // event dispatcher on the first tick where AssetManager is live. Without
    // this, hand-edits to *.humanoidrig.json / *.retargetmap.json /
    // *.profile.json never invalidate the per-character cache and the
    // running editor keeps using stale bake state. The Attach is idempotent;
    // calling twice (e.g. after AssetManager refresh) just re-installs the
    // invalidator triplet.
    if (m_AssetManager && !m_Watcher.IsAttached())
    {
        m_Watcher.Attach(m_AssetManager->GetEventDispatcher());
        m_Watcher.OnRigReloaded([this](const ::GameEngine::GUID& g) {
            std::lock_guard<std::mutex> lk(m_PendingReloadMutex);
            m_PendingRigReloads.push_back(g);
            m_RigReloadPending.store(true, std::memory_order_release);
        });
        m_Watcher.OnMapReloaded([this](const ::GameEngine::GUID& g) {
            std::lock_guard<std::mutex> lk(m_PendingReloadMutex);
            m_PendingMapReloads.push_back(g);
            m_MapReloadPending.store(true, std::memory_order_release);
        });
        m_Watcher.OnProfileReloaded([this](const ::GameEngine::GUID&) {
            // Profile reload invalidates all rigs that referenced it; the
            // drain code clears m_RigCache wholesale on this flag.
            m_ProfileReloadPending.store(true, std::memory_order_release);
        });
    }

    // Phase 14 — connect to RetargetRenderFeature's data store. The feature
    // is constructed on first request (lazy init creates the SSBOs + loads
    // SPIR-V); m_GPUStore stays nullptr in headless test paths where
    // RenderServices is null. Otherwise it points at a live store every frame.
    RetargetRenderFeature* retargetFeat = nullptr;
    if (m_RenderServices)
    {
        retargetFeat = &m_RenderServices->GetRetargetRenderFeature();
        m_GPUStore = retargetFeat->IsInitialized() ? &retargetFeat->GetDataStore() : nullptr;
        // BeginFrame is called once per frame from RenderingLoop, before any
        // system runs. Don't call it here — re-entry would clear the
        // m_HandledRuntimes set mid-frame and cause SkinningUploadSystem to
        // double-write atlas slots.
    }
    else
    {
        m_GPUStore = nullptr;
    }

    // Attach the GPU store's asset-unload watchers once both the AssetManager
    // and the store are alive. Without this the store's GUID -> headerIdx
    // tables accumulate dead entries every time a clip / retargetmap unloads
    // (scene switch, asset eviction). The store's ReleaseClip / ReleaseRigPair
    // are the targets; the AssetReloadInvalidator inside it owns RAII detach
    // on shutdown so we don't leak a callback when the store dies.
    if (m_GPUStore && m_AssetManager && !m_GPUStoreAssetEventsAttached)
    {
        m_GPUStore->AttachAssetEvents(m_AssetManager->GetEventDispatcher());
        m_GPUStoreAssetEventsAttached = true;
    }

    DrainHotReloadEvents();

    // Prune CharacterEntry rows whose target runtime no longer exists.
    // Without this the m_Characters map grows unbounded across entity destroy
    // / spawn cycles. The runtime itself is freed by the SkeletonRef removal
    // hook the rendering registrar installs (RenderWorldHooks.cpp), so
    // detecting orphaned entries here just means asking SkeletonStore whether
    // the runtime still exists.
    //
    // No GPU-side release is needed at this site:
    //   * SkinPaletteAtlas slots are per-frame transient (PerFrameWritePool
    //     resets every frame), not persistent — there's nothing to free.
    //   * RetargetGPUDataStore clip / rigPair entries are keyed by ASSET GUID,
    //     not by character. A clip used by two characters has a single header;
    //     freeing it on entity destroy would break the other character.
    //     Asset-scoped release is wired separately via AttachAssetEvents +
    //     ReleaseClip / ReleaseRigPair on AssetUnloaded events.
    {
        auto& skStore = SkeletonStore::Instance();
        for (auto it = m_Characters.begin(); it != m_Characters.end();)
        {
            const uint32 runtimeId = it->first;
            if (skStore.GetRuntime(runtimeId) == nullptr)
                it = m_Characters.erase(it);
            else
                ++it;
        }
    }

    auto& clipStore = ClipStore::Instance();
    auto& skStore = SkeletonStore::Instance();

    const bool cpuOnly = IsRetargetCPUOnly();

    RetargetFrameStats stats{};
    const auto frameStart = std::chrono::steady_clock::now();

    // One source lookup per distinct explicit clip, not per actor. Keep clip
    // ownership within this update so replacement of a ClipStore slot cannot
    // alias a cached identity: the guard releases every reference on both the
    // normal and the exception path, so the vector is empty on entry.
    struct ClearFrameSources
    {
        std::vector<SourceResolution>& Entries;
        ~ClearFrameSources() { Entries.clear(); }
    } clearFrameSources{m_FrameSources};
    m_FramePaletteInputs.clear();

    // Phase 2c: per-frame collector for GPU character params. Populated
    // inside the q.Each lambda for every successfully-built CpuNode; flushed
    // to the data store once after the loop. The GPU dispatch is gated on
    // the env-var inside RetargetRenderFeature::BuildPasses; here we just
    // make sure the data is fresh in case it does fire.
    std::vector<GPURetargetCharacterParams> gpuCharParams;
    if (m_GPUStore) gpuCharParams.reserve(64);

    auto q = world.Query<ECS::Write<HumanoidRetargeterComponent>, ECS::Read<SkeletonRef>>();
    q.Each([&](ECS::EntityHandle entity, HumanoidRetargeterComponent& rc, const SkeletonRef& sref) {
        // Sync clip selection from AnimatorRef. AnimationSystem skips entities
        // that already have HumanoidRetargeterComponent (~line 263), so clip
        // changes go through here. AnimatorRef is the source of truth: copy
        // ClipIndex and Time, and drop SourceSkeletonId so the source rig
        // re-resolves when the clip changes.
        bool tickedFromAnimatorRef = false;
        if (auto* anim = world.GetComponentForWrite<::GameEngine::Components::AnimatorRef>(entity))
        {
            if (anim->ClipIndex == 0)
            {
                rc.SourceClipIndex = 0;
                rc.ClipTimeSeconds = 0.0f;
                TickAnimatorRef(*anim, clipStore, deltaTime, nullptr);
                return;
            }
            if (anim->ClipIndex != rc.SourceClipIndex)
            {
                rc.SourceClipIndex   = anim->ClipIndex;
                rc.ClipTimeSeconds   = anim->Time;
                rc.SourceSkeletonId  = 0; // re-resolved below from clip's source model
            }
            // Mirror the playback knobs so the editor's Animator inspector
            // (Speed, Loop, Paused) drives retarget playback the same way it
            // drives the non-retarget AnimationSystem path.
            rc.Speed  = anim->Speed;
            rc.Loop   = anim->IsLooping();
            rc.Paused = anim->IsPaused();
            TickAnimatorRef(*anim, clipStore, deltaTime, ClaimClipPlaybackEvents(world, entity));
            rc.ClipTimeSeconds = anim->Time;
            tickedFromAnimatorRef = true;
        }

        if (rc.SourceClipIndex == 0 || sref.skeletonId == 0 || sref.runtimeId == 0) return;
        if (rc.Map.IsNull()) return;

        // Phase 8 LOD selection. Defaults to Full unless the component
        // overrides; a distance/on-screen signal could plug in here from the
        // GPU visibility pipeline if retarget cost ever warrants it.
        ::GameEngine::Components::HumanoidRetargetLOD lod =
            ::GameEngine::Components::HumanoidRetargetLOD::Full;
        if (rc.LODOverrideEnabled)
            lod = rc.LODOverride;
        else if (!rc.IKEnabled)
            lod = ::GameEngine::Components::HumanoidRetargetLOD::FKOnly;

        auto clip = clipStore.Get(rc.SourceClipIndex);
        if (!clip) return;

        auto* tgtSkel = skStore.Get(sref.skeletonId);
        if (!tgtSkel || tgtSkel->BoneCount == 0) return;

        // Time advance. AnimatorRef already ran TickAnimatorRef above.
        if (!tickedFromAnimatorRef && !rc.Paused)
        {
            const float frameSeconds = std::isfinite(deltaTime) ? std::max(0.0f, deltaTime) : 0.0f;
            const float speed = std::isfinite(rc.Speed) ? std::max(0.0f, rc.Speed) : 0.0f;
            const float time = std::isfinite(rc.ClipTimeSeconds) ? rc.ClipTimeSeconds : 0.0f;
            double advancedTime = static_cast<double>(time) + static_cast<double>(frameSeconds) * speed;
            const float duration = clip->GetDuration();
            if (std::isfinite(duration) && duration > 0.0f)
            {
                advancedTime = rc.Loop ? WrapClipTime(advancedTime, 0.0f, duration)
                                       : std::min(advancedTime, static_cast<double>(duration));
            }
            rc.ClipTimeSeconds = static_cast<float>(std::min(advancedTime,
                static_cast<double>(std::numeric_limits<float>::max())));
        }

        // Mirror the advanced time back to AnimatorRef so the timeline UI,
        // scripting layer, and any non-retarget observer sees playback
        // progress. AnimationSystem skips this entity (HasComponent guard),
        // so without this AnimatorRef.Time would stay frozen at bootstrap.
        if (auto* anim = world.GetComponentForWrite<::GameEngine::Components::AnimatorRef>(entity))
            anim->Time = rc.ClipTimeSeconds;

        auto& entry = m_Characters[sref.runtimeId];
        const bool mapChanged = (entry.MapGUID != rc.Map.ToGuid());
        if (mapChanged)
        {
            entry.MapGUID = rc.Map.ToGuid();
            entry.Map = nullptr;
            entry.SourceRig = nullptr;
            entry.TargetRig = nullptr;
            entry.MapDirty = true;
            entry.CpuNode.reset();
            entry.CpuNodeBuilt = false;
        }
        entry.TargetRuntimeId = sref.runtimeId;
        entry.TargetSkeletonId = sref.skeletonId;

        // PoseHold short-circuit. The cached SkeletonRuntimeState already
        // holds last frame's CompactSkinMatrices; the rasterizer reads the
        // existing palette for free. Only count the character in the LOD
        // distribution and bail. The transition-back case (PoseHold -> any
        // active level) is handled below by clearing LastLOD so the next
        // frame falls through to a full evaluate even if the level changed.
        ++stats.TotalCharacters;
        if (lod == ::GameEngine::Components::HumanoidRetargetLOD::PoseHold)
        {
            ++stats.LODPoseHoldCount;
            entry.LastLOD = lod;
            m_FramePaletteInputs.push_back(entry.EvaluatedInputs);
            return;
        }

        if (!entry.Map)
        {
            entry.Map = ResolveMap(rc.Map.ToGuid());
            if (!entry.Map) return;
        }
        // Retry rig resolution every frame while either side is null.
        // Pre-fix this only ran once inside the `if (!entry.Map)` block, so
        // a transient AssetManager miss on the very first frame after
        // bootstrap left both rig pointers null permanently — silently
        // freezing the entity at bind pose. Now we retry every frame; once
        // the bootstrap registers the rigs (see AnimationSystem auto-
        // bootstrap fixes), the lookup eventually succeeds and the system
        // self-heals without needing a re-bootstrap.
        if (!entry.SourceRig)
            entry.SourceRig = ResolveRig(entry.Map->SourceRigRef());
        if (!entry.TargetRig)
            entry.TargetRig = ResolveRig(entry.Map->TargetRigRef());
        if (!entry.SourceRig || !entry.TargetRig)
        {
            // Rig load is async-friendly — return and retry next frame. The
            // RetargetDiag-gated warning surfaces persistent failures (rig
            // sidecar missing or mis-registered with AssetManager).
            if (IsRetargetDiagEnabled())
            {
                static thread_local std::unordered_set<uint32> s_LoggedRigFail;
                if (s_LoggedRigFail.insert(sref.runtimeId).second)
                {
                    Logger::Log::Warning(
                        "[RetargetDiag] rt={} rig resolve failed src={} tgt={} mapValid={}",
                        sref.runtimeId,
                        entry.SourceRig ? "OK" : "NULL",
                        entry.TargetRig ? "OK" : "NULL",
                        entry.Map ? "OK" : "NULL");
                }
            }
            return;
        }

        uint32 desiredSrcId = 0;
        const bool modelSource = !clip->GetSourcePath().empty() ||
                                 GetAssetTypeFromExtension(clip->GetPath().extension().string()) == AssetType::Model;
        if (modelSource)
        {
            auto found = std::find_if(m_FrameSources.begin(), m_FrameSources.end(),
                                      [&](const SourceResolution& source)
                                      { return source.Clip.get() == clip.get(); });
            if (found == m_FrameSources.end())
            {
                const auto resolved = ResolveSourceSkeleton(m_AssetManager, *clip);
                m_FrameSources.push_back({clip, resolved.SkeletonId, resolved.Pending});
                found = std::prev(m_FrameSources.end());
            }
            desiredSrcId = found->SkeletonId;
            if (desiredSrcId != 0)
                entry.SourceUnusableLogged = false;
            else if (!found->Pending && !entry.SourceUnusableLogged)
            {
                entry.SourceUnusableLogged = true;
                const auto& sourcePath =
                    clip->GetSourcePath().empty() ? clip->GetPath() : clip->GetSourcePath();
                Logger::Log::Warning(
                    "[HumanoidRetargetSystem] rt={} cannot read a source skeleton from animation "
                    "model '{}' (missing, not a model, or imported without a skeleton); the "
                    "character holds its last pose until that model resolves",
                    sref.runtimeId, sourcePath.string());
            }
        }
        else
            desiredSrcId = rc.SourceSkeletonId ? rc.SourceSkeletonId : sref.skeletonId;
        auto* srcSkel = skStore.Get(desiredSrcId);
        rc.SourceSkeletonId = srcSkel ? desiredSrcId : 0;
        if (!srcSkel)
            return;
        if (entry.SourceSkeletonId != desiredSrcId)
        {
            entry.SourceSkeletonId = desiredSrcId;
            entry.CpuNodeBuilt = false; // force rebuild against new source
        }

        // Sample clip onto source skeleton's local pose. A live two-slot
        // blend samples the outgoing clip onto a retained workspace and
        // lerps source locals before retarget — one RetargetNode pass.
        const auto sampleStart = std::chrono::steady_clock::now();
        SampleClipOntoSkeleton(*srcSkel, clip.get(), rc.ClipTimeSeconds, m_SourceSampleWorkspace);
        const Animation::AnimationClip* blendPrevClip = nullptr;
        uint32 blendPrevClipIndex = 0;
        float32 blendPrevTime = 0.0f;
        float32 blendAlpha = 0.0f;
        if (auto* anim = world.GetComponent<::GameEngine::Components::AnimatorRef>(entity))
        {
            if (anim->IsBlending())
            {
                if (auto prev = clipStore.Get(anim->PrevClipIndex))
                {
                    blendPrevClip = prev.get();
                    blendPrevClipIndex = anim->PrevClipIndex;
                    blendPrevTime = anim->PrevTime;
                    blendAlpha = anim->BlendAlpha();
                    SampleClipOntoSkeleton(*srcSkel, prev.get(), blendPrevTime, m_BlendFromWorkspace);
                    BlendLocalPoses(m_SourceSampleWorkspace, m_BlendFromWorkspace, blendAlpha);
                }
            }
        }
        stats.CpuSampleNanos += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - sampleStart).count());

        // GPU dispatch is dormant during the rewrite (Phase 1c removed all
        // pre-25.4 GPU passes; Phase 2 lands the new fused retarget_full.comp).
        // CPU evaluation below is the only producer of CompactSkinMatrices
        // until then. cpuOnly is preserved as a future toggle.
        (void)cpuOnly;
        (void)retargetFeat;

        // CPU evaluation through RetargetNode.
        if (!entry.CpuNode)
            entry.CpuNode = std::make_unique<AAnim::RetargetNode>();

        auto* tgtSkelMut = skStore.Get(sref.skeletonId);
        if (!tgtSkelMut) return;

        if (!entry.CpuNodeBuilt || entry.MapDirty)
        {
            // Configure: source provider feeds the sampled pose; rigs +
            // map come from cached pointers. Build sets up bake-rewrite
            // and chain dispatch tables.
            StoredPoseProvider unusedDuringBuild;
            entry.CpuNode->Configure(/*source=*/&unusedDuringBuild,
                                     entry.SourceRig, entry.TargetRig, entry.Map);
            entry.CpuNodeBuilt = entry.CpuNode->Build(*srcSkel, *tgtSkelMut);
            ++entry.BuildGeneration;
            entry.MapDirty = false;
            entry.DiagBuildLogged = false;
        }
        if (!entry.CpuNodeBuilt)
        {
            if (IsRetargetDiagEnabled() && !entry.DiagBuildLogged)
            {
                entry.DiagBuildLogged = true;
                Logger::Log::Warning(
                    "[RetargetDiag] rt={} BUILD FAILED: srcSkelId={} (bones={}) tgtSkelId={} (bones={})",
                    sref.runtimeId, entry.SourceSkeletonId, srcSkel->BoneCount,
                    sref.skeletonId, tgtSkelMut->BoneCount);
            }
            return;
        }
        if (IsRetargetDiagEnabled() && !entry.DiagBuildLogged)
        {
            entry.DiagBuildLogged = true;
            const auto* node = entry.CpuNode.get();
            const size_t chainCount = node ? node->ChainCount() : 0;
            std::string chainSummary;
            for (size_t c = 0; c < chainCount && c < 8; ++c)
            {
                if (!chainSummary.empty()) chainSummary += ", ";
                chainSummary += "src=";
                chainSummary += std::to_string(node->ChainSourceBoneCount(c));
                chainSummary += "/tgt=";
                chainSummary += std::to_string(node->ChainTargetBoneCount(c));
            }
            Logger::Log::Info(
                "[RetargetDiag] rt={} BUILT: srcSkelId={} (bones={}) tgtSkelId={} (bones={}) chains={} [{}]",
                sref.runtimeId, entry.SourceSkeletonId, srcSkel->BoneCount,
                sref.skeletonId, tgtSkelMut->BoneCount,
                chainCount, chainSummary);
        }

        // Phase 2c+3a: GPU asset-load uploads (idempotent; per-asset GUID
        // cache returns immediately on hit) + atlas reservation + offset
        // publishing. Reserves a SkinPaletteAtlas slot here BEFORE
        // RenderExtractionSystem builds the GPUInstance data (which reads
        // runtime->AtlasPaletteOffsetBones). Marks the runtime as GPU-handled
        // so SkinningUploadSystem skips its CPU upload of the same slot.
        // GPU upload + atlas reservation. Reserve is sequential — the
        // SkinPaletteAtlas allocator is non-atomic and the q.Each iteration
        // is single-threaded; switching to Parallel would corrupt offsets.
        if (m_GPUStore && m_RenderServices)
        {
            auto* runtime = skStore.GetRuntime(sref.runtimeId);
            if (runtime)
            {
                // FBX-imported skeletons sometimes carry SkinJointCount=0 and
                // an empty JointNodes — treat that as "joint == bone" identity
                // mapping with BoneCount palette entries (matches the CPU
                // BuildPoseToSkinMatrices fallback). Upload + Reserve must
                // both use this corrected count or the GPU writes 0 joints
                // into a slot reserved for BoneCount, leaving the mesh at
                // bind pose.
                const uint32_t skinJointCount = (tgtSkelMut->SkinJointCount > 0)
                    ? tgtSkelMut->SkinJointCount : tgtSkelMut->BoneCount;

                const uint32_t rigPairIdx = m_GPUStore->EnsureRigPairUploaded(
                    rc.Map.ToGuid(), *entry.CpuNode, *srcSkel, *tgtSkelMut, skinJointCount);
                const uint32_t clipIdx = rigPairIdx != kRetargetInvalidIndex
                    ? m_GPUStore->EnsureClipUploaded(clip->GetGUID(), *clip)
                    : kRetargetInvalidIndex;
                // A crossfade's outgoing clip, blended on the GPU as the CPU
                // path blends it above. An upload failure drops the blend, not
                // the character.
                const uint32_t prevClipIdx = (clipIdx != kRetargetInvalidIndex && blendPrevClip)
                    ? m_GPUStore->EnsureClipUploaded(blendPrevClip->GetGUID(), *blendPrevClip)
                    : kRetargetInvalidIndex;

                // Only call atlas.Reserve once both uploads succeeded — a
                // failed upload (asset error) doesn't burn an atlas slot
                // every frame.
                if (rigPairIdx != kRetargetInvalidIndex && clipIdx != kRetargetInvalidIndex)
                {
                    auto& atlas = m_RenderServices->GetSkinPaletteAtlas();
                    const uint32_t paletteOffset = atlas.Reserve(skinJointCount);

                    if (paletteOffset != UINT32_MAX)
                    {
                        runtime->SetAtlasPaletteOffset(paletteOffset);
                        m_GPUStore->RegisterHandledRuntime(sref.runtimeId);

                        GPURetargetCharacterParams cp{};
                        cp.rigPairHeaderIdx = rigPairIdx;
                        cp.clipHeaderIdx    = clipIdx;
                        cp.clipTime         = rc.ClipTimeSeconds;
                        cp.paletteOffset    = paletteOffset;
                        if (prevClipIdx != kRetargetInvalidIndex)
                        {
                            cp.prevClipHeaderIdx = prevClipIdx;
                            cp.prevClipTime      = blendPrevTime;
                            cp.blendAlpha        = blendAlpha;
                        }
                        gpuCharParams.push_back(cp);
                    }
                    else
                    {
                        // Refused: retire the offset to the identity block so
                        // this frame renders bind pose. Keeping the previous
                        // value would skin the entity with whichever runtime
                        // now owns that slot.
                        runtime->SetAtlasPaletteOffset(
                            SkinPaletteAtlas::kIdentityPaletteOffsetBones);
                    }
                }
            }
        }

        // Build the source-pose AnimationPose from the sampled workspace.
        StoredPoseProvider provider;
        WorkspaceToAnimationPose(m_SourceSampleWorkspace, srcSkel->BoneCount, provider.Pose);

        // Phase 12 (audit §1.1): reuse the per-CharacterEntry PoseStack +
        // OutLocalPose across frames. Reserve / Resize are no-ops once the
        // capacity matches; we only pay for storage growth on rig
        // hot-reload (rare). This eliminates 2 allocs/character/frame —
        // ~6 000 allocs/sec at 100 chars × 60 Hz.
        AAnim::PoseStack& scratch = entry.ScratchStack;
        scratch.Reserve(std::max(srcSkel->BoneCount, tgtSkelMut->BoneCount));

        AAnim::EvaluationContext ctx;
        ctx.SourceSkeleton = srcSkel;
        ctx.TargetSkeleton = tgtSkelMut;
        ctx.ScratchStack   = &scratch;
        ctx.DeltaTime      = deltaTime;

        AAnim::AnimationPose& outLocal = entry.OutLocalPose;
        outLocal.Resize(tgtSkelMut->BoneCount);

        // Phase 20: bind the live source provider WITHOUT clobbering the
        // cached Build state. RetargetNode::Configure resets m_Built=false,
        // so calling it every frame (just to swap in the local provider)
        // caused Evaluate to take its !m_Built early-return path — leaving
        // outLocal at default-constructed values (identity rotations, zero
        // positions). Skin matrices then collapsed every vertex to origin
        // and the mesh rendered invisible. SetSourceProvider rebinds only
        // the pointer.
        entry.CpuNode->SetSourceProvider(&provider);

        // Phase 3d: skip the per-frame CPU evaluation + palette emit when
        // the GPU is the authoritative producer (the GPU compute pass writes
        // the atlas directly via runtime->AtlasPaletteOffsetBones reserved
        // above). The CpuNode->Build call earlier is still required because
        // EnsureRigPairUploaded reads its post-Build state for asset upload.
        // Forcing the CPU path back on is `GE_RETARGET_CPU=1` (set before
        // process start; cached in IsRetargetCPUOnly()).
        const bool gpuOwnsAtlas = m_GPUStore && !cpuOnly &&
                                   m_GPUStore->IsRetargetGPUHandled(sref.runtimeId);
        if (!gpuOwnsAtlas)
        {
            const auto evalStart = std::chrono::steady_clock::now();
            entry.CpuNode->Evaluate(ctx, outLocal);
            stats.CpuEvaluateNanos += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - evalStart).count());

            auto* runtime = skStore.GetRuntime(sref.runtimeId);
            if (!runtime)
            {
                if (IsRetargetDiagEnabled() && !entry.DiagEvalLogged)
                {
                    entry.DiagEvalLogged = true;
                    Logger::Log::Warning(
                        "[RetargetDiag] rt={} EVAL: skStore.GetRuntime returned null — palette write skipped",
                        sref.runtimeId);
                }
                return;
            }

            if (IsRetargetDiagEnabled() && !entry.DiagEvalLogged)
            {
                entry.DiagEvalLogged = true;
                float maxRotDelta = 0.0f;
                float maxTransMag = 0.0f;
                uint32_t firstNonIdentBone = UINT32_MAX;
                for (uint32 b = 0; b < outLocal.BoneCount; ++b)
                {
                    const auto& q = outLocal.Rotations[b].GetGLM();
                    const float r = std::max({std::fabs(q.x), std::fabs(q.y), std::fabs(q.z)});
                    const auto& p = outLocal.Positions[b];
                    const float t = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
                    if (r > maxRotDelta) maxRotDelta = r;
                    if (t > maxTransMag) maxTransMag = t;
                    if (firstNonIdentBone == UINT32_MAX && (r > 1e-4f || t > 1e-4f))
                        firstNonIdentBone = b;
                }
                Logger::Log::Info(
                    "[RetargetDiag] rt={} EVAL: outLocal bones={} maxRotDelta={:.4f} maxTransMag={:.4f} firstNonIdentBone={}",
                    sref.runtimeId, outLocal.BoneCount, maxRotDelta, maxTransMag,
                    firstNonIdentBone == UINT32_MAX ? -1 : static_cast<int>(firstNonIdentBone));
            }

            const auto paletteStart = std::chrono::steady_clock::now();
            EmitSkinMatricesFromPose(*tgtSkelMut, outLocal,
                                      runtime->CompactSkinMatrices, m_TargetPoseWorkspace);
            stats.CpuPaletteEmitNanos += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - paletteStart).count());
        }

        switch (lod)
        {
        case ::GameEngine::Components::HumanoidRetargetLOD::Full:
            ++stats.LODFullCount;
            break;
        case ::GameEngine::Components::HumanoidRetargetLOD::NoOps:
            ++stats.LODNoOpsCount;
            break;
        case ::GameEngine::Components::HumanoidRetargetLOD::FKOnly:
            ++stats.LODFKOnlyCount;
            break;
        case ::GameEngine::Components::HumanoidRetargetLOD::PoseHold:
            // Already handled above.
            break;
        }
        entry.LastLOD = lod;
        entry.EvaluatedInputs = PaletteInputs{
            sref.runtimeId,
            sref.skeletonId,
            entry.SourceSkeletonId,
            entry.BuildGeneration,
            entry.MapGUID,
            clip.get(),
            rc.SourceClipIndex,
            std::bit_cast<uint32>(rc.ClipTimeSeconds),
            blendPrevClip,
            blendPrevClipIndex,
            std::bit_cast<uint32>(blendPrevTime),
            std::bit_cast<uint32>(blendAlpha),
            lod};
        m_FramePaletteInputs.push_back(entry.EvaluatedInputs);
    });

    (void)frameStart;
    WriteFrameStats(stats);

    // Idle-recompute-elision: report palette content only when this update's
    // input records differ from the last update's (m_LastPaletteInputs
    // contract in the header). The retarget evaluation reads no IK targets or
    // other transforms, so a paused character compares equal and the
    // depth-derived passes settle.
    if (m_FramePaletteInputs != m_LastPaletteInputs)
    {
        m_LastPaletteInputs.swap(m_FramePaletteInputs);
        if (m_RenderServices)
            m_RenderServices->NotifySkinPaletteContentChanged(world.GetWorldId());
    }

    // Append this system instance's per-frame character params to the GPU
    // data store. Multiple HumanoidRetargetSystem instances (main scene
    // wave + each thumbnail world's TickThumbnailSystems) contribute
    // additively into a single shared dispatch. RetargetGPUDataStore
    // clears the list at BeginFrame, then each instance appends; replacing
    // would race (last-writer wins, others get bind pose -> staggering).
    if (m_GPUStore)
        m_GPUStore->AppendCharacterParams(gpuCharParams);

    if (!m_FirstFrameLogged && !m_Characters.empty())
    {
        Logger::Log::Info("[HumanoidRetargetSystem] First retargeted frame: characters={} cpuOnly={}",
                          static_cast<unsigned>(m_Characters.size()), cpuOnly ? 1 : 0);
        m_FirstFrameLogged = true;
    }
}

// ===== Test Hooks Implementations =====
namespace TestHooks
{

bool RunCPUEvaluateForTest(const AAnim::RetargetMap& map,
                           const AAnim::HumanoidRig& sourceRig,
                           const AAnim::HumanoidRig& targetRig,
                           uint32 sourceClipIndex,
                           uint32 sourceSkeletonId,
                           uint32 targetSkeletonId,
                           uint32 targetRuntimeId,
                           float32 clipTime)
{
    auto& clipStore = ClipStore::Instance();
    auto& skStore = SkeletonStore::Instance();

    auto clip = clipStore.Get(sourceClipIndex);
    auto* srcSkel = skStore.Get(sourceSkeletonId);
    auto* tgtSkel = skStore.Get(targetSkeletonId);
    auto* runtime = skStore.GetRuntime(targetRuntimeId);
    if (!clip || !srcSkel || !tgtSkel || !runtime) return false;

    static AAnim::PoseSampleWorkspace s_SrcWs;
    static AAnim::PoseSampleWorkspace s_TgtWs;

    SampleClipOntoSkeleton(*srcSkel, clip.get(), clipTime, s_SrcWs);

    StoredPoseProvider provider;
    WorkspaceToAnimationPose(s_SrcWs, srcSkel->BoneCount, provider.Pose);

    AAnim::RetargetNode node;
    node.Configure(&provider, &sourceRig, &targetRig, &map);
    if (!node.Build(*srcSkel, *tgtSkel)) return false;

    AAnim::PoseStack scratch;
    scratch.Reserve(std::max(srcSkel->BoneCount, tgtSkel->BoneCount));

    AAnim::EvaluationContext ctx;
    ctx.SourceSkeleton = srcSkel;
    ctx.TargetSkeleton = tgtSkel;
    ctx.ScratchStack   = &scratch;
    ctx.DeltaTime      = 0.0f;

    AAnim::AnimationPose outLocal;
    outLocal.Resize(tgtSkel->BoneCount);
    node.Evaluate(ctx, outLocal);

    EmitSkinMatricesFromPose(*tgtSkel, outLocal, runtime->CompactSkinMatrices, s_TgtWs);
    return true;
}

bool RunCPUEvaluateAtLODForTest(const AAnim::RetargetMap& map,
                                const AAnim::HumanoidRig& sourceRig,
                                const AAnim::HumanoidRig& targetRig,
                                uint32 sourceClipIndex,
                                uint32 sourceSkeletonId,
                                uint32 targetSkeletonId,
                                uint32 targetRuntimeId,
                                float32 clipTime,
                                ::GameEngine::Components::HumanoidRetargetLOD lodLevel)
{
    using LOD = ::GameEngine::Components::HumanoidRetargetLOD;
    if (lodLevel == LOD::PoseHold)
    {
        // Honor the contract: caller wants PoseHold; do no work and report
        // success. Existing CompactSkinMatrices stay resident.
        auto* runtime = SkeletonStore::Instance().GetRuntime(targetRuntimeId);
        return runtime != nullptr;
    }
    return RunCPUEvaluateForTest(map, sourceRig, targetRig,
                                 sourceClipIndex, sourceSkeletonId,
                                 targetSkeletonId, targetRuntimeId, clipTime);
}

} // namespace TestHooks

} } // namespace GameEngine::Engine::Renderer
