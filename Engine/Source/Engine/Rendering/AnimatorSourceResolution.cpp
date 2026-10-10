#include "AnimatorSourceResolution.h"

#include "Animation/AnimationController.h"
#include "Animation/AnimationLibrary.h"
#include "Animation/AnimationTimeline.h"
#include "AssetCore/Asset.h"
#include "AssetCore/AssetTypes.h"
#include "Assets/AssetManager.h"
#include "Assets/TimelineAsset.h"
#include "Components/Animation/Animator.h"
#include "Engine/Rendering/AnimatorScenePlayback.h"
#include "Engine/Rendering/TimelinePlayback.h"

#include <algorithm>
#include <string>

using namespace GameEngine::Components;

namespace GameEngine::Engine::Renderer
{

namespace
{

// A library or controller is a small JSON document with no import, so reading one that is not
// resident yet is short; the clips it names resolve without waiting (ResolveSourceClip).
SharedPtr<Asset> TryAcquireAsset(AssetManager& am, const GUID& guid)
{
    if (guid.IsNull())
        return nullptr;
    if (SharedPtr<Asset> cached = am.GetAsset(guid))
        return cached;
    AssetFuture future = am.LoadAssetAsync(guid, AssetLoadPriority::High);
    if (!future.Valid())
        return nullptr;
    return future.get();
}

// Every clip a playback source names resolves through the one rule (ResolvePlaybackClipIndex). A
// clip whose container is still loading marks the playback pending, and it starts once the
// container lands (ProcessClipAnimatorCommands).
uint32_t ResolveSourceClip(AssetManager& am, const GUID& clipGuid, const GUID& subtreeModel,
                           ResolvedAnimatorPlayback& out)
{
    bool pending = false;
    const uint32_t clipIndex =
        ResolvePlaybackClipIndex(clipGuid, GUID::Null(), subtreeModel, am, &pending);
    if (pending)
        out.SourcePending = true;
    else if (clipIndex == 0 && !clipGuid.IsNull() && out.UnresolvedClip.IsNull())
        out.UnresolvedClip = clipGuid;
    return clipIndex;
}

bool ResolveTimelinePlayback(AssetManager& am,
                             const GUID& subtreeModel,
                             const GUID& timelineGuid,
                             float32 sampleTimeSeconds,
                             bool loop,
                             ResolvedAnimatorPlayback& out)
{
    SharedPtr<Asset> asset = TryAcquireTimelineAsset(am, timelineGuid);
    auto* timelineAsset = asset ? dynamic_cast<TimelineAsset*>(asset.get()) : nullptr;
    if (!timelineAsset)
        return false;

    Animation::Timeline timeline;
    std::string error;
    if (!LoadTimelineFromAsset(*timelineAsset, timeline, &error))
        return false;

    Animation::TimelineEvaluateOptions options;
    options.Loop = loop;
    const auto evaluated = Animation::EvaluateTimeline(timeline, sampleTimeSeconds, sampleTimeSeconds, options);
    ResolvedTimelineAnimationClip clip;
    if (PickDominantAnimationClip(evaluated, am, clip))
    {
        out.ClipIndex = clip.clipIndex;
        out.TimeSeconds = clip.localTimeSeconds;
        out.Loop = loop;
        return true;
    }

    for (const auto& track : timeline.Tracks)
    {
        if (track.Type != Animation::TimelineTrackType::Animation)
            continue;
        for (const auto& trackClip : track.Clips)
        {
            if (trackClip.Muted || trackClip.ClipAssetType != AssetType::Animation)
                continue;
            const uint32_t clipIndex = ResolveSourceClip(am, trackClip.ClipGuid, subtreeModel, out);
            if (clipIndex != 0)
            {
                out.ClipIndex = clipIndex;
                out.TimeSeconds = trackClip.InTime;
                out.Loop = loop;
                return true;
            }
        }
    }

    return false;
}

bool ResolveMotionPlayback(AssetManager& am,
                           const GUID& subtreeModel,
                           const Animation::AnimationStateMotion& motion,
                           float32 sampleTimeSeconds,
                           ResolvedAnimatorPlayback& out,
                           int depth);

bool ResolveLibraryPlayback(AssetManager& am,
                            const GUID& subtreeModel,
                            const GUID& libraryGuid,
                            const std::string& animationName,
                            float32 sampleTimeSeconds,
                            ResolvedAnimatorPlayback& out,
                            int depth)
{
    auto asset = TryAcquireAsset(am, libraryGuid);
    auto* library = asset ? dynamic_cast<Animation::AnimationLibrary*>(asset.get()) : nullptr;
    if (!library || library->Entries().empty())
        return false;

    const Animation::AnimationLibraryEntry* entry = animationName.empty()
        ? &library->Entries().front()
        : library->Find(animationName);
    if (!entry)
        return false;
    if (out.LibraryEntry.empty())
        out.LibraryEntry = entry->Name;

    Animation::AnimationStateMotion motion;
    motion.AssetGuid = entry->AssetGuid;
    motion.Type = entry->Type;
    motion.LibraryName = entry->Name;
    motion.Loop = entry->Loop;
    if (!ResolveMotionPlayback(am, subtreeModel, motion, sampleTimeSeconds, out, depth + 1))
        return false;
    out.Loop = entry->Loop;
    return true;
}

bool ResolveControllerPlayback(AssetManager& am,
                               const GUID& subtreeModel,
                               const GUID& controllerGuid,
                               float32 sampleTimeSeconds,
                               ResolvedAnimatorPlayback& out,
                               int depth)
{
    auto asset = TryAcquireAsset(am, controllerGuid);
    auto* controller = asset ? dynamic_cast<Animation::AnimationController*>(asset.get()) : nullptr;
    if (!controller || controller->States().empty())
        return false;

    const Animation::AnimationControllerState* state = controller->FindState(controller->EntryState());
    if (!state)
        state = &controller->States().front();

    if (!state->BlendTree.Children.empty())
    {
        if (ResolveMotionPlayback(am, subtreeModel, state->BlendTree.Children.front().Motion, sampleTimeSeconds, out,
                                  depth + 1))
            return true;
    }
    return ResolveMotionPlayback(am, subtreeModel, state->Motion, sampleTimeSeconds, out, depth + 1);
}

bool ResolveMotionPlayback(AssetManager& am,
                           const GUID& subtreeModel,
                           const Animation::AnimationStateMotion& motion,
                           float32 sampleTimeSeconds,
                           ResolvedAnimatorPlayback& out,
                           int depth)
{
    if (depth > 4)
        return false;

    if (motion.Type == AssetType::Animation)
    {
        const uint32_t clipIndex = ResolveSourceClip(am, motion.AssetGuid, subtreeModel, out);
        if (clipIndex == 0)
            return false;
        out.ClipIndex = clipIndex;
        out.TimeSeconds = sampleTimeSeconds;
        out.SpeedScale *= motion.Speed;
        out.Loop = motion.Loop;
        return true;
    }
    if (motion.Type == AssetType::Timeline)
    {
        if (!ResolveTimelinePlayback(am, subtreeModel, motion.AssetGuid, sampleTimeSeconds, motion.Loop, out))
            return false;
        out.SpeedScale *= motion.Speed;
        return true;
    }
    if (motion.Type == AssetType::AnimationLibrary)
    {
        if (!ResolveLibraryPlayback(am, subtreeModel, motion.AssetGuid, motion.LibraryName, sampleTimeSeconds, out,
                                    depth + 1))
            return false;
        out.SpeedScale *= motion.Speed;
        return true;
    }
    if (motion.Type == AssetType::AnimationController)
    {
        if (!ResolveControllerPlayback(am, subtreeModel, motion.AssetGuid, sampleTimeSeconds, out, depth + 1))
            return false;
        out.SpeedScale *= motion.Speed;
        return true;
    }
    return false;
}

} // namespace

ResolvedAnimatorPlayback ResolveAnimatorPlayback(const Components::Animator& animator, const GUID& targetModel,
                                                 AssetManager& am)
{
    const bool section = animator.sectionEndSeconds > animator.sectionStartSeconds;
    const float32 sampleTime = section
        ? animator.sectionStartSeconds
        : std::max(0.0f, animator.seekTimeSeconds);

    ResolvedAnimatorPlayback resolved;
    resolved.SpeedScale = std::max(0.0f, animator.speedScale);
    resolved.Loop = animator.loop;

    switch (animator.source)
    {
        case Components::AnimatorPlaybackSource::Timeline:
            ResolveTimelinePlayback(am, targetModel, animator.timelineGuid.ToGuid(), sampleTime, animator.loop, resolved);
            break;
        case Components::AnimatorPlaybackSource::Library:
            ResolveLibraryPlayback(am, targetModel, animator.libraryGuid.ToGuid(), std::string(animator.AssignedAnimation()),
                                   sampleTime, resolved, 0);
            break;
        case Components::AnimatorPlaybackSource::Controller:
            ResolveControllerPlayback(am, targetModel, animator.controllerGuid.ToGuid(), sampleTime, resolved, 0);
            break;
        case Components::AnimatorPlaybackSource::Graph:
            break;
        case Components::AnimatorPlaybackSource::Clip:
        default:
            resolved.ClipIndex = ResolveAnimatorClipIndex(animator, targetModel, am, &resolved.SourcePending);
            resolved.TimeSeconds = sampleTime;
            break;
    }

    if (resolved.ClipIndex == 0 && !animator.clipGuid.IsNull() && animator.source != AnimatorPlaybackSource::Clip)
    {
        resolved.ClipIndex = ResolveSourceClip(am, animator.clipGuid.ToGuid(), targetModel, resolved);
        resolved.TimeSeconds = sampleTime;
    }
    return resolved;
}

} // namespace GameEngine::Engine::Renderer
