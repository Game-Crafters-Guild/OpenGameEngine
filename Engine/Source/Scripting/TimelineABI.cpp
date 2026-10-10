#include "Scripting/TimelineABI.h"

#include "AbiHandles.h"
#include "Animation/AnimationTimeline.h"
#include "Assets/TimelineAsset.h"
#include "Components/Animation/Animator.h"
#include "Components/Animation/TimelinePlaybackState.h"
#include "Core/Engine.h"
#include "DllEngineBootstrap.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Engine/Rendering/TimelinePlayback.h"

#include <nlohmann/json.hpp>

#include <cstring>
#include <string>

namespace
{

GE_Timeline_ManagedMethodListenerFn g_AbiManagedMethodListener = nullptr;

using namespace GameEngine;
using namespace GameEngine::Animation;
using namespace GameEngine::Components;
using DllBootstrap::EnsureEngineInitialized;
using ScriptingAbi::EntityFromId;
using ScriptingAbi::GuidFromAbiBytes;
using ScriptingAbi::WorldFromHandle;

static nlohmann::json EvaluationResultToJson(const TimelineEvaluationResult& result)
{
    nlohmann::json doc = nlohmann::json::object();

    nlohmann::json valueSamples = nlohmann::json::array();
    for (const auto& sample : result.ValueSamples)
    {
        nlohmann::json values = nlohmann::json::array();
        for (uint8_t i = 0; i < sample.ComponentCount && i < 4u; ++i)
            values.push_back(sample.Value[i]);
        valueSamples.push_back({
            {"trackIndex", sample.TrackIndex},
            {"trackType", TimelineTrackTypeToString(sample.TrackType)},
            {"trackName", sample.TrackName},
            {"targetPath", sample.TargetPath},
            {"propertyPath", sample.PropertyPath},
            {"componentCount", sample.ComponentCount},
            {"value", std::move(values)},
        });
    }
    doc["valueSamples"] = std::move(valueSamples);

    nlohmann::json methodEvents = nlohmann::json::array();
    for (const auto& event : result.MethodEvents)
    {
        methodEvents.push_back({
            {"trackIndex", event.TrackIndex},
            {"trackType", TimelineTrackTypeToString(event.TrackType)},
            {"trackName", event.TrackName},
            {"time", event.Time},
            {"methodName", event.MethodName},
            {"arguments", event.Arguments},
        });
    }
    doc["methodEvents"] = std::move(methodEvents);

    nlohmann::json audioEvents = nlohmann::json::array();
    for (const auto& event : result.AudioEvents)
    {
        audioEvents.push_back({
            {"trackIndex", event.TrackIndex},
            {"trackName", event.TrackName},
            {"time", event.Time},
            {"audioGuid", event.AudioGuid.ToString()},
            {"name", event.Name},
            {"volumeDb", event.VolumeDb},
            {"pitchScale", event.PitchScale},
        });
    }
    doc["audioEvents"] = std::move(audioEvents);

    nlohmann::json activeClips = nlohmann::json::array();
    for (const auto& clip : result.ActiveClips)
    {
        activeClips.push_back({
            {"trackIndex", clip.TrackIndex},
            {"trackType", TimelineTrackTypeToString(clip.TrackType)},
            {"clipGuid", clip.ClipGuid.ToString()},
            {"assetType", static_cast<int>(clip.ClipAssetType)},
            {"name", clip.Name},
            {"localTime", clip.LocalTime},
            {"weight", clip.Weight},
        });
    }
    doc["activeClips"] = std::move(activeClips);

    return doc;
}

static int32_t WriteJsonResult(const nlohmann::json& doc,
                               char* outResultJsonUtf8,
                               uint32_t outBufferBytes)
{
    const std::string serialized = doc.dump();
    if (serialized.size() + 1 > outBufferBytes)
        return static_cast<int32_t>(GE_Result_BufferFull);
    if (outResultJsonUtf8 && outBufferBytes > 0)
    {
        std::memcpy(outResultJsonUtf8, serialized.c_str(), serialized.size() + 1);
        return static_cast<int32_t>(serialized.size());
    }
    return static_cast<int32_t>(GE_Result_InvalidArg);
}

static TimelineEvaluateOptions OptionsFromAbi(const GE_Timeline_EvaluateOptions* options)
{
    TimelineEvaluateOptions native;
    if (options)
    {
        native.Loop = options->loop != 0;
        native.DurationOverride = options->durationOverride;
    }
    return native;
}

static int32_t EvaluateTimelineToBuffer(const Timeline& timeline,
                                        float previousTime,
                                        float currentTime,
                                        const GE_Timeline_EvaluateOptions* options,
                                        char* outResultJsonUtf8,
                                        uint32_t outBufferBytes)
{
    const TimelineEvaluationResult result =
        EvaluateTimeline(timeline, previousTime, currentTime, OptionsFromAbi(options));
    return WriteJsonResult(EvaluationResultToJson(result), outResultJsonUtf8, outBufferBytes);
}

} // namespace

extern "C" {

GE_API int32_t GE_CDECL GE_Timeline_EvaluateDocumentUtf8(const char* timelineJsonUtf8,
                                                         uint32_t timelineJsonLength,
                                                         float previousTime,
                                                         float currentTime,
                                                         const GE_Timeline_EvaluateOptions* options,
                                                         char* outResultJsonUtf8,
                                                         uint32_t outBufferBytes)
{
    if (EnsureEngineInitialized() != GE_Result_Ok)
        return static_cast<int32_t>(GE_Result_NotInitialized);
    if (!timelineJsonUtf8 || timelineJsonLength == 0 || !outResultJsonUtf8 || outBufferBytes == 0)
        return static_cast<int32_t>(GE_Result_InvalidArg);

    try
    {
        const std::string jsonText(timelineJsonUtf8, timelineJsonUtf8 + timelineJsonLength);
        const nlohmann::json doc = nlohmann::json::parse(jsonText);
        Timeline timeline;
        std::string error;
        if (!LoadTimelineFromJson(doc, timeline, &error))
            return static_cast<int32_t>(GE_Result_Fail);
        return EvaluateTimelineToBuffer(timeline, previousTime, currentTime, options, outResultJsonUtf8, outBufferBytes);
    }
    catch (...)
    {
        return static_cast<int32_t>(GE_Result_Fail);
    }
}

GE_API GE_Result GE_CDECL GE_Timeline_GetDurationFromDocumentUtf8(const char* timelineJsonUtf8,
                                                                   uint32_t timelineJsonLength,
                                                                   float* outDurationSeconds)
{
    if (EnsureEngineInitialized() != GE_Result_Ok)
        return GE_Result_NotInitialized;
    if (!timelineJsonUtf8 || timelineJsonLength == 0 || !outDurationSeconds)
        return GE_Result_InvalidArg;

    try
    {
        const std::string jsonText(timelineJsonUtf8, timelineJsonUtf8 + timelineJsonLength);
        const nlohmann::json doc = nlohmann::json::parse(jsonText);
        Timeline timeline;
        std::string error;
        if (!LoadTimelineFromJson(doc, timeline, &error))
            return GE_Result_Fail;
        *outDurationSeconds = GetTimelineDuration(timeline);
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

GE_API int32_t GE_CDECL GE_Timeline_EvaluateAssetGuid(const uint8_t timelineGuidBytes[16],
                                                      float previousTime,
                                                      float currentTime,
                                                      const GE_Timeline_EvaluateOptions* options,
                                                      char* outResultJsonUtf8,
                                                      uint32_t outBufferBytes)
{
    if (EnsureEngineInitialized() != GE_Result_Ok)
        return static_cast<int32_t>(GE_Result_NotInitialized);
    if (!timelineGuidBytes || !outResultJsonUtf8 || outBufferBytes == 0)
        return static_cast<int32_t>(GE_Result_InvalidArg);

    AssetManager& assetManager = EngineCore::GetInstance().GetAssetManager();
    SharedPtr<Asset> asset = Engine::Renderer::TryAcquireTimelineAsset(assetManager, GuidFromAbiBytes(timelineGuidBytes));
    auto* timelineAsset = asset ? dynamic_cast<TimelineAsset*>(asset.get()) : nullptr;
    if (!timelineAsset)
        return static_cast<int32_t>(GE_Result_NotFound);

    Timeline timeline;
    std::string error;
    if (!Engine::Renderer::LoadTimelineFromAsset(*timelineAsset, timeline, &error))
        return static_cast<int32_t>(GE_Result_Fail);

    return EvaluateTimelineToBuffer(timeline, previousTime, currentTime, options, outResultJsonUtf8, outBufferBytes);
}

GE_API GE_Result GE_CDECL GE_Timeline_GetDurationAssetGuid(const uint8_t timelineGuidBytes[16],
                                                           float* outDurationSeconds)
{
    if (EnsureEngineInitialized() != GE_Result_Ok)
        return GE_Result_NotInitialized;
    if (!timelineGuidBytes || !outDurationSeconds)
        return GE_Result_InvalidArg;

    AssetManager& assetManager = EngineCore::GetInstance().GetAssetManager();
    SharedPtr<Asset> asset = Engine::Renderer::TryAcquireTimelineAsset(assetManager, GuidFromAbiBytes(timelineGuidBytes));
    auto* timelineAsset = asset ? dynamic_cast<TimelineAsset*>(asset.get()) : nullptr;
    if (!timelineAsset)
        return GE_Result_NotFound;

    Timeline timeline;
    std::string error;
    if (!Engine::Renderer::LoadTimelineFromAsset(*timelineAsset, timeline, &error))
        return GE_Result_Fail;

    *outDurationSeconds = GetTimelineDuration(timeline);
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_Timeline_PlayOnEntity(GE_Handle worldHandle,
                                                   uint64_t entityId,
                                                   const uint8_t timelineGuidBytes[16],
                                                   int32_t loop,
                                                   int32_t autoPlay)
{
    if (EnsureEngineInitialized() != GE_Result_Ok)
        return GE_Result_NotInitialized;

    ECS::World* world = WorldFromHandle(worldHandle);
    if (!world)
        return GE_Result_InvalidArg;

    const ECS::EntityHandle entity = EntityFromId(entityId);
    if (!world->IsValid(entity))
        return GE_Result_NotFound;

    auto* animator = world->GetComponentForWrite<Animator>(entity);
    if (!animator)
    {
        Animator created;
        world->AddComponentImmediate(entity, created);
        animator = world->GetComponentForWrite<Animator>(entity);
    }
    if (!animator)
        return GE_Result_Fail;

    animator->PlayTimeline(GuidFromAbiBytes(timelineGuidBytes));
    animator->loop = loop != 0;
    animator->autoPlayOnEnterPlayMode = autoPlay != 0;

    TimelinePlaybackState state;
    state.playing = true;
    state.paused = false;
    state.timeSeconds = 0.0f;
    state.previousTimeSeconds = 0.0f;
    Engine::Renderer::EnsureTimelinePlaybackState(*world, entity, state);

    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_Timeline_PauseOnEntity(GE_Handle worldHandle, uint64_t entityId)
{
    if (EnsureEngineInitialized() != GE_Result_Ok)
        return GE_Result_NotInitialized;
    ECS::World* world = WorldFromHandle(worldHandle);
    if (!world)
        return GE_Result_InvalidArg;
    const ECS::EntityHandle entity = EntityFromId(entityId);
    auto* animator = world->GetComponentForWrite<Animator>(entity);
    if (!animator)
        return GE_Result_NotFound;
    animator->Pause();
    if (auto* state = world->GetComponentForWrite<TimelinePlaybackState>(entity))
        state->paused = true;
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_Timeline_StopOnEntity(GE_Handle worldHandle, uint64_t entityId)
{
    if (EnsureEngineInitialized() != GE_Result_Ok)
        return GE_Result_NotInitialized;
    ECS::World* world = WorldFromHandle(worldHandle);
    if (!world)
        return GE_Result_InvalidArg;
    const ECS::EntityHandle entity = EntityFromId(entityId);
    auto* animator = world->GetComponentForWrite<Animator>(entity);
    if (!animator)
        return GE_Result_NotFound;
    animator->Stop();
    if (auto* state = world->GetComponentForWrite<TimelinePlaybackState>(entity))
    {
        state->playing = false;
        state->paused = false;
        state->timeSeconds = 0.0f;
        state->previousTimeSeconds = 0.0f;
    }
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_Timeline_SeekOnEntity(GE_Handle worldHandle, uint64_t entityId, float timeSeconds)
{
    if (EnsureEngineInitialized() != GE_Result_Ok)
        return GE_Result_NotInitialized;
    ECS::World* world = WorldFromHandle(worldHandle);
    if (!world)
        return GE_Result_InvalidArg;
    const ECS::EntityHandle entity = EntityFromId(entityId);
    auto* animator = world->GetComponentForWrite<Animator>(entity);
    if (!animator)
        return GE_Result_NotFound;
    animator->Seek(timeSeconds);
    if (auto* state = world->GetComponentForWrite<TimelinePlaybackState>(entity))
    {
        state->timeSeconds = std::max(0.0f, timeSeconds);
        state->previousTimeSeconds = state->timeSeconds;
    }
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_Timeline_RegisterMethodCallback(const char* methodNameUtf8, uint32_t methodNameLength)
{
    if (!methodNameUtf8 || methodNameLength == 0)
        return GE_Result_InvalidArg;
    const std::string methodName(methodNameUtf8, methodNameUtf8 + methodNameLength);
    Engine::Renderer::RegisterTimelineMethodCallback(
        methodName,
        [](ECS::World& world, ECS::EntityHandle root, const TimelineMethodEvent& event)
        {
            (void)world;
            (void)root;
            (void)event;
        });
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_Timeline_UnregisterMethodCallback(const char* methodNameUtf8, uint32_t methodNameLength)
{
    if (!methodNameUtf8 || methodNameLength == 0)
        return GE_Result_InvalidArg;
    const std::string methodName(methodNameUtf8, methodNameUtf8 + methodNameLength);
    Engine::Renderer::UnregisterTimelineMethodCallback(methodName);
    return GE_Result_Ok;
}

static void ForwardManagedMethodListener(uint64_t entityId,
                                         const char* methodNameUtf8,
                                         const char* argumentsUtf8)
{
    if (!g_AbiManagedMethodListener)
        return;
    g_AbiManagedMethodListener(entityId,
                               methodNameUtf8,
                               methodNameUtf8 ? static_cast<uint32_t>(std::strlen(methodNameUtf8)) : 0u,
                               argumentsUtf8,
                               argumentsUtf8 ? static_cast<uint32_t>(std::strlen(argumentsUtf8)) : 0u);
}

GE_API void GE_CDECL GE_Timeline_SetManagedMethodListener(GE_Timeline_ManagedMethodListenerFn listener)
{
    g_AbiManagedMethodListener = listener;
    Engine::Renderer::SetManagedTimelineMethodListener(listener ? ForwardManagedMethodListener : nullptr);
}

} // extern "C"
