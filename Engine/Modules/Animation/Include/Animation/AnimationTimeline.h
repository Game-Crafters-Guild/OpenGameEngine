#pragma once

#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include "AssetCore/Types.h"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace GameEngine
{
namespace Animation
{

inline constexpr const char* kTransformRotationPropertyPath = "Transform.rotation";

enum class TimelineTrackType : uint8
{
    Animation,
    Audio,
    Property,
    Position3D,
    Rotation3D,
    Scale3D,
    BlendShape,
    Method,
    Event,
    Bezier
};

enum class TimelineTrackUpdateMode : uint8
{
    Continuous,
    Discrete,
    Capture
};

enum class TimelineTrackInterpolation : uint8
{
    Nearest,
    Linear,
    Cubic
};

enum class TimelineBezierHandleMode : uint8
{
    Free,
    Linear,
    Balanced,
    Mirrored
};

struct TimelineMarker
{
    float32 Time = 0.0f;
    std::string Name;
};

struct TimelineValueKey
{
    float32 Time = 0.0f;
    // Position3D/Scale3D: x,y,z.
    // Rotation (Rotation3D, or Property/Bezier Transform.rotation):
    //   ComponentCount 4 — quaternion x,y,z,w, slerped on sample.
    //   ComponentCount 3 — Euler XYZ in degrees, lerped per-component.
    // Timeline playback consumes both layouts.
    float32 Value[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    uint8 ComponentCount = 1;
    std::string Label;
    float32 InHandle[2] = {-0.1f, 0.0f};
    float32 OutHandle[2] = {0.1f, 0.0f};
    TimelineBezierHandleMode HandleMode = TimelineBezierHandleMode::Balanced;
};

struct TimelineMethodArgument
{
    std::string Type = "string";
    std::string Value;
};

struct TimelineMethodKey
{
    float32 Time = 0.0f;
    std::string MethodName;
    std::string Arguments;
    std::vector<TimelineMethodArgument> TypedArguments;
};

struct TimelineAudioKey
{
    float32 Time = 0.0f;
    GUID AudioGuid;
    std::filesystem::path SourcePath;
    std::string Name;
    float32 StartOffset = 0.0f;
    float32 EndOffset = 0.0f;
    float32 VolumeDb = 0.0f;
    float32 PitchScale = 1.0f;
};

struct TimelineAnimationKey
{
    float32 Time = 0.0f;
    std::string AnimationName;
    GUID AnimationGuid;
    std::filesystem::path SourcePath;
    float32 SpeedScale = 1.0f;
};

struct TimelineClip
{
    GUID ClipGuid;
    AssetType ClipAssetType = AssetType::Animation;
    std::string Name;
    std::filesystem::path SourcePath;
    float32 OffsetOnTimeline = 0.0f;
    float32 InTime = 0.0f;
    float32 OutTime = 0.0f;
    bool Muted = false;
    float32 FadeInDuration = 0.0f;
    float32 FadeOutDuration = 0.0f;
    std::vector<TimelineMarker> Markers;
};

struct TimelineTrack
{
    TimelineTrackType Type = TimelineTrackType::Animation;
    std::string Name;
    std::string TargetPath;
    std::string PropertyPath;
    std::vector<TimelineClip> Clips;
    std::vector<TimelineValueKey> ValueKeys;
    std::vector<TimelineMethodKey> MethodKeys;
    std::vector<TimelineAudioKey> AudioKeys;
    std::vector<TimelineAnimationKey> AnimationKeys;
    std::vector<TimelineMarker> Markers;
    TimelineTrackUpdateMode UpdateMode = TimelineTrackUpdateMode::Continuous;
    TimelineTrackInterpolation Interpolation = TimelineTrackInterpolation::Linear;
    bool LoopWrap = true;
    bool Enabled = true;
    bool Imported = false;
    bool UseBlend = true;
    uint32 Color = 0;
};

struct Timeline
{
    int32 SchemaVersion = 1;
    std::vector<TimelineTrack> Tracks;
};

struct TimelineEvaluateOptions
{
    bool Loop = false;
    float32 DurationOverride = 0.0f;
};

struct TimelineValueSample
{
    uint32 TrackIndex = 0;
    TimelineTrackType TrackType = TimelineTrackType::Property;
    std::string TrackName;
    std::string TargetPath;
    std::string PropertyPath;
    // Same layout as TimelineValueKey::Value.
    float32 Value[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    uint8 ComponentCount = 1;
};

struct TimelineMethodEvent
{
    uint32 TrackIndex = 0;
    TimelineTrackType TrackType = TimelineTrackType::Method;
    std::string TrackName;
    float32 Time = 0.0f;
    std::string MethodName;
    std::string Arguments;
    std::vector<TimelineMethodArgument> TypedArguments;
};

struct TimelineAudioEvent
{
    uint32 TrackIndex = 0;
    std::string TrackName;
    float32 Time = 0.0f;
    GUID AudioGuid;
    std::filesystem::path SourcePath;
    std::string Name;
    float32 StartOffset = 0.0f;
    float32 EndOffset = 0.0f;
    float32 VolumeDb = 0.0f;
    float32 PitchScale = 1.0f;
    bool UseBlend = true;
};

struct TimelineClipSample
{
    uint32 TrackIndex = 0;
    TimelineTrackType TrackType = TimelineTrackType::Animation;
    GUID ClipGuid;
    AssetType ClipAssetType = AssetType::Animation;
    std::string Name;
    float32 LocalTime = 0.0f;
    float32 Weight = 1.0f;
};

struct TimelineEvaluationResult
{
    std::vector<TimelineValueSample> ValueSamples;
    std::vector<TimelineMethodEvent> MethodEvents;
    std::vector<TimelineAudioEvent> AudioEvents;
    std::vector<TimelineClipSample> ActiveClips;
};

const char* TimelineTrackTypeToString(TimelineTrackType type);
TimelineTrackType TimelineTrackTypeFromString(const std::string& value);
uint8 TimelineTrackTypeComponentCount(TimelineTrackType type);
bool TimelineTrackTypeUsesValueKeys(TimelineTrackType type);
bool TimelineTrackTypeUsesClips(TimelineTrackType type);
bool TimelineTrackTypeUsesAudioKeys(TimelineTrackType type);
bool TimelineTrackTypeUsesAnimationKeys(TimelineTrackType type);

bool LoadTimelineFromJson(const nlohmann::json& doc, Timeline& outTimeline, std::string* outError = nullptr);
nlohmann::json SaveTimelineToJson(const Timeline& timeline);
float32 GetTimelineDuration(const Timeline& timeline);
TimelineEvaluationResult EvaluateTimeline(
    const Timeline& timeline,
    float32 previousTime,
    float32 currentTime,
    const TimelineEvaluateOptions& options = {});

} // namespace Animation
} // namespace GameEngine
