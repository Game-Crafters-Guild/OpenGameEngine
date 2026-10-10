#include "Panels/TimeCompositeTimelineBridge.h"

#include "Animation/AnimationTimeline.h"
#include "Panels/TimeCompositeModel.h"

#include <nlohmann/json.hpp>

namespace GameEngine::Editor
{
namespace
{

using namespace Animation;

TimelineTrackType MapCompositeTrackType(CompositeTrackType type)
{
    switch (type)
    {
    case CompositeTrackType::Animation: return TimelineTrackType::Animation;
    case CompositeTrackType::Audio: return TimelineTrackType::Audio;
    case CompositeTrackType::Property: return TimelineTrackType::Property;
    case CompositeTrackType::Position3D: return TimelineTrackType::Position3D;
    case CompositeTrackType::Rotation3D: return TimelineTrackType::Rotation3D;
    case CompositeTrackType::Scale3D: return TimelineTrackType::Scale3D;
    case CompositeTrackType::BlendShape: return TimelineTrackType::BlendShape;
    case CompositeTrackType::Method: return TimelineTrackType::Method;
    case CompositeTrackType::Event: return TimelineTrackType::Event;
    case CompositeTrackType::Bezier: return TimelineTrackType::Bezier;
    case CompositeTrackType::Video:
        return TimelineTrackType::Animation;
    }
    return TimelineTrackType::Animation;
}

CompositeTrackType MapTimelineTrackType(TimelineTrackType type)
{
    switch (type)
    {
    case TimelineTrackType::Animation: return CompositeTrackType::Animation;
    case TimelineTrackType::Audio: return CompositeTrackType::Audio;
    case TimelineTrackType::Property: return CompositeTrackType::Property;
    case TimelineTrackType::Position3D: return CompositeTrackType::Position3D;
    case TimelineTrackType::Rotation3D: return CompositeTrackType::Rotation3D;
    case TimelineTrackType::Scale3D: return CompositeTrackType::Scale3D;
    case TimelineTrackType::BlendShape: return CompositeTrackType::BlendShape;
    case TimelineTrackType::Method: return CompositeTrackType::Method;
    case TimelineTrackType::Event: return CompositeTrackType::Event;
    case TimelineTrackType::Bezier: return CompositeTrackType::Bezier;
    }
    return CompositeTrackType::Animation;
}

TimelineBezierHandleMode MapHandleMode(CompositeBezierHandleMode mode)
{
    switch (mode)
    {
    case CompositeBezierHandleMode::Free: return TimelineBezierHandleMode::Free;
    case CompositeBezierHandleMode::Linear: return TimelineBezierHandleMode::Linear;
    case CompositeBezierHandleMode::Mirrored: return TimelineBezierHandleMode::Mirrored;
    case CompositeBezierHandleMode::Balanced:
    default:
        return TimelineBezierHandleMode::Balanced;
    }
}

CompositeBezierHandleMode MapHandleMode(TimelineBezierHandleMode mode)
{
    switch (mode)
    {
    case TimelineBezierHandleMode::Free: return CompositeBezierHandleMode::Free;
    case TimelineBezierHandleMode::Linear: return CompositeBezierHandleMode::Linear;
    case TimelineBezierHandleMode::Mirrored: return CompositeBezierHandleMode::Mirrored;
    case TimelineBezierHandleMode::Balanced:
    default:
        return CompositeBezierHandleMode::Balanced;
    }
}

TimelineTrackUpdateMode MapUpdateMode(CompositeTrackUpdateMode mode)
{
    switch (mode)
    {
    case CompositeTrackUpdateMode::Discrete: return TimelineTrackUpdateMode::Discrete;
    case CompositeTrackUpdateMode::Capture: return TimelineTrackUpdateMode::Capture;
    case CompositeTrackUpdateMode::Continuous:
    default:
        return TimelineTrackUpdateMode::Continuous;
    }
}

CompositeTrackUpdateMode MapUpdateMode(TimelineTrackUpdateMode mode)
{
    switch (mode)
    {
    case TimelineTrackUpdateMode::Discrete: return CompositeTrackUpdateMode::Discrete;
    case TimelineTrackUpdateMode::Capture: return CompositeTrackUpdateMode::Capture;
    case TimelineTrackUpdateMode::Continuous:
    default:
        return CompositeTrackUpdateMode::Continuous;
    }
}

TimelineTrackInterpolation MapInterpolation(CompositeTrackInterpolation interpolation)
{
    switch (interpolation)
    {
    case CompositeTrackInterpolation::Nearest: return TimelineTrackInterpolation::Nearest;
    case CompositeTrackInterpolation::Cubic: return TimelineTrackInterpolation::Cubic;
    case CompositeTrackInterpolation::Linear:
    default:
        return TimelineTrackInterpolation::Linear;
    }
}

CompositeTrackInterpolation MapInterpolation(TimelineTrackInterpolation interpolation)
{
    switch (interpolation)
    {
    case TimelineTrackInterpolation::Nearest: return CompositeTrackInterpolation::Nearest;
    case TimelineTrackInterpolation::Cubic: return CompositeTrackInterpolation::Cubic;
    case TimelineTrackInterpolation::Linear:
    default:
        return CompositeTrackInterpolation::Linear;
    }
}

void CopyClipToRuntime(const CompositeClip& src, TimelineClip& dst)
{
    dst.ClipGuid = src.clipGuid;
    dst.ClipAssetType = AssetType::Animation;
    dst.Name = src.name;
    dst.SourcePath = src.sourcePath;
    dst.OffsetOnTimeline = src.offsetOnTimeline;
    dst.InTime = src.inTime;
    dst.OutTime = src.outTime;
    dst.Muted = src.muted;
    dst.FadeInDuration = src.fadeInDuration;
    dst.FadeOutDuration = src.fadeOutDuration;
    dst.Markers.clear();
    dst.Markers.reserve(src.markers.size());
    for (const auto& marker : src.markers)
    {
        Animation::TimelineMarker m;
        m.Time = marker.time;
        m.Name = marker.name;
        dst.Markers.push_back(std::move(m));
    }
}

void CopyClipToComposite(const TimelineClip& src, CompositeClip& dst)
{
    dst.clipGuid = src.ClipGuid;
    dst.name = src.Name;
    dst.sourcePath = src.SourcePath;
    dst.offsetOnTimeline = src.OffsetOnTimeline;
    dst.inTime = src.InTime;
    dst.outTime = src.OutTime;
    dst.muted = src.Muted;
    dst.fadeInDuration = src.FadeInDuration;
    dst.fadeOutDuration = src.FadeOutDuration;
    dst.markers.clear();
    dst.markers.reserve(src.Markers.size());
    for (const auto& marker : src.Markers)
    {
        ::GameEngine::TimelineMarker m;
        m.time = marker.Time;
        m.name = marker.Name;
        dst.markers.push_back(std::move(m));
    }
}

void CopyTrackToRuntime(const CompositeTrack& src, TimelineTrack& dst)
{
    dst.Type = MapCompositeTrackType(src.type);
    dst.Name = src.name;
    dst.TargetPath = src.targetPath;
    dst.PropertyPath = src.propertyPath;
    dst.UpdateMode = MapUpdateMode(src.updateMode);
    dst.Interpolation = MapInterpolation(src.interpolation);
    dst.LoopWrap = src.loopWrap;
    dst.Enabled = src.enabled;
    dst.Imported = src.imported;
    dst.UseBlend = src.useBlend;
    dst.Color = src.color;

    dst.Clips.clear();
    for (const auto& clip : src.clips)
    {
        TimelineClip runtimeClip;
        CopyClipToRuntime(clip, runtimeClip);
        if (src.type == CompositeTrackType::Audio)
            runtimeClip.ClipAssetType = AssetType::Audio;
        dst.Clips.push_back(std::move(runtimeClip));
    }

    dst.ValueKeys.clear();
    for (const auto& key : src.valueKeys)
    {
        TimelineValueKey runtimeKey;
        runtimeKey.Time = key.time;
        runtimeKey.ComponentCount = key.componentCount;
        for (uint8_t i = 0; i < 4; ++i)
            runtimeKey.Value[i] = key.value[i];
        runtimeKey.Label = key.label;
        runtimeKey.InHandle[0] = key.inHandle[0];
        runtimeKey.InHandle[1] = key.inHandle[1];
        runtimeKey.OutHandle[0] = key.outHandle[0];
        runtimeKey.OutHandle[1] = key.outHandle[1];
        runtimeKey.HandleMode = MapHandleMode(key.handleMode);
        dst.ValueKeys.push_back(std::move(runtimeKey));
    }

    dst.MethodKeys.clear();
    for (const auto& key : src.methodKeys)
    {
        TimelineMethodKey runtimeKey;
        runtimeKey.Time = key.time;
        runtimeKey.MethodName = key.methodName;
        runtimeKey.Arguments = key.arguments;
        for (const auto& arg : key.typedArguments)
        {
            TimelineMethodArgument typed;
            typed.Type = arg.type;
            typed.Value = arg.value;
            runtimeKey.TypedArguments.push_back(std::move(typed));
        }
        dst.MethodKeys.push_back(std::move(runtimeKey));
    }

    dst.AudioKeys.clear();
    for (const auto& key : src.audioKeys)
    {
        TimelineAudioKey runtimeKey;
        runtimeKey.Time = key.time;
        runtimeKey.AudioGuid = key.audioGuid;
        runtimeKey.SourcePath = key.sourcePath;
        runtimeKey.Name = key.name;
        runtimeKey.StartOffset = key.startOffset;
        runtimeKey.EndOffset = key.endOffset;
        runtimeKey.VolumeDb = key.volumeDb;
        runtimeKey.PitchScale = key.pitchScale;
        dst.AudioKeys.push_back(std::move(runtimeKey));
    }

    dst.AnimationKeys.clear();
    for (const auto& key : src.animationKeys)
    {
        TimelineAnimationKey runtimeKey;
        runtimeKey.Time = key.time;
        runtimeKey.AnimationName = key.animationName;
        runtimeKey.AnimationGuid = key.animationGuid;
        runtimeKey.SourcePath = key.sourcePath;
        runtimeKey.SpeedScale = key.speedScale;
        dst.AnimationKeys.push_back(std::move(runtimeKey));
    }

    dst.Markers.clear();
    for (const auto& marker : src.markers)
    {
        Animation::TimelineMarker runtimeMarker;
        runtimeMarker.Time = marker.time;
        runtimeMarker.Name = marker.name;
        dst.Markers.push_back(std::move(runtimeMarker));
    }
}

void CopyTrackToComposite(const TimelineTrack& src, CompositeTrack& dst)
{
    dst.type = MapTimelineTrackType(src.Type);
    dst.name = src.Name;
    dst.targetPath = src.TargetPath;
    dst.propertyPath = src.PropertyPath;
    dst.updateMode = MapUpdateMode(src.UpdateMode);
    dst.interpolation = MapInterpolation(src.Interpolation);
    dst.loopWrap = src.LoopWrap;
    dst.enabled = src.Enabled;
    dst.imported = src.Imported;
    dst.useBlend = src.UseBlend;
    dst.color = src.Color;

    dst.clips.clear();
    for (const auto& clip : src.Clips)
    {
        CompositeClip compositeClip;
        CopyClipToComposite(clip, compositeClip);
        dst.clips.push_back(std::move(compositeClip));
    }

    dst.valueKeys.clear();
    for (const auto& key : src.ValueKeys)
    {
        CompositeValueKey compositeKey;
        compositeKey.time = key.Time;
        compositeKey.componentCount = key.ComponentCount;
        for (uint8_t i = 0; i < 4; ++i)
            compositeKey.value[i] = key.Value[i];
        compositeKey.label = key.Label;
        compositeKey.inHandle[0] = key.InHandle[0];
        compositeKey.inHandle[1] = key.InHandle[1];
        compositeKey.outHandle[0] = key.OutHandle[0];
        compositeKey.outHandle[1] = key.OutHandle[1];
        compositeKey.handleMode = MapHandleMode(key.HandleMode);
        dst.valueKeys.push_back(std::move(compositeKey));
    }

    dst.methodKeys.clear();
    for (const auto& key : src.MethodKeys)
    {
        CompositeMethodKey compositeKey;
        compositeKey.time = key.Time;
        compositeKey.methodName = key.MethodName;
        compositeKey.arguments = key.Arguments;
        for (const auto& arg : key.TypedArguments)
            compositeKey.typedArguments.push_back({arg.Type, arg.Value});
        dst.methodKeys.push_back(std::move(compositeKey));
    }

    dst.audioKeys.clear();
    for (const auto& key : src.AudioKeys)
    {
        CompositeAudioKey compositeKey;
        compositeKey.time = key.Time;
        compositeKey.audioGuid = key.AudioGuid;
        compositeKey.sourcePath = key.SourcePath;
        compositeKey.name = key.Name;
        compositeKey.startOffset = key.StartOffset;
        compositeKey.endOffset = key.EndOffset;
        compositeKey.volumeDb = key.VolumeDb;
        compositeKey.pitchScale = key.PitchScale;
        dst.audioKeys.push_back(std::move(compositeKey));
    }

    dst.animationKeys.clear();
    for (const auto& key : src.AnimationKeys)
    {
        CompositeAnimationKey compositeKey;
        compositeKey.time = key.Time;
        compositeKey.animationName = key.AnimationName;
        compositeKey.animationGuid = key.AnimationGuid;
        compositeKey.sourcePath = key.SourcePath;
        compositeKey.speedScale = key.SpeedScale;
        dst.animationKeys.push_back(std::move(compositeKey));
    }

    dst.markers.clear();
    for (const auto& marker : src.Markers)
    {
        ::GameEngine::TimelineMarker compositeMarker;
        compositeMarker.time = marker.Time;
        compositeMarker.name = marker.Name;
        dst.markers.push_back(std::move(compositeMarker));
    }
}

nlohmann::json SerializeEditorOnlyTracks(const TimeCompositeModel& model)
{
    nlohmann::json editorOnly = nlohmann::json::object();
    nlohmann::json videoTracks = nlohmann::json::array();
    for (const auto& track : model.tracks)
    {
        if (track.type != CompositeTrackType::Video)
            continue;
        TimeCompositeModel partial;
        partial.tracks.push_back(track);
        nlohmann::json t = SerializeTimeCompositeModel(partial);
        if (t.contains("tracks") && t["tracks"].is_array() && !t["tracks"].empty())
            videoTracks.push_back(t["tracks"][0]);
    }
    if (!videoTracks.empty())
        editorOnly["videoTracks"] = std::move(videoTracks);
    return editorOnly;
}

void MergeEditorOnlyTracks(const nlohmann::json& editorOnly, TimeCompositeModel& model)
{
    auto videoIt = editorOnly.find("videoTracks");
    if (videoIt == editorOnly.end() || !videoIt->is_array())
        return;

    for (const auto& trackJson : *videoIt)
    {
        TimeCompositeModel partial;
        nlohmann::json wrapper;
        wrapper["tracks"] = nlohmann::json::array({trackJson});
        if (!DeserializeTimeCompositeModel(wrapper, partial) || partial.tracks.empty())
            continue;
        model.tracks.push_back(std::move(partial.tracks.front()));
    }
}

} // namespace

void BuildRuntimeTimelineFromCompositeModel(const TimeCompositeModel& model, Timeline& outTimeline)
{
    outTimeline.Tracks.clear();
    outTimeline.SchemaVersion = 1;
    for (const auto& track : model.tracks)
    {
        if (track.type == CompositeTrackType::Video)
            continue;
        TimelineTrack runtimeTrack;
        CopyTrackToRuntime(track, runtimeTrack);
        outTimeline.Tracks.push_back(std::move(runtimeTrack));
    }
}

void ApplyCompositeModelFromRuntimeTimeline(const Timeline& timeline, TimeCompositeModel& outModel)
{
    outModel.tracks.clear();
    for (const auto& track : timeline.Tracks)
    {
        CompositeTrack compositeTrack;
        CopyTrackToComposite(track, compositeTrack);
        outModel.tracks.push_back(std::move(compositeTrack));
    }
}

nlohmann::json SaveTimelineDocumentFromCompositeModel(const TimeCompositeModel& model)
{
    Timeline runtime;
    BuildRuntimeTimelineFromCompositeModel(model, runtime);
    nlohmann::json doc = SaveTimelineToJson(runtime);
    nlohmann::json editorOnly = SerializeEditorOnlyTracks(model);
    if (!editorOnly.empty())
        doc["editorOnly"] = std::move(editorOnly);
    return doc;
}

bool LoadCompositeModelFromTimelineDocument(const nlohmann::json& doc,
                                            TimeCompositeModel& outModel,
                                            std::string* outError)
{
    outModel.tracks.clear();
    if (!doc.is_object())
    {
        if (outError)
            *outError = "Timeline document must be a JSON object";
        return false;
    }

    Timeline runtime;
    if (!LoadTimelineFromJson(doc, runtime, outError))
        return false;

    ApplyCompositeModelFromRuntimeTimeline(runtime, outModel);

    if (auto editorOnly = doc.find("editorOnly"); editorOnly != doc.end())
        MergeEditorOnlyTracks(*editorOnly, outModel);

    if (outModel.tracks.empty() && doc.contains("tracks"))
        return DeserializeTimeCompositeModel(doc, outModel);

    return true;
}

} // namespace GameEngine::Editor
