#include "Inspectors/AnimationPreviewManager.h"
#include "PlayMode/AnimationPlayModeHelpers.h"

#include "Assets/AssetManager.h"
#include "AssetCore/Asset.h"
#include "AssetCore/AssetTypes.h"
#include "Assets/AnimationClip.h"
#include "Components/Animation/Animator.h"
#include "Components/Animation/AnimatorRef.h"
#include "Components/Animation/SkeletonRef.h"
#include "Core/Engine.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Logger/Logger.h"

#include <unordered_map>

namespace GameEngine::Editor
{

namespace
{

using PreviewMap = std::unordered_map<uint32_t, AnimationPreviewManager::PreviewState>;

// entity-id -> originally-requested clip GUID (may be null = "use embedded default").
using PendingMap = std::unordered_map<uint32_t, GUID>;

PreviewMap& GetActivePreviews()
{
    static PreviewMap s_Previews;
    return s_Previews;
}

PendingMap& GetPendingPreviews()
{
    static PendingMap s_Pending;
    return s_Pending;
}

uint32_t PackEntity(ECS::EntityHandle e) { return e.id; }

// The model GUID backing this entity's subtree, or null if none is referenced.
GUID SubtreeModelGuid(ECS::World& world, ECS::EntityHandle rootEntity)
{
    std::vector<ECS::EntityHandle> subtree;
    CollectEntitiesInSubtree(world, rootEntity, subtree);
    return FindFirstModelGuidInSubtree(world, subtree);
}

void UpdateAnimationSystemEnabled()
{
    bool anyActive = !GetActivePreviews().empty();
    EngineCore::GetInstance().SetRenderingSystemEnabled("Animation", anyActive);
}

} // namespace

AnimationPreviewManager::EnableResult AnimationPreviewManager::TryResolveAndEnable(
    ECS::World& world, ECS::EntityHandle rootEntity, const GUID& clipGuid, bool logOnFailure)
{
    auto& am = EngineCore::GetInstance().GetAssetManager();

    GUID effectiveClipGuid = clipGuid;
    if (effectiveClipGuid.IsNull())
        effectiveClipGuid = ResolveDefaultEmbeddedClip(world, rootEntity, am).clipGuid;

    if (!effectiveClipGuid.IsNull())
    {
        uint32_t clipIndex = ResolveClipIndexForEntity(world, rootEntity, effectiveClipGuid, am);
        if (clipIndex != 0)
        {
            PreviewState entry{};
            CollectEntitiesInSubtree(world, rootEntity, entry.subtree);

            auto resolved = Engine::Renderer::ClipStore::Instance().Get(clipIndex);
            entry.clipIndex = clipIndex;
            entry.clipGuid = effectiveClipGuid;
            entry.duration = resolved ? resolved->GetDuration() : 0.0f;

            const auto* animator = world.GetComponent<Components::Animator>(rootEntity);
            entry.speed = animator ? std::max(0.0f, animator->speedScale) : 1.0f;
            const bool loop = animator ? animator->loop : true;

            ApplyAnimatorPlayState(world, entry.subtree, clipIndex, 0.0f, entry.speed, loop);
            const size_t subtreeSize = entry.subtree.size();
            const float durationCopy = entry.duration;
            GetActivePreviews()[PackEntity(rootEntity)] = std::move(entry);

            UpdateAnimationSystemEnabled();
            Logger::Log::Info(
                "AnimationPreview: enabled for entity {} clipIdx={} duration={:.2f}s subtree={}",
                rootEntity.id, clipIndex, durationCopy, subtreeSize);
            return EnableResult::Enabled;
        }
    }

    // Couldn't resolve a clip. If the subtree references a model that isn't
    // resident yet, the load was just kicked (non-blocking) — treat as pending
    // and retry once it loads rather than reporting a failure.
    const GUID modelGuid = SubtreeModelGuid(world, rootEntity);
    if (!modelGuid.IsNull() && !am.GetAsset(modelGuid))
        return EnableResult::Pending;

    if (logOnFailure)
    {
        if (clipGuid.IsNull())
            Logger::Log::Warning("AnimationPreview: no clip assigned for entity {}", rootEntity.id);
        else
            Logger::Log::Warning("AnimationPreview: could not resolve clip {} (entity {})",
                                 clipGuid.ToCompactString(), rootEntity.id);
    }
    return EnableResult::Failed;
}

bool AnimationPreviewManager::EnablePreview(ECS::World& world, ECS::EntityHandle rootEntity,
                                            const GUID& clipGuid)
{
    auto& previews = GetActivePreviews();
    uint32_t key = PackEntity(rootEntity);

    // Already active — switch clip instead
    if (previews.count(key))
        return SwitchClip(world, rootEntity, clipGuid);

    // First attempt: suppress the failure warning so a still-loading model
    // registers as pending instead of logging.
    switch (TryResolveAndEnable(world, rootEntity, clipGuid, /*logOnFailure*/ false))
    {
        case EnableResult::Enabled:
            GetPendingPreviews().erase(key);
            return true;
        case EnableResult::Pending:
            GetPendingPreviews()[key] = clipGuid;
            return true; // pending, not a hard failure
        case EnableResult::Failed:
            // Re-run once with logging to emit the genuine warning.
            (void)TryResolveAndEnable(world, rootEntity, clipGuid, /*logOnFailure*/ true);
            return false;
    }
    return false;
}

void AnimationPreviewManager::TickPending(ECS::World& world)
{
    auto& pending = GetPendingPreviews();
    if (pending.empty())
        return;

    auto& am = EngineCore::GetInstance().GetAssetManager();

    for (auto it = pending.begin(); it != pending.end();)
    {
        const ECS::EntityHandle root{it->first};
        const GUID clipGuid = it->second;

        if (!world.IsValid(root))
        {
            it = pending.erase(it);
            continue;
        }

        // Still loading? Keep waiting.
        const GUID modelGuid = SubtreeModelGuid(world, root);
        if (!modelGuid.IsNull() && !am.GetAsset(modelGuid))
        {
            ++it;
            continue;
        }

        // Model is resident (or none referenced). Resolve once, logging a
        // genuine no-clip failure since the model is no longer the holdup.
        switch (TryResolveAndEnable(world, root, clipGuid, /*logOnFailure*/ true))
        {
            case EnableResult::Pending:
                // Model load was re-kicked between the check and resolve; keep waiting.
                ++it;
                break;
            case EnableResult::Enabled:
            case EnableResult::Failed:
                it = pending.erase(it);
                break;
        }
    }
}

void AnimationPreviewManager::DisablePreview(ECS::World& world, ECS::EntityHandle rootEntity)
{
    auto& previews = GetActivePreviews();
    uint32_t key = PackEntity(rootEntity);

    // A deselect/clear should also cancel a not-yet-resolved pending retry.
    GetPendingPreviews().erase(key);

    auto it = previews.find(key);
    if (it == previews.end())
        return;

    ApplyAnimatorRestState(world, it->second.subtree);
    previews.erase(it);

    UpdateAnimationSystemEnabled();
}

bool AnimationPreviewManager::SwitchClip(ECS::World& world, ECS::EntityHandle rootEntity,
                                         const GUID& clipGuid)
{
    auto& previews = GetActivePreviews();
    uint32_t key = PackEntity(rootEntity);

    // If this entity's preview is still pending (model loading), just retarget
    // the requested clip — it'll resolve on the next TickPending.
    if (auto pendingIt = GetPendingPreviews().find(key); pendingIt != GetPendingPreviews().end())
    {
        pendingIt->second = clipGuid;
        return true;
    }

    auto it = previews.find(key);
    if (it == previews.end())
        return false;

    auto& entry = it->second;
    auto& am = EngineCore::GetInstance().GetAssetManager();

    uint32_t clipIndex = ResolveClipIndexForEntity(world, rootEntity, clipGuid, am);
    if (clipIndex == 0)
        return false;

    auto resolved = Engine::Renderer::ClipStore::Instance().Get(clipIndex);
    entry.clipIndex = clipIndex;
    entry.clipGuid = clipGuid;
    entry.duration = resolved ? resolved->GetDuration() : 0.0f;

    // Update ClipIndex and reset Time, but preserve existing Flags and Speed.
    for (ECS::EntityHandle e : entry.subtree)
    {
        auto* anim = world.GetComponent<Components::AnimatorRef>(e);
        auto* skel = world.GetComponent<Components::SkeletonRef>(e);
        if (!anim || !skel)
            continue;

        Components::AnimatorRef updated = *anim;
        updated.ClipIndex = clipIndex;
        updated.Time = 0.0f;
        world.AddComponentImmediate(e, updated);
    }

    return true;
}

void AnimationPreviewManager::SetPaused(ECS::World& world, ECS::EntityHandle rootEntity, bool paused)
{
    auto& previews = GetActivePreviews();
    auto it = previews.find(PackEntity(rootEntity));
    if (it == previews.end())
        return;

    for (ECS::EntityHandle e : it->second.subtree)
    {
        auto* anim = world.GetComponent<Components::AnimatorRef>(e);
        auto* skel = world.GetComponent<Components::SkeletonRef>(e);
        if (!anim || !skel)
            continue;

        Components::AnimatorRef updated = *anim;
        if (paused)
            updated.Flags |= Components::AnimatorRef::kFlag_Paused;
        else
            updated.Flags &= ~Components::AnimatorRef::kFlag_Paused;
        world.AddComponentImmediate(e, updated);
    }
}

void AnimationPreviewManager::SetSpeed(ECS::World& world, ECS::EntityHandle rootEntity, float speed)
{
    auto& previews = GetActivePreviews();
    auto it = previews.find(PackEntity(rootEntity));
    if (it == previews.end())
        return;

    it->second.speed = speed;

    for (ECS::EntityHandle e : it->second.subtree)
    {
        auto* anim = world.GetComponent<Components::AnimatorRef>(e);
        auto* skel = world.GetComponent<Components::SkeletonRef>(e);
        if (!anim || !skel)
            continue;

        Components::AnimatorRef updated = *anim;
        updated.Speed = speed;
        world.AddComponentImmediate(e, updated);
    }
}

void AnimationPreviewManager::SetTime(ECS::World& world, ECS::EntityHandle rootEntity, float time)
{
    auto& previews = GetActivePreviews();
    auto it = previews.find(PackEntity(rootEntity));
    if (it == previews.end())
        return;

    for (ECS::EntityHandle e : it->second.subtree)
    {
        auto* anim = world.GetComponent<Components::AnimatorRef>(e);
        auto* skel = world.GetComponent<Components::SkeletonRef>(e);
        if (!anim || !skel)
            continue;

        Components::AnimatorRef updated = *anim;
        updated.Time = time;
        world.AddComponentImmediate(e, updated);
    }
}

bool AnimationPreviewManager::IsPreviewActive(ECS::EntityHandle rootEntity)
{
    return GetActivePreviews().count(PackEntity(rootEntity)) > 0;
}

const AnimationPreviewManager::PreviewState* AnimationPreviewManager::GetState(ECS::EntityHandle rootEntity)
{
    auto& previews = GetActivePreviews();
    auto it = previews.find(PackEntity(rootEntity));
    if (it == previews.end())
        return nullptr;
    return &it->second;
}

void AnimationPreviewManager::DisableAllPreviews(ECS::World& world)
{
    auto& previews = GetActivePreviews();
    for (auto& [key, entry] : previews)
        ApplyAnimatorRestState(world, entry.subtree);

    previews.clear();
    GetPendingPreviews().clear();
    UpdateAnimationSystemEnabled();
}

bool AnimationPreviewManager::AnyPreviewActive()
{
    return !GetActivePreviews().empty();
}

} // namespace GameEngine::Editor
