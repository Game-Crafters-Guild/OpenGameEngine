#pragma once

#include "Animation/AnimationEvent.h"
#include "AssetCore/Asset.h"
#include "Types/Types.h"
#include "Types/StringId.h"
#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine
{
namespace Animation
{

enum class AnimPath
{
    Translation,
    Rotation,
    Scale,
    MorphWeight
};

enum class AnimInterp
{
    Linear,
    Step,
    CubicSpline
};

// Per-tangent-handle type. Auto=0 so memset-initialized keyframes default to Auto.
enum class AnimTangentType : uint8
{
    Auto,      // smooth Catmull-Rom slope, anti-overshoot
    Plateau,   // same but zero at local extrema (peaks/valleys)
    Clamped,   // spline slope, but linear when adjacent values are very close
    Fixed,     // preserves world-space angle when key moves; only weight adjusts
    Linear,    // straight slope to the adjacent key
    Flat,      // zero slope
    Custom,    // user-edited; never auto-recomputed
};

// Curve extrapolation beyond the first/last keyframe. Constant=0 so default-constructed channels hold.
enum class AnimExtrapolation : uint8
{
    Constant,        // hold first/last key value
    Linear,          // project along terminal tangent
    Cycle,           // repeat [firstTime..lastTime] range
    CycleWithOffset, // cycle + accumulate value offset per cycle
    Oscillate,       // ping-pong (mirror alternate cycles)
};

// Keyframe data layout (compact); real impl may use SoA for perf
struct AnimKeyframe
{
    float32 time; // seconds
    float32 translation[3];
    float32 rotation[4]; // quaternion
    float32 scale[3];
    float32 inTangent[4];
    float32 outTangent[4];
    float32 inWeight[4];
    float32 outWeight[4];
    // Per-component tangent types; Auto=0 so memset-initialized keyframes get Auto by default.
    AnimTangentType inTangentType[4];
    AnimTangentType outTangentType[4];
    // Bitmask: bit i set means component i has independent in/out tangents (broken tangent).
    uint8 tangentBroken;
    // Per-key segment interpolation override (controls the segment from this key to the next).
    AnimInterp segmentInterp = AnimInterp::Linear;
    bool hasSegmentInterpOverride = false;
};

struct AnimChannel
{
    uint32 boneIndex;                       // legacy fallback (clip's source-skeleton index)
    StringId targetNameId = 0;              // hashed at clip load; primary key for sample-time
                                            // name resolution. 0 = "no name; use boneIndex".
    String targetName;                     // editor display + JSON round-trip
    AnimPath path;                          // which TRS component this channel animates
    AnimInterp interp = AnimInterp::Linear; // glTF sampler interpolation
    std::vector<AnimKeyframe> keys;         // sorted by time
    AnimExtrapolation preInfinity  = AnimExtrapolation::Constant; // behavior before first key
    AnimExtrapolation postInfinity = AnimExtrapolation::Constant; // behavior after last key
};

/// How a clip's translation keys apply to the skeleton that plays it.
enum class ClipTranslationMode : uint8
{
    Absolute,     ///< A key is the bone's local translation.
    RestRelative, ///< A key moves the bone from the skeleton's rest by the key's offset from the clip's rest.
};

/// The bone whose motion a clip hands to the entity that plays it, and which parts of that motion.
struct ClipRootMotion
{
    String Bone;
    bool Translation = true;
    bool Rotation = true;
};

/**
 * @brief The playback settings a clip's source file declares.
 *
 * glTF clips read them from the animation's `extras.clip` block (schemaVersion 1); a value the file does not
 * declare stays empty. Loop and speed have the lowest precedence: the Animator, an animation library entry, a
 * controller state's motion, a graph's clip player node and a montage's play rate each carry their own, and theirs
 * is the one playback uses.
 */
struct AnimationClipSettings
{
    std::optional<bool> Loop;
    std::optional<float32> Speed;
    std::optional<ClipRootMotion> RootMotion;
    ClipTranslationMode Translations = ClipTranslationMode::Absolute;
};

class AnimationClip : public ::GameEngine::Asset
{
  public:
    AnimationClip(const ::GameEngine::GUID& guid, const std::filesystem::path& path)
        : ::GameEngine::Asset(guid, ::GameEngine::AssetType::Animation, path) {}

    bool Load() override; // parse clip from file/container (gltf/fbx)
    bool LoadFromData(const Vector<uint8>& data) override;
    void Unload() override;

    float32 GetDuration() const
    {
        return m_Duration;
    }
    void SetDuration(float32 duration)
    {
        m_Duration = std::max(0.0f, duration);
    }
    const std::vector<AnimChannel>& GetChannels() const
    {
        return m_Channels;
    }
    uint32 GetSelectedAnimationIndex() const
    {
        return m_SelectedAnimation;
    }
    const std::filesystem::path& GetSourcePath() const
    {
        return m_SourcePath;
    }
    uint32 GetSourceAnimationIndex() const
    {
        return m_SourceAnimationIndex;
    }
    /// The settings the source file declares; empty for a format that declares none (every format but glTF).
    const AnimationClipSettings& GetSettings() const
    {
        return m_Settings;
    }
    /// The events the source file declares, sorted by time in seconds; empty for every format but glTF.
    const AnimationEventTrack& GetEventTrack() const
    {
        return m_EventTrack;
    }
    bool IsEditable() const;

    // Select which animation index to parse from a container (e.g., glTF)
    void SetSelectedAnimationIndex(uint32 idx)
    {
        m_SelectedAnimation = idx;
    }
    void SetSourceInfo(const std::filesystem::path& sourcePath, uint32 sourceAnimationIndex);
    // Copies the keys, duration and source of `other` into this editable clip. The settings and events are left
    // empty: the editable format (.anim) does not hold them.
    void CopyFrom(const AnimationClip& other);

    // Test-only helper: allow injecting channels/duration without parsing
    void SetChannelsAndDurationForTest(const std::vector<AnimChannel>& channels, float32 duration);

    // Editor: set keyframe time (e.g. when dragging in dope sheet). Finds key by currentTime (within epsilon), sets to newTime, re-sorts and updates duration.
    bool SetKeyframeTime(size_t channelIndex, float currentTime, float newTime);
    bool SetKeyframeComponentValue(size_t channelIndex, float currentTime, uint32 componentIndex, float value);
    bool SetKeyframeTangent(size_t channelIndex, float currentTime, uint32 componentIndex, bool incoming, float tangent, float weight);
    bool AddKeyframe(size_t channelIndex, float time);
    bool AddKeyframeWithData(size_t channelIndex, const AnimKeyframe& keyframe);
    bool DuplicateKeyframe(size_t channelIndex, float currentTime, float newTime);
    bool RemoveKeyframe(size_t channelIndex, float currentTime);
    bool SetChannelInterpolation(size_t channelIndex, AnimInterp interpolation);
    bool SetChannelExtrapolation(size_t channelIndex, AnimExtrapolation pre, AnimExtrapolation post);
    bool SetKeyframeTangentType(size_t channelIndex, float keyTime, uint32 componentIndex, bool incoming, AnimTangentType type);
    bool SetKeyframeTangentBroken(size_t channelIndex, float keyTime, uint32 componentIndex, bool broken);
    bool SetKeyframeSegmentInterp(size_t channelIndex, float keyTime, AnimInterp interp);
    // Converts baked-style keys to curves: Step or Linear → CubicSpline (+ tangents) for Translation/Scale; Step → Linear for Rotation (quaternion-safe).
    bool ResampleBakedChannelToCurve(size_t channelIndex);
    size_t FindOrAddChannel(uint32 boneIndex, const String& targetName, AnimPath path);
    bool SaveToData(Vector<uint8>& outData) const;
    bool SaveToPath(const std::filesystem::path& path) const;
    bool RestoreFromData(const Vector<uint8>& data);

    // Returns the names of all animations in the loaded source file.
    // Only meaningful for container formats (glTF/FBX); returns a single entry for .anim files.
    // Requires the clip to have been loaded at least once (uses cached file data).
    std::vector<std::string> EnumerateAnimationNames() const;

  private:
    bool LoadEditableData(const Vector<uint8>& data);
    void RecomputeDuration();

    uint32 m_SelectedAnimation = 0u;
    std::filesystem::path m_SourcePath;
    uint32 m_SourceAnimationIndex = 0u;

    float32 m_Duration = 0.0f;
    std::vector<AnimChannel> m_Channels;
    AnimationClipSettings m_Settings;
    AnimationEventTrack m_EventTrack;

    // Cached raw file data to avoid re-reading from disk when switching
    // animation indices via Unload()/SetSelectedAnimationIndex()/Load().
    // Intentionally retained across Unload() and freed with the clip. Only Load()
    // fills it: a clip parsed from bytes its caller holds (LoadFromData) keeps no
    // copy of the file.
    Vector<uint8> m_CachedFileData;
};

} // namespace Animation
} // namespace GameEngine
