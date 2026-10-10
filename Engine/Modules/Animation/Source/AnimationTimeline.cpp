#include "Animation/AnimationTimeline.h"

#include "Mathematics/Quaternion.h"

#include <algorithm>
#include <cmath>

namespace GameEngine
{
namespace Animation
{

namespace
{

using json = nlohmann::json;

constexpr uint8 kQuaternionComponentCount = 4;

float32 Clamp01(float32 value)
{
    return std::max(0.0f, std::min(1.0f, value));
}

bool TrackSlerpsAsQuaternion(const TimelineTrack& track, uint8 componentCount)
{
    if (componentCount != kQuaternionComponentCount)
        return false;
    if (track.Type == TimelineTrackType::Rotation3D)
        return true;
    return track.PropertyPath == kTransformRotationPropertyPath
        && (track.Type == TimelineTrackType::Property || track.Type == TimelineTrackType::Bezier);
}

void SlerpQuaternionKeys(const TimelineValueKey& left,
                         const TimelineValueKey& right,
                         float32 t,
                         TimelineValueSample& sample)
{
    using Mathematics::Quaternion;
    const Quaternion leftRotation(left.Value[3], left.Value[0], left.Value[1], left.Value[2]);
    const Quaternion rightRotation(right.Value[3], right.Value[0], right.Value[1], right.Value[2]);
    const glm::quat blended = Quaternion::Slerp(leftRotation, rightRotation, t).GetGLM();
    sample.Value[0] = blended.x;
    sample.Value[1] = blended.y;
    sample.Value[2] = blended.z;
    sample.Value[3] = blended.w;
}

float32 WrapTime(float32 time, float32 duration)
{
    if (duration <= 0.0f)
        return time;

    float32 wrapped = std::fmod(time, duration);
    if (wrapped < 0.0f)
        wrapped += duration;
    return wrapped;
}

bool IsTrackEventInRange(float32 keyTime, float32 previousTime, float32 currentTime, float32 duration, bool loop)
{
    if (loop && duration > 0.0f)
    {
        const float32 prev = WrapTime(previousTime, duration);
        const float32 curr = WrapTime(currentTime, duration);
        if (prev <= curr)
            return keyTime > prev && keyTime <= curr;
        return keyTime > prev || keyTime <= curr;
    }

    if (previousTime <= currentTime)
        return keyTime > previousTime && keyTime <= currentTime;
    return keyTime > currentTime && keyTime <= previousTime;
}

float32 ReadFloat(const json& doc, const char* key, float32 fallback)
{
    if (!doc.contains(key) || !doc[key].is_number())
        return fallback;
    return doc[key].get<float32>();
}

uint32 ReadColor(const json& doc)
{
    if (!doc.contains("color") || !doc["color"].is_number_unsigned())
        return 0;
    return doc["color"].get<uint32>();
}

AssetType TimelineAssetTypeFromString(const std::string& value)
{
    if (value == "Timeline") return AssetType::Timeline;
    if (value == "ClipSet") return AssetType::ClipSet;
    if (value == "Audio") return AssetType::Audio;
    if (value == "AnimationController") return AssetType::AnimationController;
    return AssetType::Animation;
}

void SortTrackKeys(TimelineTrack& track)
{
    std::sort(track.ValueKeys.begin(), track.ValueKeys.end(),
        [](const TimelineValueKey& a, const TimelineValueKey& b) { return a.Time < b.Time; });
    std::sort(track.MethodKeys.begin(), track.MethodKeys.end(),
        [](const TimelineMethodKey& a, const TimelineMethodKey& b) { return a.Time < b.Time; });
    std::sort(track.AudioKeys.begin(), track.AudioKeys.end(),
        [](const TimelineAudioKey& a, const TimelineAudioKey& b) { return a.Time < b.Time; });
    std::sort(track.AnimationKeys.begin(), track.AnimationKeys.end(),
        [](const TimelineAnimationKey& a, const TimelineAnimationKey& b) { return a.Time < b.Time; });
    std::sort(track.Markers.begin(), track.Markers.end(),
        [](const TimelineMarker& a, const TimelineMarker& b) { return a.Time < b.Time; });
    for (auto& clip : track.Clips)
    {
        std::sort(clip.Markers.begin(), clip.Markers.end(),
            [](const TimelineMarker& a, const TimelineMarker& b) { return a.Time < b.Time; });
    }
}

TimelineMarker LoadMarker(const json& doc)
{
    TimelineMarker marker;
    marker.Time = ReadFloat(doc, "time", 0.0f);
    marker.Name = doc.value("name", std::string());
    return marker;
}

json SaveMarker(const TimelineMarker& marker)
{
    return json{
        {"time", marker.Time},
        {"name", marker.Name}
    };
}

TimelineValueKey LoadValueKey(const json& doc, TimelineTrackType trackType)
{
    TimelineValueKey key;
    key.Time = ReadFloat(doc, "time", 0.0f);
    key.ComponentCount = TimelineTrackTypeComponentCount(trackType);
    key.Label = doc.value("label", std::string());

    if (doc.contains("componentCount") && doc["componentCount"].is_number_unsigned())
        key.ComponentCount = std::max<uint8>(1, std::min<uint8>(4, doc["componentCount"].get<uint8>()));

    if (doc.contains("value") && doc["value"].is_array())
    {
        const uint8 count = static_cast<uint8>(std::min<size_t>(4, doc["value"].size()));
        for (uint8 i = 0; i < count; ++i)
        {
            if (doc["value"][i].is_number())
                key.Value[i] = doc["value"][i].get<float32>();
        }
        if (!doc.contains("componentCount"))
            key.ComponentCount = std::max<uint8>(1, count);
    }
    else if (doc.contains("value") && doc["value"].is_number())
    {
        key.Value[0] = doc["value"].get<float32>();
        key.ComponentCount = 1;
    }

    if (doc.contains("inHandle") && doc["inHandle"].is_array() && doc["inHandle"].size() >= 2)
    {
        if (doc["inHandle"][0].is_number()) key.InHandle[0] = doc["inHandle"][0].get<float32>();
        if (doc["inHandle"][1].is_number()) key.InHandle[1] = doc["inHandle"][1].get<float32>();
    }
    if (doc.contains("outHandle") && doc["outHandle"].is_array() && doc["outHandle"].size() >= 2)
    {
        if (doc["outHandle"][0].is_number()) key.OutHandle[0] = doc["outHandle"][0].get<float32>();
        if (doc["outHandle"][1].is_number()) key.OutHandle[1] = doc["outHandle"][1].get<float32>();
    }
    const std::string handleMode = doc.value("handleMode", std::string("balanced"));
    if (handleMode == "free") key.HandleMode = TimelineBezierHandleMode::Free;
    else if (handleMode == "linear") key.HandleMode = TimelineBezierHandleMode::Linear;
    else if (handleMode == "mirrored") key.HandleMode = TimelineBezierHandleMode::Mirrored;
    else key.HandleMode = TimelineBezierHandleMode::Balanced;

    return key;
}

json SaveValueKey(const TimelineValueKey& key)
{
    json value = json::array();
    for (uint8 i = 0; i < key.ComponentCount && i < 4; ++i)
        value.push_back(key.Value[i]);

    json doc;
    doc["time"] = key.Time;
    doc["value"] = std::move(value);
    doc["componentCount"] = key.ComponentCount;
    if (!key.Label.empty())
        doc["label"] = key.Label;
    doc["inHandle"] = {key.InHandle[0], key.InHandle[1]};
    doc["outHandle"] = {key.OutHandle[0], key.OutHandle[1]};
    switch (key.HandleMode)
    {
        case TimelineBezierHandleMode::Free: doc["handleMode"] = "free"; break;
        case TimelineBezierHandleMode::Linear: doc["handleMode"] = "linear"; break;
        case TimelineBezierHandleMode::Mirrored: doc["handleMode"] = "mirrored"; break;
        case TimelineBezierHandleMode::Balanced: default: doc["handleMode"] = "balanced"; break;
    }
    return doc;
}

TimelineMethodKey LoadMethodKey(const json& doc)
{
    TimelineMethodKey key;
    key.Time = ReadFloat(doc, "time", 0.0f);
    key.MethodName = doc.value("methodName", std::string());
    key.Arguments = doc.value("arguments", std::string());
    if (doc.contains("typedArguments") && doc["typedArguments"].is_array())
    {
        for (const auto& argJson : doc["typedArguments"])
        {
            if (!argJson.is_object()) continue;
            key.TypedArguments.push_back({
                argJson.value("type", std::string("string")),
                argJson.value("value", std::string())
            });
        }
    }
    return key;
}

json SaveMethodKey(const TimelineMethodKey& key)
{
    json doc{
        {"time", key.Time},
        {"methodName", key.MethodName},
        {"arguments", key.Arguments}
    };
    if (!key.TypedArguments.empty())
    {
        json args = json::array();
        for (const auto& arg : key.TypedArguments)
            args.push_back({{"type", arg.Type}, {"value", arg.Value}});
        doc["typedArguments"] = std::move(args);
    }
    return doc;
}

TimelineAudioKey LoadAudioKey(const json& doc)
{
    TimelineAudioKey key;
    key.Time = ReadFloat(doc, "time", 0.0f);
    if (doc.contains("audioGuid") && doc["audioGuid"].is_string())
        key.AudioGuid = GUID(doc["audioGuid"].get<std::string>());
    key.SourcePath = doc.value("sourcePath", std::string());
    key.Name = doc.value("name", std::string());
    key.StartOffset = std::max(0.0f, ReadFloat(doc, "startOffset", 0.0f));
    key.EndOffset = std::max(0.0f, ReadFloat(doc, "endOffset", 0.0f));
    key.VolumeDb = ReadFloat(doc, "volumeDb", 0.0f);
    key.PitchScale = ReadFloat(doc, "pitchScale", 1.0f);
    return key;
}

json SaveAudioKey(const TimelineAudioKey& key)
{
    return json{
        {"time", key.Time},
        {"audioGuid", key.AudioGuid.ToString()},
        {"sourcePath", key.SourcePath.string()},
        {"name", key.Name},
        {"startOffset", key.StartOffset},
        {"endOffset", key.EndOffset},
        {"volumeDb", key.VolumeDb},
        {"pitchScale", key.PitchScale}
    };
}

TimelineAnimationKey LoadAnimationKey(const json& doc)
{
    TimelineAnimationKey key;
    key.Time = ReadFloat(doc, "time", 0.0f);
    key.AnimationName = doc.value("animationName", std::string());
    if (doc.contains("animationGuid") && doc["animationGuid"].is_string())
        key.AnimationGuid = GUID(doc["animationGuid"].get<std::string>());
    key.SourcePath = doc.value("sourcePath", std::string());
    key.SpeedScale = ReadFloat(doc, "speedScale", 1.0f);
    return key;
}

json SaveAnimationKey(const TimelineAnimationKey& key)
{
    return json{
        {"time", key.Time},
        {"animationName", key.AnimationName},
        {"animationGuid", key.AnimationGuid.ToString()},
        {"sourcePath", key.SourcePath.string()},
        {"speedScale", key.SpeedScale}
    };
}

TimelineClip LoadClip(const json& doc)
{
    TimelineClip clip;
    clip.Name = doc.value("name", std::string());
    clip.SourcePath = doc.value("sourcePath", std::string());
    clip.OffsetOnTimeline = ReadFloat(doc, "offsetOnTimeline", 0.0f);
    clip.InTime = ReadFloat(doc, "inTime", 0.0f);
    clip.OutTime = ReadFloat(doc, "outTime", clip.InTime);
    clip.Muted = doc.value("muted", false);
    clip.FadeInDuration = std::max(0.0f, ReadFloat(doc, "fadeInDuration", 0.0f));
    clip.FadeOutDuration = std::max(0.0f, ReadFloat(doc, "fadeOutDuration", 0.0f));

    if (doc.contains("clipGuid") && doc["clipGuid"].is_string())
        clip.ClipGuid = GUID(doc["clipGuid"].get<std::string>());
    clip.ClipAssetType = TimelineAssetTypeFromString(doc.value("assetType", doc.value("type", std::string("Animation"))));

    if (doc.contains("markers") && doc["markers"].is_array())
    {
        for (const auto& markerJson : doc["markers"])
            clip.Markers.push_back(LoadMarker(markerJson));
    }

    return clip;
}

json SaveClip(const TimelineClip& clip)
{
    json doc;
    doc["clipGuid"] = clip.ClipGuid.ToString();
    doc["assetType"] = AssetTypeToString(clip.ClipAssetType);
    doc["name"] = clip.Name;
    doc["sourcePath"] = clip.SourcePath.string();
    doc["offsetOnTimeline"] = clip.OffsetOnTimeline;
    doc["inTime"] = clip.InTime;
    doc["outTime"] = clip.OutTime;
    doc["muted"] = clip.Muted;
    doc["fadeInDuration"] = clip.FadeInDuration;
    doc["fadeOutDuration"] = clip.FadeOutDuration;

    json markers = json::array();
    for (const auto& marker : clip.Markers)
        markers.push_back(SaveMarker(marker));
    doc["markers"] = std::move(markers);
    return doc;
}

// Handles are offsets in (seconds, value). Clamp their time coordinates to
// this segment so the curve remains a single-valued function of time.
float32 SampleBezierSegment(const TimelineValueKey& left, const TimelineValueKey& right,
                           uint8 component, float32 time)
{
    const double duration = static_cast<double>(right.Time) - left.Time;
    auto cubic = [](double a, double b, double c, double d, double t) {
        const double s = 1.0 - t;
        return s * s * s * a + 3.0 * s * s * t * b + 3.0 * s * t * t * c + t * t * t * d;
    };
    double x1 = std::clamp(static_cast<double>(left.OutHandle[0]), 0.0, duration);
    double x2 = std::clamp(duration + right.InHandle[0], 0.0, duration);
    double y1 = left.Value[component] + left.OutHandle[1];
    double y2 = right.Value[component] + right.InHandle[1];
    if (left.HandleMode == TimelineBezierHandleMode::Linear)
    {
        x1 = duration / 3.0;
        y1 = left.Value[component] + (right.Value[component] - left.Value[component]) / 3.0;
    }
    if (right.HandleMode == TimelineBezierHandleMode::Linear)
    {
        x2 = duration * 2.0 / 3.0;
        y2 = left.Value[component] + (right.Value[component] - left.Value[component]) * 2.0 / 3.0;
    }
    double low = 0.0;
    double high = 1.0;
    const double target = static_cast<double>(time) - left.Time;
    for (int iteration = 0; iteration < 40; ++iteration)
    {
        const double t = (low + high) * 0.5;
        if (cubic(0.0, x1, x2, duration, t) < target) low = t;
        else high = t;
    }
    return static_cast<float32>(cubic(left.Value[component], y1, y2, right.Value[component], (low + high) * 0.5));
}

void SampleValueTrack(const TimelineTrack& track, uint32 trackIndex, float32 time, TimelineEvaluationResult& result)
{
    if (track.ValueKeys.empty())
        return;

    TimelineValueSample sample;
    sample.TrackIndex = trackIndex;
    sample.TrackType = track.Type;
    sample.TrackName = track.Name;
    sample.TargetPath = track.TargetPath;
    sample.PropertyPath = track.PropertyPath;

    const auto& keys = track.ValueKeys;
    if (time <= keys.front().Time || keys.size() == 1)
    {
        sample.ComponentCount = keys.front().ComponentCount;
        std::copy(std::begin(keys.front().Value), std::end(keys.front().Value), std::begin(sample.Value));
        result.ValueSamples.push_back(std::move(sample));
        return;
    }
    if (time >= keys.back().Time)
    {
        sample.ComponentCount = keys.back().ComponentCount;
        std::copy(std::begin(keys.back().Value), std::end(keys.back().Value), std::begin(sample.Value));
        result.ValueSamples.push_back(std::move(sample));
        return;
    }

    auto upper = std::upper_bound(keys.begin(), keys.end(), time,
        [](float32 value, const TimelineValueKey& key) { return value < key.Time; });
    const TimelineValueKey& right = *upper;
    const TimelineValueKey& left = *(upper - 1);
    const float32 denom = std::max(0.00001f, right.Time - left.Time);
    float32 t = Clamp01((time - left.Time) / denom);
    if (track.UpdateMode == TimelineTrackUpdateMode::Discrete)
        t = 0.0f;
    else if (track.Interpolation == TimelineTrackInterpolation::Nearest)
        t = (t < 0.5f) ? 0.0f : 1.0f;
    else if (track.Interpolation == TimelineTrackInterpolation::Cubic)
        t = t * t * (3.0f - 2.0f * t);
    sample.ComponentCount = std::max<uint8>(1, std::min<uint8>(4, std::max(left.ComponentCount, right.ComponentCount)));
    if (TrackSlerpsAsQuaternion(track, sample.ComponentCount))
    {
        // Four-component rotation is quaternion x,y,z,w. Slerp, not
        // per-component lerp: a raw lerp of the four floats does not stay
        // unit-length and visibly shrinks and wobbles mid-turn.
        SlerpQuaternionKeys(left, right, t, sample);
    }
    else
    {
        for (uint8 i = 0; i < sample.ComponentCount; ++i)
        {
            if (track.Type == TimelineTrackType::Bezier && track.UpdateMode != TimelineTrackUpdateMode::Discrete)
                sample.Value[i] = SampleBezierSegment(left, right, i, time);
            else
                sample.Value[i] = left.Value[i] + (right.Value[i] - left.Value[i]) * t;
        }
    }
    result.ValueSamples.push_back(std::move(sample));
}

void SampleAnimationKeyTrack(const TimelineTrack& track, uint32 trackIndex, float32 time, TimelineEvaluationResult& result)
{
    if (track.AnimationKeys.empty())
        return;

    const TimelineAnimationKey* active = nullptr;
    for (const auto& key : track.AnimationKeys)
    {
        if (key.Time <= time)
            active = &key;
        else
            break;
    }
    if (!active)
        return;

    TimelineClipSample sample;
    sample.TrackIndex = trackIndex;
    sample.TrackType = TimelineTrackType::Animation;
    sample.ClipGuid = active->AnimationGuid;
    sample.ClipAssetType = AssetType::Animation;
    sample.Name = active->AnimationName;
    sample.LocalTime = std::max(0.0f, (time - active->Time) * std::max(0.0f, active->SpeedScale));
    sample.Weight = 1.0f;
    result.ActiveClips.push_back(std::move(sample));
}

void SampleClipTrack(const TimelineTrack& track, uint32 trackIndex, float32 time, TimelineEvaluationResult& result)
{
    for (const auto& clip : track.Clips)
    {
        if (clip.Muted)
            continue;

        const float32 length = std::max(0.0f, clip.OutTime - clip.InTime);
        const float32 clipStart = clip.OffsetOnTimeline;
        const float32 clipEnd = clipStart + length;
        if (length <= 0.0f || time < clipStart || time > clipEnd)
            continue;

        const float32 elapsed = time - clipStart;
        const float32 remaining = clipEnd - time;
        float32 weight = 1.0f;
        if (clip.FadeInDuration > 0.0f)
            weight = std::min(weight, elapsed / clip.FadeInDuration);
        if (clip.FadeOutDuration > 0.0f)
            weight = std::min(weight, remaining / clip.FadeOutDuration);

        TimelineClipSample sample;
        sample.TrackIndex = trackIndex;
        sample.TrackType = track.Type;
        sample.ClipGuid = clip.ClipGuid;
        sample.ClipAssetType = clip.ClipAssetType;
        sample.Name = clip.Name;
        sample.LocalTime = clip.InTime + elapsed;
        sample.Weight = Clamp01(weight);
        result.ActiveClips.push_back(std::move(sample));
    }
}

} // namespace

const char* TimelineTrackTypeToString(TimelineTrackType type)
{
    switch (type)
    {
        case TimelineTrackType::Animation: return "animation";
        case TimelineTrackType::Audio: return "audio";
        case TimelineTrackType::Property: return "property";
        case TimelineTrackType::Position3D: return "position3d";
        case TimelineTrackType::Rotation3D: return "rotation3d";
        case TimelineTrackType::Scale3D: return "scale3d";
        case TimelineTrackType::BlendShape: return "blendShape";
        case TimelineTrackType::Method: return "method";
        case TimelineTrackType::Event: return "event";
        case TimelineTrackType::Bezier: return "bezier";
    }
    return "animation";
}

TimelineTrackType TimelineTrackTypeFromString(const std::string& value)
{
    if (value == "audio") return TimelineTrackType::Audio;
    if (value == "property") return TimelineTrackType::Property;
    if (value == "position3d") return TimelineTrackType::Position3D;
    if (value == "rotation3d") return TimelineTrackType::Rotation3D;
    if (value == "scale3d") return TimelineTrackType::Scale3D;
    if (value == "blendShape") return TimelineTrackType::BlendShape;
    if (value == "method") return TimelineTrackType::Method;
    if (value == "event" || value == "animationEvent") return TimelineTrackType::Event;
    if (value == "bezier") return TimelineTrackType::Bezier;
    return TimelineTrackType::Animation;
}

uint8 TimelineTrackTypeComponentCount(TimelineTrackType type)
{
    switch (type)
    {
        case TimelineTrackType::Position3D:
        case TimelineTrackType::Scale3D:
            return 3;
        case TimelineTrackType::Rotation3D:
            return 4;
        case TimelineTrackType::Property:
        case TimelineTrackType::BlendShape:
        case TimelineTrackType::Bezier:
            return 1;
        default:
            return 0;
    }
}

bool TimelineTrackTypeUsesValueKeys(TimelineTrackType type)
{
    return type == TimelineTrackType::Property ||
           type == TimelineTrackType::Position3D ||
           type == TimelineTrackType::Rotation3D ||
           type == TimelineTrackType::Scale3D ||
           type == TimelineTrackType::BlendShape ||
           type == TimelineTrackType::Bezier;
}

bool TimelineTrackTypeUsesClips(TimelineTrackType type)
{
    return type == TimelineTrackType::Animation || type == TimelineTrackType::Audio;
}

bool TimelineTrackTypeUsesAudioKeys(TimelineTrackType type)
{
    return type == TimelineTrackType::Audio;
}

bool TimelineTrackTypeUsesAnimationKeys(TimelineTrackType type)
{
    return type == TimelineTrackType::Animation;
}

bool LoadTimelineFromJson(const nlohmann::json& doc, Timeline& outTimeline, std::string* outError)
{
    if (!doc.is_object())
    {
        if (outError) *outError = "Timeline document must be a JSON object";
        return false;
    }

    Timeline timeline;
    timeline.SchemaVersion = doc.value("schemaVersion", 1);

    if (doc.contains("tracks") && doc["tracks"].is_array())
    {
        for (const auto& trackJson : doc["tracks"])
        {
            const std::string trackTypeString = trackJson.value("type", std::string("animation"));
            if (trackTypeString == "video")
                continue;

            TimelineTrack track;
            track.Type = TimelineTrackTypeFromString(trackTypeString);
            track.Name = trackJson.value("name", std::string());
            track.TargetPath = trackJson.value("targetPath", std::string());
            track.PropertyPath = trackJson.value("propertyPath", std::string());
            track.Color = ReadColor(trackJson);
            track.Enabled = trackJson.value("enabled", true);
            track.Imported = trackJson.value("imported", false);
            const std::string updateMode = trackJson.value("updateMode", std::string("continuous"));
            if (updateMode == "discrete") track.UpdateMode = TimelineTrackUpdateMode::Discrete;
            else if (updateMode == "capture") track.UpdateMode = TimelineTrackUpdateMode::Capture;
            else track.UpdateMode = TimelineTrackUpdateMode::Continuous;
            const std::string interpolation = trackJson.value("interpolation", std::string("linear"));
            if (interpolation == "nearest") track.Interpolation = TimelineTrackInterpolation::Nearest;
            else if (interpolation == "cubic") track.Interpolation = TimelineTrackInterpolation::Cubic;
            else track.Interpolation = TimelineTrackInterpolation::Linear;
            track.LoopWrap = trackJson.value("loopWrap", true);
            track.UseBlend = trackJson.value("useBlend", true);

            if (trackJson.contains("markers") && trackJson["markers"].is_array())
            {
                for (const auto& markerJson : trackJson["markers"])
                    track.Markers.push_back(LoadMarker(markerJson));
            }
            if (trackJson.contains("clips") && trackJson["clips"].is_array())
            {
                for (const auto& clipJson : trackJson["clips"])
                    track.Clips.push_back(LoadClip(clipJson));
            }
            if (trackJson.contains("valueKeys") && trackJson["valueKeys"].is_array())
            {
                for (const auto& keyJson : trackJson["valueKeys"])
                    track.ValueKeys.push_back(LoadValueKey(keyJson, track.Type));
            }
            if (trackJson.contains("methodKeys") && trackJson["methodKeys"].is_array())
            {
                for (const auto& keyJson : trackJson["methodKeys"])
                    track.MethodKeys.push_back(LoadMethodKey(keyJson));
            }
            if (trackJson.contains("audioKeys") && trackJson["audioKeys"].is_array())
            {
                for (const auto& keyJson : trackJson["audioKeys"])
                    track.AudioKeys.push_back(LoadAudioKey(keyJson));
            }
            if (trackJson.contains("animationKeys") && trackJson["animationKeys"].is_array())
            {
                for (const auto& keyJson : trackJson["animationKeys"])
                    track.AnimationKeys.push_back(LoadAnimationKey(keyJson));
            }

            SortTrackKeys(track);
            timeline.Tracks.push_back(std::move(track));
        }
    }

    outTimeline = std::move(timeline);
    return true;
}

nlohmann::json SaveTimelineToJson(const Timeline& timeline)
{
    json doc;
    doc["schemaVersion"] = timeline.SchemaVersion;
    doc["assetType"] = "Timeline";

    json tracks = json::array();
    for (const auto& track : timeline.Tracks)
    {
        json trackJson;
        trackJson["type"] = TimelineTrackTypeToString(track.Type);
        trackJson["name"] = track.Name;
        if (!track.TargetPath.empty())
            trackJson["targetPath"] = track.TargetPath;
        if (!track.PropertyPath.empty())
            trackJson["propertyPath"] = track.PropertyPath;
        if (track.Color != 0)
            trackJson["color"] = track.Color;
        trackJson["enabled"] = track.Enabled;
        trackJson["imported"] = track.Imported;
        switch (track.UpdateMode)
        {
            case TimelineTrackUpdateMode::Discrete: trackJson["updateMode"] = "discrete"; break;
            case TimelineTrackUpdateMode::Capture: trackJson["updateMode"] = "capture"; break;
            case TimelineTrackUpdateMode::Continuous: default: trackJson["updateMode"] = "continuous"; break;
        }
        switch (track.Interpolation)
        {
            case TimelineTrackInterpolation::Nearest: trackJson["interpolation"] = "nearest"; break;
            case TimelineTrackInterpolation::Cubic: trackJson["interpolation"] = "cubic"; break;
            case TimelineTrackInterpolation::Linear: default: trackJson["interpolation"] = "linear"; break;
        }
        trackJson["loopWrap"] = track.LoopWrap;
        if (track.Type == TimelineTrackType::Audio)
            trackJson["useBlend"] = track.UseBlend;

        json markers = json::array();
        for (const auto& marker : track.Markers)
            markers.push_back(SaveMarker(marker));
        trackJson["markers"] = std::move(markers);

        json clips = json::array();
        for (const auto& clip : track.Clips)
            clips.push_back(SaveClip(clip));
        trackJson["clips"] = std::move(clips);

        json valueKeys = json::array();
        for (const auto& key : track.ValueKeys)
            valueKeys.push_back(SaveValueKey(key));
        trackJson["valueKeys"] = std::move(valueKeys);

        json methodKeys = json::array();
        for (const auto& key : track.MethodKeys)
            methodKeys.push_back(SaveMethodKey(key));
        trackJson["methodKeys"] = std::move(methodKeys);

        json audioKeys = json::array();
        for (const auto& key : track.AudioKeys)
            audioKeys.push_back(SaveAudioKey(key));
        trackJson["audioKeys"] = std::move(audioKeys);

        json animationKeys = json::array();
        for (const auto& key : track.AnimationKeys)
            animationKeys.push_back(SaveAnimationKey(key));
        trackJson["animationKeys"] = std::move(animationKeys);

        tracks.push_back(std::move(trackJson));
    }
    doc["tracks"] = std::move(tracks);
    return doc;
}

float32 GetTimelineDuration(const Timeline& timeline)
{
    float32 duration = 0.0f;
    for (const auto& track : timeline.Tracks)
    {
        for (const auto& key : track.ValueKeys)
            duration = std::max(duration, key.Time);
        for (const auto& key : track.MethodKeys)
            duration = std::max(duration, key.Time);
        for (const auto& key : track.AudioKeys)
            duration = std::max(duration, key.Time);
        for (const auto& key : track.AnimationKeys)
            duration = std::max(duration, key.Time);
        for (const auto& marker : track.Markers)
            duration = std::max(duration, marker.Time);
        for (const auto& clip : track.Clips)
            duration = std::max(duration, clip.OffsetOnTimeline + std::max(0.0f, clip.OutTime - clip.InTime));
    }
    return duration;
}

TimelineEvaluationResult EvaluateTimeline(
    const Timeline& timeline,
    float32 previousTime,
    float32 currentTime,
    const TimelineEvaluateOptions& options)
{
    TimelineEvaluationResult result;
    const float32 duration = options.DurationOverride > 0.0f ? options.DurationOverride : GetTimelineDuration(timeline);
    const float32 sampleTime = options.Loop ? WrapTime(currentTime, duration) : currentTime;

    for (uint32 i = 0; i < static_cast<uint32>(timeline.Tracks.size()); ++i)
    {
        const TimelineTrack& track = timeline.Tracks[i];
        if (!track.Enabled)
            continue;

        if (TimelineTrackTypeUsesValueKeys(track.Type))
            SampleValueTrack(track, i, sampleTime, result);

        if (TimelineTrackTypeUsesClips(track.Type))
            SampleClipTrack(track, i, sampleTime, result);

        if (TimelineTrackTypeUsesAnimationKeys(track.Type))
            SampleAnimationKeyTrack(track, i, sampleTime, result);

        if (track.Type == TimelineTrackType::Method || track.Type == TimelineTrackType::Event)
        {
            for (const auto& key : track.MethodKeys)
            {
                if (!IsTrackEventInRange(key.Time, previousTime, currentTime, duration, options.Loop))
                    continue;
                TimelineMethodEvent event;
                event.TrackIndex = i;
                event.TrackType = track.Type;
                event.TrackName = track.Name;
                event.Time = key.Time;
                event.MethodName = key.MethodName;
                event.Arguments = key.Arguments;
                event.TypedArguments = key.TypedArguments;
                result.MethodEvents.push_back(std::move(event));
            }
        }
        if (TimelineTrackTypeUsesAudioKeys(track.Type))
        {
            for (const auto& key : track.AudioKeys)
            {
                if (!IsTrackEventInRange(key.Time, previousTime, currentTime, duration, options.Loop))
                    continue;
                TimelineAudioEvent event;
                event.TrackIndex = i;
                event.TrackName = track.Name;
                event.Time = key.Time;
                event.AudioGuid = key.AudioGuid;
                event.SourcePath = key.SourcePath;
                event.Name = key.Name;
                event.StartOffset = key.StartOffset;
                event.EndOffset = key.EndOffset;
                event.VolumeDb = key.VolumeDb;
                event.PitchScale = key.PitchScale;
                event.UseBlend = track.UseBlend;
                result.AudioEvents.push_back(std::move(event));
            }
        }
    }

    return result;
}

} // namespace Animation
} // namespace GameEngine
