#include "PlayMode/AnimationPlayModeHelpers.h"

#include "Assets/AnimationClip.h"
#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "AssetCore/Asset.h"
#include "Components/Animation/AnimatorRef.h"
#include "Components/Animation/SkeletonRef.h"
#include "Components/Hierarchy.h"
#include "Components/Rendering/MeshRenderer.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "Engine/Rendering/TimelinePlayback.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Logger/Logger.h"

using namespace GameEngine::Components;

namespace GameEngine::Editor
{

void CollectEntitiesInSubtree(ECS::World& world, ECS::EntityHandle root,
                              std::vector<ECS::EntityHandle>& out)
{
    out.clear();
    if (!root.IsValid())
        return;

    std::vector<ECS::EntityHandle> queue;
    queue.push_back(root);
    out.push_back(root);

    for (size_t qi = 0; qi < queue.size(); ++qi)
    {
        const ECS::EntityHandle cur = queue[qi];
        world.Query<ECS::Read<Components::Parent>>().IncludeDisabled().Each(
            [&](ECS::EntityHandle child, const Components::Parent& p)
            {
                if (p.parent == cur)
                {
                    queue.push_back(child);
                    out.push_back(child);
                }
            });
    }
}

GUID FindFirstModelGuidInSubtree(ECS::World& world,
                                 const std::vector<ECS::EntityHandle>& subtree)
{
    for (ECS::EntityHandle e : subtree)
    {
        const auto* mr = world.GetComponent<Components::MeshRenderer>(e);
        if (!mr)
            continue;
        const GUID g = mr->modelAssetGuid.ToGuid();
        if (!g.IsNull())
            return g;
    }
    return GUID::Null();
}

void ApplyAnimatorRestState(ECS::World& world,
                            const std::vector<ECS::EntityHandle>& subtree)
{
    for (ECS::EntityHandle e : subtree)
    {
        auto* anim = world.GetComponent<Components::AnimatorRef>(e);
        auto* skel = world.GetComponent<Components::SkeletonRef>(e);
        if (!anim || !skel)
            continue;

        Components::AnimatorRef updated = *anim;
        updated.SetAnimation(0);
        updated.Speed = 1.0f;
        updated.SectionStart = 0.0f;
        updated.SectionEnd = 0.0f;
        updated.Flags |= Components::AnimatorRef::kFlag_Paused;
        updated.Flags &= ~Components::AnimatorRef::kFlag_Section;
        world.AddComponentImmediate(e, updated);
    }
}

void ApplyAnimatorPlayState(ECS::World& world,
                            const std::vector<ECS::EntityHandle>& subtree,
                            uint32_t clipIndex,
                            float32 timeSeconds,
                            float32 speedScale,
                            bool loop,
                            float32 blendSeconds,
                            bool section,
                            float32 sectionStartSeconds,
                            float32 sectionEndSeconds)
{
    if (clipIndex == 0)
    {
        ApplyAnimatorRestState(world, subtree);
        return;
    }

    for (ECS::EntityHandle e : subtree)
    {
        auto* anim = world.GetComponent<Components::AnimatorRef>(e);
        auto* skel = world.GetComponent<Components::SkeletonRef>(e);
        if (!anim || !skel)
            continue;

        Components::AnimatorRef updated = *anim;
        updated.SetAnimation(clipIndex, blendSeconds);
        updated.Time = timeSeconds;
        updated.Speed = speedScale;
        updated.SectionStart = sectionStartSeconds;
        updated.SectionEnd = sectionEndSeconds;
        updated.Flags &= ~(Components::AnimatorRef::kFlag_Paused |
                           Components::AnimatorRef::kFlag_Loop |
                           Components::AnimatorRef::kFlag_Section);
        if (loop)
            updated.Flags |= Components::AnimatorRef::kFlag_Loop;
        if (section)
            updated.Flags |= Components::AnimatorRef::kFlag_Section;
        world.AddComponentImmediate(e, updated);
    }
}

SharedPtr<Asset> TryAcquireModelAsset(AssetManager& am, const GUID& modelGuid)
{
    if (SharedPtr<Asset> cached = am.GetAsset(modelGuid))
        return cached;

    // Never block the main thread: a model's GPU upload finishes on this thread,
    // so a synchronous wait can stall the editor. Kick a non-blocking load and
    // return "not ready"; callers retry once GetAsset hits.
    (void)am.LoadAssetAsync(modelGuid, AssetLoadPriority::High);
    return nullptr;
}

uint32_t ResolveClipIndex(const GUID& clipGuid, AssetManager& am)
{
    return Engine::Renderer::ResolveClipIndex(clipGuid, am);
}

DefaultEmbeddedClip ResolveDefaultEmbeddedClip(ECS::World& world, ECS::EntityHandle rootEntity,
                                               AssetManager& am)
{
    std::vector<ECS::EntityHandle> subtree;
    CollectEntitiesInSubtree(world, rootEntity, subtree);

    const GUID modelGuid = FindFirstModelGuidInSubtree(world, subtree);
    if (modelGuid.IsNull())
        return {};

    SharedPtr<Asset> asset = TryAcquireModelAsset(am, modelGuid);
    auto* modelAsset = dynamic_cast<ModelAsset*>(asset.get());
    if (!modelAsset || !modelAsset->IsLoaded())
        return {};

    const auto& embedGuids = modelAsset->GetEmbeddedClipGuids();
    if (embedGuids.empty())
        return {};

    return {modelGuid, embedGuids.front()};
}

uint32_t ResolveClipIndexForEntity(ECS::World& world, ECS::EntityHandle rootEntity,
                                   const GUID& clipGuid, AssetManager& am)
{
    if (clipGuid.IsNull())
        return 0;

    if (uint32_t idx = ResolveClipIndex(clipGuid, am); idx != 0)
        return idx;

    std::vector<ECS::EntityHandle> subtree;
    CollectEntitiesInSubtree(world, rootEntity, subtree);
    const GUID modelGuid = FindFirstModelGuidInSubtree(world, subtree);
    if (!modelGuid.IsNull())
        (void)TryAcquireModelAsset(am, modelGuid);

    return ResolveClipIndex(clipGuid, am);
}

} // namespace GameEngine::Editor
