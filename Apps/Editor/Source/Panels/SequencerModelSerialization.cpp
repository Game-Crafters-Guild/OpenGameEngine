// JSON (de)serializers for the .timeline and .clipset asset formats.
//
// Schema is intentionally flat and human-readable so the files diff cleanly
// in version control. Animation clips are referenced by GUID string — the
// timeline / clipset assets do not embed clip data.

#include "Panels/TimeCompositeModel.h"
#include "Panels/LaneClipModel.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace GameEngine
{

namespace
{

nlohmann::json MarkersToJson(const std::vector<TimelineMarker>& markers)
{
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& m : markers)
        arr.push_back({{"time", m.time}, {"name", m.name}});
    return arr;
}

void MarkersFromJson(const nlohmann::json& arr, std::vector<TimelineMarker>& out)
{
    if (!arr.is_array()) return;
    for (const auto& m : arr)
    {
        if (!m.is_object()) continue;
        TimelineMarker tm;
        tm.time = m.value("time", 0.0f);
        tm.name = m.value("name", std::string{});
        out.push_back(std::move(tm));
    }
}

nlohmann::json LaneMarkersToJson(const std::vector<LaneMarker>& markers)
{
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& m : markers)
        arr.push_back({{"time", m.time}, {"name", m.name}});
    return arr;
}

void LaneMarkersFromJson(const nlohmann::json& arr, std::vector<LaneMarker>& out)
{
    if (!arr.is_array()) return;
    for (const auto& m : arr)
    {
        if (!m.is_object()) continue;
        LaneMarker tm;
        tm.time = m.value("time", 0.0f);
        tm.name = m.value("name", std::string{});
        out.push_back(std::move(tm));
    }
}

} // namespace

// -------- Timeline (.timeline) ----------------------------------------------

const char* CompositeTrackTypeToString(CompositeTrackType type)
{
    switch (type)
    {
    case CompositeTrackType::Animation:  return "animation";
    case CompositeTrackType::Audio:      return "audio";
    case CompositeTrackType::Video:      return "video";
    case CompositeTrackType::Property:   return "property";
    case CompositeTrackType::Position3D: return "position3d";
    case CompositeTrackType::Rotation3D: return "rotation3d";
    case CompositeTrackType::Scale3D:    return "scale3d";
    case CompositeTrackType::BlendShape: return "blendShape";
    case CompositeTrackType::Method:     return "method";
    case CompositeTrackType::Event:      return "event";
    case CompositeTrackType::Bezier:     return "bezier";
    }
    return "animation";
}

const char* CompositeTrackTypeDisplayName(CompositeTrackType type)
{
    switch (type)
    {
    case CompositeTrackType::Animation:  return "Animation Playback";
    case CompositeTrackType::Audio:      return "Audio Playback";
    case CompositeTrackType::Video:      return "Video Playback";
    case CompositeTrackType::Property:   return "Property";
    case CompositeTrackType::Position3D: return "3D Position";
    case CompositeTrackType::Rotation3D: return "3D Rotation";
    case CompositeTrackType::Scale3D:    return "3D Scale";
    case CompositeTrackType::BlendShape: return "Blend Shape";
    case CompositeTrackType::Method:     return "Call Method";
    case CompositeTrackType::Event:      return "Event";
    case CompositeTrackType::Bezier:     return "Bezier Curve";
    }
    return "Animation Playback";
}

CompositeTrackType CompositeTrackTypeFromString(const std::string& type)
{
    if (type == "audio") return CompositeTrackType::Audio;
    if (type == "video") return CompositeTrackType::Video;
    if (type == "property") return CompositeTrackType::Property;
    if (type == "position3d" || type == "position_3d") return CompositeTrackType::Position3D;
    if (type == "rotation3d" || type == "rotation_3d") return CompositeTrackType::Rotation3D;
    if (type == "scale3d" || type == "scale_3d") return CompositeTrackType::Scale3D;
    if (type == "blendShape" || type == "blend_shape") return CompositeTrackType::BlendShape;
    if (type == "method" || type == "callMethod") return CompositeTrackType::Method;
    if (type == "event" || type == "animationEvent") return CompositeTrackType::Event;
    if (type == "bezier") return CompositeTrackType::Bezier;
    return CompositeTrackType::Animation;
}

bool CompositeTrackTypeUsesClips(CompositeTrackType type)
{
    return type == CompositeTrackType::Animation || type == CompositeTrackType::Audio || type == CompositeTrackType::Video;
}

bool CompositeTrackTypeUsesValueKeys(CompositeTrackType type)
{
    return type == CompositeTrackType::Property ||
           type == CompositeTrackType::Position3D ||
           type == CompositeTrackType::Rotation3D ||
           type == CompositeTrackType::Scale3D ||
           type == CompositeTrackType::BlendShape ||
           type == CompositeTrackType::Bezier;
}

bool CompositeTrackTypeUsesAudioKeys(CompositeTrackType type)
{
    return type == CompositeTrackType::Audio;
}

bool CompositeTrackTypeUsesMethodKeys(CompositeTrackType type)
{
    return type == CompositeTrackType::Method || type == CompositeTrackType::Event;
}

bool CompositeTrackTypeUsesAnimationKeys(CompositeTrackType type)
{
    return type == CompositeTrackType::Animation;
}

uint8_t CompositeTrackTypeComponentCount(CompositeTrackType type)
{
    switch (type)
    {
    case CompositeTrackType::Position3D:
    case CompositeTrackType::Scale3D:
        return 3u;
    case CompositeTrackType::Rotation3D:
        return 4u;
    case CompositeTrackType::Property:
    case CompositeTrackType::BlendShape:
    case CompositeTrackType::Bezier:
    case CompositeTrackType::Animation:
    case CompositeTrackType::Audio:
    case CompositeTrackType::Video:
    case CompositeTrackType::Method:
    case CompositeTrackType::Event:
        return 1u;
    }
    return 1u;
}

const char* CompositeTrackUpdateModeToString(CompositeTrackUpdateMode mode)
{
    switch (mode)
    {
    case CompositeTrackUpdateMode::Continuous: return "continuous";
    case CompositeTrackUpdateMode::Discrete:   return "discrete";
    case CompositeTrackUpdateMode::Capture:    return "capture";
    }
    return "continuous";
}

CompositeTrackUpdateMode CompositeTrackUpdateModeFromString(const std::string& value)
{
    if (value == "discrete") return CompositeTrackUpdateMode::Discrete;
    if (value == "capture") return CompositeTrackUpdateMode::Capture;
    return CompositeTrackUpdateMode::Continuous;
}

const char* CompositeTrackInterpolationToString(CompositeTrackInterpolation interpolation)
{
    switch (interpolation)
    {
    case CompositeTrackInterpolation::Nearest: return "nearest";
    case CompositeTrackInterpolation::Linear:  return "linear";
    case CompositeTrackInterpolation::Cubic:   return "cubic";
    }
    return "linear";
}

CompositeTrackInterpolation CompositeTrackInterpolationFromString(const std::string& value)
{
    if (value == "nearest") return CompositeTrackInterpolation::Nearest;
    if (value == "cubic") return CompositeTrackInterpolation::Cubic;
    return CompositeTrackInterpolation::Linear;
}

const char* CompositeBezierHandleModeToString(CompositeBezierHandleMode mode)
{
    switch (mode)
    {
    case CompositeBezierHandleMode::Free:     return "free";
    case CompositeBezierHandleMode::Linear:   return "linear";
    case CompositeBezierHandleMode::Balanced: return "balanced";
    case CompositeBezierHandleMode::Mirrored: return "mirrored";
    }
    return "balanced";
}

CompositeBezierHandleMode CompositeBezierHandleModeFromString(const std::string& value)
{
    if (value == "free") return CompositeBezierHandleMode::Free;
    if (value == "linear") return CompositeBezierHandleMode::Linear;
    if (value == "mirrored") return CompositeBezierHandleMode::Mirrored;
    return CompositeBezierHandleMode::Balanced;
}

const char* CompositeMediaSyncModeToString(CompositeMediaSyncMode mode)
{
    switch (mode)
    {
    case CompositeMediaSyncMode::Lock:       return "lock";
    case CompositeMediaSyncMode::KeepOffset: return "keepOffset";
    }
    return "lock";
}

CompositeMediaSyncMode CompositeMediaSyncModeFromString(const std::string& value)
{
    if (value == "keepOffset") return CompositeMediaSyncMode::KeepOffset;
    return CompositeMediaSyncMode::Lock;
}

nlohmann::json SerializeTimeCompositeModel(const TimeCompositeModel& model)
{
    nlohmann::json j;
    j["schemaVersion"] = 1;
    j["assetType"] = "Timeline";
    nlohmann::json tracks = nlohmann::json::array();
    for (const auto& track : model.tracks)
    {
        nlohmann::json t;
        t["type"] = CompositeTrackTypeToString(track.type);
        t["name"] = track.name;
        if (!track.targetPath.empty()) t["targetPath"] = track.targetPath;
        if (!track.propertyPath.empty()) t["propertyPath"] = track.propertyPath;
        if (track.color != kDefaultSequencerRowColorArgb) t["color"] = track.color;
        t["enabled"] = track.enabled;
        t["imported"] = track.imported;
        t["updateMode"] = CompositeTrackUpdateModeToString(track.updateMode);
        t["interpolation"] = CompositeTrackInterpolationToString(track.interpolation);
        t["loopWrap"] = track.loopWrap;
        if (track.type == CompositeTrackType::Audio)
        {
            t["useBlend"] = track.useBlend;
            t["syncLinkedVideo"] = track.syncLinkedVideo;
            t["syncMode"] = CompositeMediaSyncModeToString(track.mediaSyncMode);
        }
        t["markers"] = MarkersToJson(track.markers);
        nlohmann::json clips = nlohmann::json::array();
        for (const auto& clip : track.clips)
        {
            nlohmann::json c;
            c["clipGuid"] = clip.clipGuid.ToString();
            c["name"] = clip.name;
            c["sourcePath"] = clip.sourcePath.string();
            c["linkedMediaGroupGuid"] = clip.linkedMediaGroupGuid.ToString();
            c["linkedMediaOffsetSeconds"] = clip.linkedMediaOffsetSeconds;
            c["offsetOnTimeline"] = clip.offsetOnTimeline;
            c["inTime"] = clip.inTime;
            c["outTime"] = clip.outTime;
            c["muted"] = clip.muted;
            c["fadeInDuration"] = clip.fadeInDuration;
            c["fadeOutDuration"] = clip.fadeOutDuration;
            c["markers"] = MarkersToJson(clip.markers);
            clips.push_back(std::move(c));
        }
        t["clips"] = std::move(clips);
        nlohmann::json valueKeys = nlohmann::json::array();
        for (const CompositeValueKey& key : track.valueKeys)
        {
            nlohmann::json k;
            k["time"] = key.time;
            k["componentCount"] = key.componentCount;
            k["value"] = nlohmann::json::array();
            for (uint8_t i = 0; i < key.componentCount && i < 4u; ++i)
                k["value"].push_back(key.value[i]);
            if (!key.label.empty())
                k["label"] = key.label;
            k["inHandle"] = {key.inHandle[0], key.inHandle[1]};
            k["outHandle"] = {key.outHandle[0], key.outHandle[1]};
            k["handleMode"] = CompositeBezierHandleModeToString(key.handleMode);
            valueKeys.push_back(std::move(k));
        }
        t["valueKeys"] = std::move(valueKeys);
        nlohmann::json methodKeys = nlohmann::json::array();
        for (const CompositeMethodKey& key : track.methodKeys)
        {
            nlohmann::json k;
            k["time"] = key.time;
            k["methodName"] = key.methodName;
            if (!key.arguments.empty())
                k["arguments"] = key.arguments;
            if (!key.typedArguments.empty())
            {
                nlohmann::json args = nlohmann::json::array();
                for (const CompositeMethodArgument& arg : key.typedArguments)
                    args.push_back({{"type", arg.type}, {"value", arg.value}});
                k["typedArguments"] = std::move(args);
            }
            methodKeys.push_back(std::move(k));
        }
        t["methodKeys"] = std::move(methodKeys);
        nlohmann::json audioKeys = nlohmann::json::array();
        for (const CompositeAudioKey& key : track.audioKeys)
        {
            nlohmann::json k;
            k["time"] = key.time;
            k["audioGuid"] = key.audioGuid.ToString();
            k["sourcePath"] = key.sourcePath.string();
            k["name"] = key.name;
            k["startOffset"] = key.startOffset;
            k["endOffset"] = key.endOffset;
            k["volumeDb"] = key.volumeDb;
            k["pitchScale"] = key.pitchScale;
            audioKeys.push_back(std::move(k));
        }
        t["audioKeys"] = std::move(audioKeys);
        nlohmann::json animationKeys = nlohmann::json::array();
        for (const CompositeAnimationKey& key : track.animationKeys)
        {
            nlohmann::json k;
            k["time"] = key.time;
            k["animationName"] = key.animationName;
            k["animationGuid"] = key.animationGuid.ToString();
            k["sourcePath"] = key.sourcePath.string();
            k["speedScale"] = key.speedScale;
            animationKeys.push_back(std::move(k));
        }
        t["animationKeys"] = std::move(animationKeys);
        tracks.push_back(std::move(t));
    }
    j["tracks"] = std::move(tracks);
    return j;
}

bool DeserializeTimeCompositeModel(const nlohmann::json& doc, TimeCompositeModel& outModel)
{
    outModel.tracks.clear();
    if (!doc.is_object()) return false;
    auto tracksIt = doc.find("tracks");
    if (tracksIt == doc.end() || !tracksIt->is_array()) return true; // empty timeline is valid
    for (const auto& t : *tracksIt)
    {
        if (!t.is_object()) continue;
        CompositeTrack track;
        track.type = CompositeTrackTypeFromString(t.value("type", std::string{"animation"}));
        track.name = t.value("name", std::string{});
        track.targetPath = t.value("targetPath", std::string{});
        track.propertyPath = t.value("propertyPath", std::string{});
        track.color = t.value("color", kDefaultSequencerRowColorArgb);
        track.enabled = t.value("enabled", true);
        track.imported = t.value("imported", false);
        track.updateMode = CompositeTrackUpdateModeFromString(t.value("updateMode", std::string{"continuous"}));
        track.interpolation = CompositeTrackInterpolationFromString(t.value("interpolation", std::string{"linear"}));
        track.loopWrap = t.value("loopWrap", true);
        track.useBlend = t.value("useBlend", true);
        track.syncLinkedVideo = t.value("syncLinkedVideo", true);
        track.mediaSyncMode = CompositeMediaSyncModeFromString(t.value("syncMode", std::string{"lock"}));
        if (auto m = t.find("markers"); m != t.end()) MarkersFromJson(*m, track.markers);
        if (auto cs = t.find("clips"); cs != t.end() && cs->is_array())
        {
            for (const auto& c : *cs)
            {
                if (!c.is_object()) continue;
                CompositeClip clip;
                clip.clipGuid = GUID(c.value("clipGuid", std::string{}));
                clip.name = c.value("name", std::string{});
                clip.sourcePath = c.value("sourcePath", std::string{});
                clip.linkedMediaGroupGuid = GUID(c.value("linkedMediaGroupGuid", std::string{}));
                clip.linkedMediaOffsetSeconds = c.value("linkedMediaOffsetSeconds", 0.0f);
                clip.offsetOnTimeline = c.value("offsetOnTimeline", 0.0f);
                clip.inTime  = c.value("inTime", 0.0f);
                clip.outTime = c.value("outTime", 10.0f);
                clip.muted = c.value("muted", false);
                clip.fadeInDuration  = c.value("fadeInDuration", 0.0f);
                clip.fadeOutDuration = c.value("fadeOutDuration", 0.0f);
                if (auto cm = c.find("markers"); cm != c.end()) MarkersFromJson(*cm, clip.markers);
                track.clips.push_back(std::move(clip));
            }
        }
        if (auto ks = t.find("valueKeys"); ks != t.end() && ks->is_array())
        {
            for (const auto& k : *ks)
            {
                if (!k.is_object()) continue;
                CompositeValueKey key;
                key.time = k.value("time", 0.0f);
                key.componentCount = static_cast<uint8_t>(
                    std::clamp(k.value("componentCount", static_cast<int>(CompositeTrackTypeComponentCount(track.type))), 1, 4));
                if (auto values = k.find("value"); values != k.end() && values->is_array())
                {
                    const size_t n = std::min<size_t>(values->size(), 4u);
                    for (size_t i = 0; i < n; ++i)
                        key.value[i] = (*values)[i].get<float>();
                }
                key.label = k.value("label", std::string{});
                if (auto handle = k.find("inHandle"); handle != k.end() && handle->is_array() && handle->size() >= 2)
                {
                    key.inHandle[0] = (*handle)[0].get<float>();
                    key.inHandle[1] = (*handle)[1].get<float>();
                }
                if (auto handle = k.find("outHandle"); handle != k.end() && handle->is_array() && handle->size() >= 2)
                {
                    key.outHandle[0] = (*handle)[0].get<float>();
                    key.outHandle[1] = (*handle)[1].get<float>();
                }
                key.handleMode = CompositeBezierHandleModeFromString(k.value("handleMode", std::string{"balanced"}));
                track.valueKeys.push_back(std::move(key));
            }
        }
        if (auto ks = t.find("methodKeys"); ks != t.end() && ks->is_array())
        {
            for (const auto& k : *ks)
            {
                if (!k.is_object()) continue;
                CompositeMethodKey key;
                key.time = k.value("time", 0.0f);
                key.methodName = k.value("methodName", std::string{});
                key.arguments = k.value("arguments", std::string{});
                if (auto args = k.find("typedArguments"); args != k.end() && args->is_array())
                {
                    for (const auto& argJson : *args)
                    {
                        if (!argJson.is_object()) continue;
                        key.typedArguments.push_back({
                            argJson.value("type", std::string{"string"}),
                            argJson.value("value", std::string{})
                        });
                    }
                }
                track.methodKeys.push_back(std::move(key));
            }
        }
        if (auto ks = t.find("audioKeys"); ks != t.end() && ks->is_array())
        {
            for (const auto& k : *ks)
            {
                if (!k.is_object()) continue;
                CompositeAudioKey key;
                key.time = k.value("time", 0.0f);
                key.audioGuid = GUID(k.value("audioGuid", std::string{}));
                key.sourcePath = k.value("sourcePath", std::string{});
                key.name = k.value("name", std::string{});
                key.startOffset = k.value("startOffset", 0.0f);
                key.endOffset = k.value("endOffset", 0.0f);
                key.volumeDb = k.value("volumeDb", 0.0f);
                key.pitchScale = k.value("pitchScale", 1.0f);
                track.audioKeys.push_back(std::move(key));
            }
        }
        if (auto ks = t.find("animationKeys"); ks != t.end() && ks->is_array())
        {
            for (const auto& k : *ks)
            {
                if (!k.is_object()) continue;
                CompositeAnimationKey key;
                key.time = k.value("time", 0.0f);
                key.animationName = k.value("animationName", std::string{});
                key.animationGuid = GUID(k.value("animationGuid", std::string{}));
                key.sourcePath = k.value("sourcePath", std::string{});
                key.speedScale = k.value("speedScale", 1.0f);
                track.animationKeys.push_back(std::move(key));
            }
        }
        outModel.tracks.push_back(std::move(track));
    }
    return true;
}

// -------- ClipSet (.clipset) ------------------------------------------------

nlohmann::json SerializeLaneClipModel(const LaneClipModel& model)
{
    nlohmann::json j;
    j["schemaVersion"] = 1;
    j["assetType"] = "ClipSet";
    nlohmann::json lanes = nlohmann::json::array();
    for (const auto& lane : model.lanes)
    {
        nlohmann::json l;
        l["name"] = lane.name;
        if (lane.color != kDefaultLaneRowColorArgb) l["color"] = lane.color;
        l["markers"] = LaneMarkersToJson(lane.markers);
        nlohmann::json clips = nlohmann::json::array();
        for (const auto& clip : lane.clips)
        {
            nlohmann::json c;
            c["clipGuid"] = clip.clipGuid.ToString();
            c["name"] = clip.name;
            c["startTimeOnLane"] = clip.startTimeOnLane;
            c["scale"] = clip.scale;
            c["sourceDuration"] = clip.sourceDuration;
            c["loopCount"] = clip.loopCount;
            c["muted"] = clip.muted;
            c["fadeInDuration"] = clip.fadeInDuration;
            c["fadeOutDuration"] = clip.fadeOutDuration;
            c["markers"] = LaneMarkersToJson(clip.markers);
            clips.push_back(std::move(c));
        }
        l["clips"] = std::move(clips);
        lanes.push_back(std::move(l));
    }
    j["lanes"] = std::move(lanes);
    return j;
}

bool DeserializeLaneClipModel(const nlohmann::json& doc, LaneClipModel& outModel)
{
    outModel.lanes.clear();
    if (!doc.is_object()) return false;
    auto lanesIt = doc.find("lanes");
    if (lanesIt == doc.end() || !lanesIt->is_array()) return true;
    for (const auto& l : *lanesIt)
    {
        if (!l.is_object()) continue;
        LaneClipLane lane;
        lane.name = l.value("name", std::string{});
        lane.color = l.value("color", kDefaultLaneRowColorArgb);
        if (auto m = l.find("markers"); m != l.end()) LaneMarkersFromJson(*m, lane.markers);
        if (auto cs = l.find("clips"); cs != l.end() && cs->is_array())
        {
            for (const auto& c : *cs)
            {
                if (!c.is_object()) continue;
                LaneClipInstance clip;
                clip.clipGuid = GUID(c.value("clipGuid", std::string{}));
                clip.name = c.value("name", std::string{});
                clip.startTimeOnLane = c.value("startTimeOnLane", 0.0f);
                clip.scale = c.value("scale", 1.0f);
                clip.sourceDuration = c.value("sourceDuration", 1.0f);
                clip.loopCount = c.value("loopCount", 1);
                clip.muted = c.value("muted", false);
                clip.fadeInDuration  = c.value("fadeInDuration", 0.0f);
                clip.fadeOutDuration = c.value("fadeOutDuration", 0.0f);
                if (auto cm = c.find("markers"); cm != c.end()) LaneMarkersFromJson(*cm, clip.markers);
                lane.clips.push_back(std::move(clip));
            }
        }
        outModel.lanes.push_back(std::move(lane));
    }
    return true;
}

} // namespace GameEngine
