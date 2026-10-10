#pragma once

#include "AssetCore/GUID.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace GameEngine
{

// Default 0xAARRGGBB row tint for composite tracks (#373737).
inline constexpr uint32_t kDefaultSequencerRowColorArgb = 0xFF373737u;

enum class CompositeTrackType : uint8_t
{
    Animation = 0,
    Audio,
    Video,
    Property,
    Position3D,
    Rotation3D,
    Scale3D,
    BlendShape,
    Method,
    Event,
    Bezier,
};

enum class CompositeTrackUpdateMode : uint8_t
{
    Continuous = 0,
    Discrete,
    Capture,
};

enum class CompositeTrackInterpolation : uint8_t
{
    Nearest = 0,
    Linear,
    Cubic,
};

enum class CompositeBezierHandleMode : uint8_t
{
    Free = 0,
    Linear,
    Balanced,
    Mirrored,
};

enum class CompositeMediaSyncMode : uint8_t
{
    Lock = 0,
    KeepOffset,
};

// A named time marker; used on tracks (absolute time) and clips (time relative to clip start).
struct TimelineMarker
{
    float time = 0.0f;
    std::string name;
};

struct CompositeValueKey
{
    float time = 0.0f;
    float value[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    uint8_t componentCount = 1u;
    std::string label;
    float inHandle[2] = {-0.1f, 0.0f};
    float outHandle[2] = {0.1f, 0.0f};
    CompositeBezierHandleMode handleMode = CompositeBezierHandleMode::Balanced;
};

struct CompositeMethodArgument
{
    std::string type = "string";
    std::string value;
};

struct CompositeMethodKey
{
    float time = 0.0f;
    std::string methodName;
    std::string arguments;
    std::vector<CompositeMethodArgument> typedArguments;
};

struct CompositeAudioKey
{
    float time = 0.0f;
    GUID audioGuid;
    std::filesystem::path sourcePath;
    std::string name;
    float startOffset = 0.0f;
    float endOffset = 0.0f;
    float volumeDb = 0.0f;
    float pitchScale = 1.0f;
};

struct CompositeAnimationKey
{
    float time = 0.0f;
    std::string animationName;
    GUID animationGuid;
    std::filesystem::path sourcePath;
    float speedScale = 1.0f;
};

// Editor-only: a clip instance on the composite timeline (reference + in/out + offset).
struct CompositeClip
{
    GUID clipGuid;           // AnimationClip asset
    GUID linkedMediaGroupGuid; // Shared GUID linking video/audio clips imported from the same media file.
    float linkedMediaOffsetSeconds = 0.0f; // Audio->video offset used by KeepOffset sync mode.
    std::string name;
    std::filesystem::path sourcePath; // original media file path (video/audio), empty for animation clips
    float offsetOnTimeline = 0.0f; // start time on the composite
    float inTime = 0.0f;     // trim start within clip
    float outTime = 10.0f;   // trim end within clip
    bool muted = false;
    // Cross-fade ramps. fadeInDuration: gain ramps 0→1 over this many seconds at clip start;
    // fadeOutDuration: gain ramps 1→0 over this many seconds at clip end. Overlapping clips with
    // matching out/in fades produce a cross-fade.
    float fadeInDuration = 0.0f;
    float fadeOutDuration = 0.0f;
    std::vector<TimelineMarker> markers; // times relative to clip start (0 = clip start)
};

// Editor-only: a track containing clips.
struct CompositeTrack
{
    CompositeTrackType type = CompositeTrackType::Animation;
    std::string name;
    std::string targetPath;
    std::string propertyPath;
    std::vector<CompositeClip> clips;
    std::vector<CompositeValueKey> valueKeys;
    std::vector<CompositeMethodKey> methodKeys;
    std::vector<CompositeAudioKey> audioKeys;
    std::vector<CompositeAnimationKey> animationKeys;
    std::vector<TimelineMarker> markers; // absolute times on the timeline
    CompositeTrackUpdateMode updateMode = CompositeTrackUpdateMode::Continuous;
    CompositeTrackInterpolation interpolation = CompositeTrackInterpolation::Linear;
    bool loopWrap = true;
    bool enabled = true;
    bool imported = false;
    bool useBlend = true; // Godot-style audio track blend/mix toggle.
    bool syncLinkedVideo = true; // Audio track toggle: keep linked audio/video clips time-synced.
    CompositeMediaSyncMode mediaSyncMode = CompositeMediaSyncMode::Lock;
  // 0xAARRGGBB row tint. Defaults to a neutral dark gray that matches the
  // editor's normal track row background; users override via the sidebar swatch.
  uint32_t color = kDefaultSequencerRowColorArgb;
};

// Editor-only: composite timeline model (multiple tracks).
struct TimeCompositeModel
{
    std::vector<CompositeTrack> tracks;
};

// JSON serialization for the .timeline asset format. Schema is intentionally
// flat and human-readable — referenced clips are stored by GUID string so the
// timeline asset round-trips through the AssetCore registry like any other.
class TimelineAsset;
const char* CompositeTrackTypeToString(CompositeTrackType type);
const char* CompositeTrackTypeDisplayName(CompositeTrackType type);
CompositeTrackType CompositeTrackTypeFromString(const std::string& type);
bool CompositeTrackTypeUsesClips(CompositeTrackType type);
bool CompositeTrackTypeUsesValueKeys(CompositeTrackType type);
bool CompositeTrackTypeUsesAudioKeys(CompositeTrackType type);
bool CompositeTrackTypeUsesMethodKeys(CompositeTrackType type);
bool CompositeTrackTypeUsesAnimationKeys(CompositeTrackType type);
uint8_t CompositeTrackTypeComponentCount(CompositeTrackType type);
const char* CompositeTrackUpdateModeToString(CompositeTrackUpdateMode mode);
CompositeTrackUpdateMode CompositeTrackUpdateModeFromString(const std::string& value);
const char* CompositeTrackInterpolationToString(CompositeTrackInterpolation interpolation);
CompositeTrackInterpolation CompositeTrackInterpolationFromString(const std::string& value);
const char* CompositeBezierHandleModeToString(CompositeBezierHandleMode mode);
CompositeBezierHandleMode CompositeBezierHandleModeFromString(const std::string& value);
const char* CompositeMediaSyncModeToString(CompositeMediaSyncMode mode);
CompositeMediaSyncMode CompositeMediaSyncModeFromString(const std::string& value);
nlohmann::json SerializeTimeCompositeModel(const TimeCompositeModel& model);
bool DeserializeTimeCompositeModel(const nlohmann::json& doc, TimeCompositeModel& outModel);

} // namespace GameEngine
