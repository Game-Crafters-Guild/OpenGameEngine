#include "ECSModules/Rendering/Systems/TimelinePlaybackSystem.h"

#include "Animation/AnimationTimeline.h"
#include "AssetCore/Asset.h"
#include "Assets/TimelineAsset.h"
#include "Audio/AudioSystem.h"
#include "Components/Animation/Animator.h"
#include "Components/Animation/TimelinePlaybackState.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Engine/Rendering/AnimatorScenePlayback.h"
#include "Engine/Rendering/TimelinePlayback.h"

#include <cmath>
#include <vector>

namespace GameEngine::Engine::Renderer
{
namespace
{

using namespace GameEngine::Components;

float32 WrapTimelineTime(float32 time, float32 duration, bool loop)
{
    if (duration <= 0.0f)
        return std::max(0.0f, time);
    if (!loop)
        return std::clamp(time, 0.0f, duration);
    float32 wrapped = std::fmod(time, duration);
    if (wrapped < 0.0f)
        wrapped += duration;
    return wrapped;
}

void ProcessAnimatorCommands(Animator& animator, TimelinePlaybackState& state)
{
    switch (animator.pendingCommand)
    {
    case AnimatorPlaybackCommand::Play:
    case AnimatorPlaybackCommand::PlaySection:
    case AnimatorPlaybackCommand::PlayWithBlend:
        state.playing = true;
        state.paused = false;
        if (animator.seekTimeSeconds > 0.0f)
        {
            state.timeSeconds = animator.seekTimeSeconds;
            state.previousTimeSeconds = animator.seekTimeSeconds;
        }
        break;
    case AnimatorPlaybackCommand::Seek:
        state.timeSeconds = std::max(0.0f, animator.seekTimeSeconds);
        state.previousTimeSeconds = state.timeSeconds;
        state.playing = true;
        break;
    case AnimatorPlaybackCommand::Pause:
        state.paused = true;
        break;
    case AnimatorPlaybackCommand::Stop:
        state.playing = false;
        state.paused = false;
        state.timeSeconds = 0.0f;
        state.previousTimeSeconds = 0.0f;
        break;
    case AnimatorPlaybackCommand::None:
    default:
        break;
    }
    animator.pendingCommand = AnimatorPlaybackCommand::None;
}

} // namespace

TimelinePlaybackSystem::TimelinePlaybackSystem(Audio::AudioSystem* audioSystem)
    : m_AudioSystem(audioSystem)
{
}

void TimelinePlaybackSystem::Update(ECS::World& world, float32 deltaTime)
{
    ProcessClipAnimatorCommands(world);

    AssetManager& assetManager = EngineCore::GetInstance().GetAssetManager();
    Audio::AudioSystem* audio = m_AudioSystem;
    if (!audio)
        audio = EngineCore::GetInstance().GetAudioSystem();

    world.Query<ECS::Write<Animator>>().Each([&](ECS::EntityHandle rootEntity, Animator& animator)
    {
        if (!animator.active || animator.source != AnimatorPlaybackSource::Timeline || animator.timelineGuid.IsNull())
            return;

        auto* state = world.GetComponentForWrite<TimelinePlaybackState>(rootEntity);
        if (!state)
        {
            TimelinePlaybackState initial;
            initial.playing = animator.autoPlayOnEnterPlayMode;
            initial.timeSeconds = std::max(0.0f, animator.seekTimeSeconds);
            initial.previousTimeSeconds = initial.timeSeconds;
            EnsureTimelinePlaybackState(world, rootEntity, initial);
            state = world.GetComponentForWrite<TimelinePlaybackState>(rootEntity);
            if (!state)
                return;
        }

        ProcessAnimatorCommands(animator, *state);

        if (!state->playing || state->paused)
            return;

        SharedPtr<Asset> asset = TryAcquireTimelineAsset(assetManager, animator.timelineGuid.ToGuid());
        auto* timelineAsset = asset ? dynamic_cast<TimelineAsset*>(asset.get()) : nullptr;
        if (!timelineAsset)
            return;

        Animation::Timeline timeline;
        std::string loadError;
        if (!LoadTimelineFromAsset(*timelineAsset, timeline, &loadError))
            return;

        const float32 duration = Animation::GetTimelineDuration(timeline);
        const float32 dt = std::max(0.0f, deltaTime) * std::max(0.0f, animator.speedScale);
        const float32 previousTime = state->timeSeconds;
        float32 currentTime = previousTime + dt;
        currentTime = WrapTimelineTime(currentTime, duration, animator.loop);
        state->previousTimeSeconds = previousTime;
        state->timeSeconds = currentTime;

        Animation::TimelineEvaluateOptions options;
        options.Loop = animator.loop;
        const Animation::TimelineEvaluationResult evaluation =
            Animation::EvaluateTimeline(timeline, previousTime, currentTime, options);

        std::vector<ECS::EntityHandle> subtree;
        CollectEntitiesInSubtree(world, rootEntity, subtree);

        std::vector<Audio::AudioEmitterHandle> activeVoices;
        TimelinePlaybackApplyContext context;
        context.world = &world;
        context.rootEntity = rootEntity;
        context.assetManager = &assetManager;
        context.audioSystem = audio;
        context.subtree = &subtree;
        context.rootNodePath = animator.RootNode();
        context.speedScale = animator.speedScale;
        context.loop = animator.loop;
        context.audioMaxPolyphony = std::max(1u, animator.audioMaxPolyphony);
        context.activeVoices = &activeVoices;

        ApplyTimelineEvaluation(evaluation, context);
    });
}

} // namespace GameEngine::Engine::Renderer
