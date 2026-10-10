#include "PlayMode/ModelAnimationPlayMode.h"
#include "PlayMode/AnimationPlayModeHelpers.h"

#include "Animation/AnimationTimeline.h"
#include "Assets/AssetManager.h"
#include "AssetCore/Asset.h"
#include "AssetCore/AssetTypes.h"
#include "Assets/TimelineAsset.h"
#include "Components/Animation/AnimatorRef.h"
#include "Components/Animation/Animator.h"
#include "Components/Animation/TimelinePlaybackState.h"
#include "Components/Animation/SkeletonRef.h"
#include "Engine/Rendering/TimelinePlayback.h"
#include "Engine/Rendering/AnimatorScenePlayback.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace GameEngine::Components;

namespace GameEngine::Editor
{

void ApplyAnimatorOnEnterPlayMode(ECS::World& world)
{
    auto& am = EngineCore::GetInstance().GetAssetManager();
    ApplyAnimatorOnEnterPlayMode(world, am);
}

namespace
{

void StartAnimatorPlayback(ECS::World& world, AssetManager& am, ECS::EntityHandle rootEntity,
                           Components::Animator& animator)
{
    std::vector<ECS::EntityHandle> subtree;
    CollectEntitiesInSubtree(world, rootEntity, subtree);

    if (animator.pendingCommand != AnimatorPlaybackCommand::None)
        return;

    if (!animator.active || !animator.autoPlayOnEnterPlayMode)
    {
        ApplyAnimatorRestState(world, subtree);
        return;
    }

    // Clip playback acquires its own source model without blocking
    // (ResolveAnimatorClipIndex); the other sources still need the
    // entity's model resident so its embedded clips are registered.
    const GUID modelGuid = FindFirstModelGuidInSubtree(world, subtree);
    if (!modelGuid.IsNull() && animator.source != AnimatorPlaybackSource::Clip)
    {
        SharedPtr<Asset> modelAsset = TryAcquireModelAsset(am, modelGuid);
        if (modelAsset && modelAsset->GetType() != AssetType::Model)
        {
            Logger::Log::Warning("Animator: asset {} is not a model (type {})",
                modelGuid.ToCompactString(),
                static_cast<int>(modelAsset->GetType()));
            ApplyAnimatorRestState(world, subtree);
            return;
        }
    }

    Logger::Log::Debug("Animator: starting playback, clip={} subtreeSize={}",
        animator.clipGuid.ToGuid().ToCompactString(), subtree.size());

    if (animator.source == AnimatorPlaybackSource::Graph)
        return;

    if (animator.source == AnimatorPlaybackSource::Timeline)
    {
        TimelinePlaybackState playbackState;
        playbackState.playing = animator.autoPlayOnEnterPlayMode;
        playbackState.timeSeconds = std::max(0.0f, animator.seekTimeSeconds);
        playbackState.previousTimeSeconds = playbackState.timeSeconds;
        Engine::Renderer::EnsureTimelinePlaybackState(world, rootEntity, playbackState);

        SharedPtr<Asset> timelineAssetPtr =
            Engine::Renderer::TryAcquireTimelineAsset(am, animator.timelineGuid.ToGuid());
        auto* timelineAsset =
            timelineAssetPtr ? dynamic_cast<TimelineAsset*>(timelineAssetPtr.get()) : nullptr;
        if (timelineAsset)
        {
            Animation::Timeline timeline;
            std::string loadError;
            if (Engine::Renderer::LoadTimelineFromAsset(*timelineAsset, timeline, &loadError))
            {
                Animation::TimelineEvaluateOptions options;
                options.Loop = animator.loop;
                const float32 t = playbackState.timeSeconds;
                const auto evaluation =
                    Animation::EvaluateTimeline(timeline, t, t, options);
                Engine::Renderer::TimelinePlaybackApplyContext context;
                context.world = &world;
                context.rootEntity = rootEntity;
                context.assetManager = &am;
                context.subtree = &subtree;
                context.rootNodePath = animator.RootNode();
                context.speedScale = animator.speedScale;
                context.loop = animator.loop;
                context.audioMaxPolyphony = std::max(1u, animator.audioMaxPolyphony);
                Engine::Renderer::ApplyTimelineEvaluation(evaluation, context);
            }
        }
        return;
    }

    Engine::Renderer::StartAnimatorFromSource(world, am, animator, subtree);
}

} // namespace

void ApplyAnimatorOnEnterPlayMode(ECS::World& world, AssetManager& am)
{
    world.Query<ECS::Write<Components::Animator>>().Each(
        [&](ECS::EntityHandle rootEntity, Components::Animator& animator)
        { StartAnimatorPlayback(world, am, rootEntity, animator); });
}

} // namespace GameEngine::Editor
