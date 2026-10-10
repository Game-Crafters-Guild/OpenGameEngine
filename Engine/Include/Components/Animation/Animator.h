#pragma once

#include "AssetCore/GUID.h"
#include "AssetCore/SubassetDeriveKeys.h"
#include "Components/AssetRef.h"
#include "Types/StringId.h"
#include "Types/StringUtils.h"
#include "Types/Types.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

namespace GameEngine::Components {

enum class AnimatorPlaybackSource : uint8
{
    Clip,
    Timeline,
    Library,
    Controller,
    Graph
};

enum class AnimatorGraphParamKind : uint8
{
    Float,
    Bool,
    Trigger
};

// One pending StringId param write. Applied by AnimationGraphSystem before
// Evaluate. POD so it can live on the Animator component.
struct AnimatorGraphParamWrite
{
    StringId id = 0;
    float floatValue = 0.0f;
    AnimatorGraphParamKind kind = AnimatorGraphParamKind::Float;
    uint8 boolValue = 0;
};

enum class AnimatorPlaybackCommand : uint8
{
    None,
    Play,
    PlaySection,
    PlayWithBlend,
    Seek,
    Pause,
    Stop,
    // Internal deferred start: preserve authored seek/section/blend settings
    // while the explicit clip source finishes loading. Not a SceneIO field.
    Autoplay
};

enum class AnimatorCallbackProcess : uint8
{
    Idle,
    Physics,
    Manual
};

enum class AnimatorCallbackMethod : uint8
{
    Deferred,
    Immediate
};

enum class AnimatorDiscreteCallbackMode : uint8
{
    Recessive,
    Dominant
};

// User-facing animation control component. Present on the root entity for
// animated entities. clipGuid is the Clip playback path; timelineGuid is
// Timeline; graphGuid is the pose-graph path (evaluated by
// AnimationGraphSystem). pendingCommand is consumed each animation tick
// for Clip (PlayState / CrossFadeSeconds) and Timeline. Graph param
// stamps are consumed by AnimationGraphSystem. Runtime pose lives on
// child entities via AnimatorRef.
struct Animator
{
    static constexpr uint32 kAnimationNameCapacity = 64;
    static constexpr uint32 kNodePathCapacity = 128;
    static constexpr uint32 kPendingGraphParamCapacity = 8;

    bool active = true;
    bool autoPlayOnEnterPlayMode = true;
    Components::AssetRef<AssetType::Animation> clipGuid;
    Components::AssetRef<AssetType::AnimationLibrary> libraryGuid;
    Components::AssetRef<AssetType::Timeline> timelineGuid;
    Components::AssetRef<AssetType::AnimationController> controllerGuid;
    Components::AssetRef<AssetType::AnimationGraph> graphGuid;
    char assignedAnimation[kAnimationNameCapacity] = {};
    char rootNode[kNodePathCapacity] = {};
    char rootMotionTrack[kNodePathCapacity] = {};
    float32 speedScale = 1.0f;
    float32 blendSeconds = 0.0f;
    float32 seekTimeSeconds = 0.0f;
    float32 sectionStartSeconds = 0.0f;
    float32 sectionEndSeconds = 0.0f;
    uint32 audioMaxPolyphony = 32;
    bool loop = true;
    bool deterministic = false;
    bool resetOnSave = true;
    // Graph source: extract bone-0 delta, zero the pose root, and drive
    // CharacterController (or Transform) this tick in entity-local space.
    bool rootMotionLocal = false;
    AnimatorPlaybackSource source = AnimatorPlaybackSource::Clip;
    AnimatorPlaybackCommand pendingCommand = AnimatorPlaybackCommand::None;
    AnimatorCallbackProcess callbackProcess = AnimatorCallbackProcess::Idle;
    AnimatorCallbackMethod callbackMethod = AnimatorCallbackMethod::Deferred;
    AnimatorDiscreteCallbackMode discreteCallbackMode = AnimatorDiscreteCallbackMode::Recessive;

    // [DoNotSerialize] GraphStore handle. 0 = none.
    uint64 graphRuntimeId = 0;
    // [DoNotSerialize] AnimationEventCollectorStore handle: the events this
    // animator's playback fired in the last animation wave. 0 = none yet;
    // AnimationEventCollectorSystem gives one at the start of the next wave.
    uint64 eventCollectorId = 0;
    // [DoNotSerialize] GUID the current graphRuntimeId was instantiated from.
    GUID graphInstanceGuid{};
    // [DoNotSerialize] Pause freezes graph DeltaTime without destroying the player.
    bool graphPaused = false;
    uint8 pendingGraphParamCount = 0;
    AnimatorGraphParamWrite pendingGraphParams[kPendingGraphParamCapacity] = {};

    // Durable source of an embedded clip. Numeric AnimatorRef indices are only
    // runtime caches. An absent pair preserves standalone clip/name playback.
    ModelRef clipSourceModelGuid;
    uint32 clipSourceAnimationIndex = UINT32_MAX;

    void ClearClipSource()
    {
        clipSourceModelGuid.Clear();
        clipSourceAnimationIndex = UINT32_MAX;
    }

    // Authoring selection of a clip by GUID: a standalone .anim, or an embedded
    // GUID whose container is not known at the call site. Drops the previous
    // source pair so a later clear cannot fall back to it.
    void SelectClip(const GUID& guid)
    {
        ClearClipSource();
        clipGuid.Set(guid);
        source = AnimatorPlaybackSource::Clip;
    }

    // Authoring selection of the Nth embedded animation of a model. The pair is
    // the durable identity; clipGuid receives the derived subasset GUID the
    // ClipStore keys on (same rule as ModelAsset::DeriveEmbeddedClipGuid).
    void SelectEmbeddedClip(const GUID& modelGuid, uint32 animationIndex)
    {
        clipSourceModelGuid.Set(modelGuid);
        clipSourceAnimationIndex = animationIndex;
        clipGuid.Set(GUID::Derive(modelGuid, EmbeddedClipDeriveKey(animationIndex)));
        source = AnimatorPlaybackSource::Clip;
    }

    template <size_t Capacity>
    static void CopyFixedString(char (&target)[Capacity], std::string_view value)
    {
        std::memset(target, 0, sizeof(target));
        const size_t count = std::min(value.size(), sizeof(target) - 1);
        if (count > 0)
            std::memcpy(target, value.data(), count);
    }

    std::string_view AssignedAnimation() const { return FixedStringView(assignedAnimation); }
    std::string_view RootNode() const { return FixedStringView(rootNode); }
    std::string_view RootMotionTrack() const { return FixedStringView(rootMotionTrack); }

    void SetAssignedAnimation(std::string_view animationName)
    {
        CopyFixedString(assignedAnimation, animationName);
    }

    void SetRootNode(std::string_view path)
    {
        CopyFixedString(rootNode, path);
    }

    void SetRootMotionTrack(std::string_view track)
    {
        CopyFixedString(rootMotionTrack, track);
    }

    // Hard-cut to a named embedded clip (or first embedded clip if the name
    // is empty). Consumed for Clip source by ProcessClipAnimatorCommands.
    void PlayState(std::string_view animationName)
    {
        ClearClipSource();
        SetAssignedAnimation(animationName);
        clipGuid.Clear();
        blendSeconds = 0.0f;
        source = AnimatorPlaybackSource::Clip;
        pendingCommand = AnimatorPlaybackCommand::Play;
    }

    void PlayState(const GUID& guid)
    {
        ClearClipSource();
        clipGuid.Set(guid);
        SetAssignedAnimation({});
        blendSeconds = 0.0f;
        source = AnimatorPlaybackSource::Clip;
        pendingCommand = AnimatorPlaybackCommand::Play;
    }

    // Crossfade to a named embedded clip. Duration is wall-clock seconds
    // (not normalized to the clip's length). Zero / non-finite duration is a hard cut.
    void CrossFadeSeconds(std::string_view animationName, float32 blendDurationSeconds)
    {
        ClearClipSource();
        SetAssignedAnimation(animationName);
        clipGuid.Clear();
        source = AnimatorPlaybackSource::Clip;
        StampBlendCommand(blendDurationSeconds);
    }

    void CrossFadeSeconds(const GUID& guid, float32 blendDurationSeconds)
    {
        ClearClipSource();
        clipGuid.Set(guid);
        SetAssignedAnimation({});
        source = AnimatorPlaybackSource::Clip;
        StampBlendCommand(blendDurationSeconds);
    }

    void PlayTimeline(const GUID& guid)
    {
        timelineGuid.Set(guid);
        source = AnimatorPlaybackSource::Timeline;
        pendingCommand = AnimatorPlaybackCommand::Play;
    }

    void PlayGraph(const GUID& guid)
    {
        graphGuid.Set(guid);
        graphPaused = false;
        source = AnimatorPlaybackSource::Graph;
        // Force instantiate this tick even if the previous player was Stop()'d.
        graphInstanceGuid = GUID{};
    }

    void SetFloat(::GameEngine::StringId id, float32 value)
    {
        StampGraphParam(id, AnimatorGraphParamKind::Float, value, 0);
    }

    void SetFloat(std::string_view name, float32 value)
    {
        SetFloat(HashStringId(name), value);
    }

    void SetBool(::GameEngine::StringId id, bool value)
    {
        StampGraphParam(id, AnimatorGraphParamKind::Bool, 0.0f, value ? 1u : 0u);
    }

    void SetBool(std::string_view name, bool value)
    {
        SetBool(HashStringId(name), value);
    }

    void SetTrigger(::GameEngine::StringId id)
    {
        StampGraphParam(id, AnimatorGraphParamKind::Trigger, 0.0f, 1u);
    }

    void SetTrigger(std::string_view name)
    {
        SetTrigger(HashStringId(name));
    }

    void PlaySection(std::string_view animationName, float32 startSeconds, float32 endSeconds)
    {
        ClearClipSource();
        clipGuid.Clear();
        SetAssignedAnimation(animationName);
        sectionStartSeconds = startSeconds;
        sectionEndSeconds = endSeconds;
        source = AnimatorPlaybackSource::Clip;
        pendingCommand = AnimatorPlaybackCommand::PlaySection;
    }

    void Seek(float32 timeSeconds)
    {
        seekTimeSeconds = timeSeconds;
        pendingCommand = AnimatorPlaybackCommand::Seek;
    }

    void Pause()
    {
        pendingCommand = AnimatorPlaybackCommand::Pause;
    }

    void Stop()
    {
        pendingCommand = AnimatorPlaybackCommand::Stop;
    }

private:
    void StampBlendCommand(float32 blendDurationSeconds)
    {
        const bool crossfade = std::isfinite(blendDurationSeconds) && blendDurationSeconds > 0.0f;
        blendSeconds = crossfade ? blendDurationSeconds : 0.0f;
        pendingCommand = crossfade ? AnimatorPlaybackCommand::PlayWithBlend : AnimatorPlaybackCommand::Play;
    }

    void StampGraphParam(::GameEngine::StringId id, AnimatorGraphParamKind kind, float32 floatValue, uint8 boolValue)
    {
        if (source != AnimatorPlaybackSource::Graph)
            return;
        if (id == 0)
            return;
        for (uint32 i = 0; i < pendingGraphParamCount; ++i)
        {
            if (pendingGraphParams[i].id != id)
                continue;
            pendingGraphParams[i].kind = kind;
            pendingGraphParams[i].floatValue = floatValue;
            pendingGraphParams[i].boolValue = boolValue;
            return;
        }
        if (pendingGraphParamCount >= kPendingGraphParamCapacity)
        {
            std::memmove(pendingGraphParams,
                         pendingGraphParams + 1,
                         sizeof(AnimatorGraphParamWrite) * (kPendingGraphParamCapacity - 1));
            --pendingGraphParamCount;
        }
        AnimatorGraphParamWrite& slot = pendingGraphParams[pendingGraphParamCount++];
        slot.id = id;
        slot.kind = kind;
        slot.floatValue = floatValue;
        slot.boolValue = boolValue;
    }
};

} // namespace GameEngine::Components
