#include "Engine/Rendering/TimelinePlayback.h"

#include "Animation/AnimationTimeline.h"
#include "AssetCore/Asset.h"
#include "AssetCore/AssetTypes.h"
#include "Assets/AssetManager.h"
#include "Assets/TimelineAsset.h"
#include "Audio/AudioSystem.h"
#include "Components/Animation/AnimatorRef.h"
#include "Components/Animation/SkeletonRef.h"
#include "Components/Animation/TimelinePlaybackState.h"
#include "Components/Hierarchy.h"
#include "Components/HierarchyQueries.h"
#include "Components/Name.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/MorphTargetWeights.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "Logger/Logger.h"
#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <glm/gtc/quaternion.hpp>
#include <mutex>
#include <string_view>

namespace GameEngine::Engine::Renderer
{
namespace
{

using namespace GameEngine::Components;
using Mathematics::Quaternion;
using Mathematics::Vector3;

std::mutex g_MethodCallbackMutex;
std::unordered_map<std::string, TimelineMethodCallback> g_MethodCallbacks;
ManagedTimelineMethodListener g_ManagedMethodListener = nullptr;

float32 DbToLinear(float32 volumeDb)
{
    return std::pow(10.0f, volumeDb / 20.0f);
}

Quaternion RotationFromSample(const Animation::TimelineValueSample& sample)
{
    if (sample.ComponentCount >= 4)
    {
        return Quaternion(sample.Value[3], sample.Value[0], sample.Value[1], sample.Value[2]).Normalized();
    }
    const float32 degToRad = 3.14159265358979323846f / 180.0f;
    const glm::quat q = glm::quat(glm::vec3(sample.Value[0] * degToRad,
                                             sample.Value[1] * degToRad,
                                             sample.Value[2] * degToRad));
    return Quaternion(q);
}

bool PathSegmentEquals(std::string_view segment, std::string_view entityName)
{
    return !segment.empty() && segment == entityName;
}

ECS::EntityHandle FindChildByName(ECS::World& world,
                                  ECS::EntityHandle parent,
                                  std::string_view segment)
{
    ECS::EntityHandle found;
    // Path binding resolves names against the authored hierarchy, which
    // includes branches that are currently switched off.
    world.Query<ECS::Read<Parent>, ECS::Read<Name>>().IncludeDisabled().Each([&](ECS::EntityHandle entity, const Parent& p, const Name& name)
    {
        if (found.IsValid())
            return;
        if (p.parent != parent)
            return;
        if (PathSegmentEquals(segment, name.View()))
            found = entity;
    });
    return found;
}

void ApplyAnimatorClipToSubtree(ECS::World& world,
                                const std::vector<ECS::EntityHandle>& subtree,
                                uint32_t clipIndex,
                                float32 localTimeSeconds,
                                float32 speedScale,
                                bool loop)
{
    for (ECS::EntityHandle entity : subtree)
    {
        auto* anim = world.GetComponent<AnimatorRef>(entity);
        auto* skel = world.GetComponent<SkeletonRef>(entity);
        if (!anim || !skel)
            continue;

        AnimatorRef updated = *anim;
        updated.ClipIndex = clipIndex;
        updated.Time = localTimeSeconds;
        updated.Speed = speedScale;
        // Timeline drives absolute clip time each frame; AnimationSystem must not add deltaTime.
        updated.Flags |= AnimatorRef::kFlag_Paused;
        if (loop)
            updated.Flags |= AnimatorRef::kFlag_Loop;
        else
            updated.Flags &= ~AnimatorRef::kFlag_Loop;
        world.AddComponentImmediate(entity, updated);
    }
}

void ApplyScalarPropertyPath(Components::Light* light,
                             Components::PostProcessVolume* postProcess,
                             std::string_view propertyPath,
                             float32 value)
{
    if (propertyPath == "Light.intensity" || propertyPath == "Light.Intensity")
    {
        if (light)
            light->Intensity = value;
        return;
    }
    if (propertyPath == "PostProcessVolume.weight" || propertyPath == "PostProcess.Weight")
    {
        if (postProcess)
            postProcess->Weight = value;
        return;
    }
    // Exposure moved off the volume onto the camera; a legacy "PostProcessVolume.exposure" track path
    // no longer matches anything and is ignored.
}

void ApplyValueSample(ECS::World& world,
                      ECS::EntityHandle target,
                      const Animation::TimelineValueSample& sample)
{
    // Write grants are branch-scoped: each track type dirties only the column it
    // actually writes (a Light-only track must not re-trigger Changed<Transform> gates).
    switch (sample.TrackType)
    {
    case Animation::TimelineTrackType::Position3D:
    {
        auto* transform = world.GetComponentForWrite<Transform>(target);
        if (!transform)
            return;
        const Vector3 position(sample.Value[0], sample.Value[1], sample.Value[2]);
        *transform = Transform::FromTRS(position, transform->GetRotation(), transform->GetScale());
        return;
    }
    case Animation::TimelineTrackType::Rotation3D:
    {
        auto* transform = world.GetComponentForWrite<Transform>(target);
        if (!transform)
            return;
        const Quaternion rotation = RotationFromSample(sample);
        *transform = Transform::FromTRS(transform->GetPosition(), rotation, transform->GetScale());
        return;
    }
    case Animation::TimelineTrackType::Scale3D:
    {
        auto* transform = world.GetComponentForWrite<Transform>(target);
        if (!transform)
            return;
        const Vector3 scale(sample.Value[0], sample.Value[1], sample.Value[2]);
        *transform = Transform::FromTRS(transform->GetPosition(), transform->GetRotation(), scale);
        return;
    }
    case Animation::TimelineTrackType::Property:
    case Animation::TimelineTrackType::Bezier:
    {
        // ApplyScalarPropertyPath only addresses Light.* / PostProcessVolume.* paths;
        // Transform.* paths are handled below and must not grant those columns.
        const bool transformPath = sample.PropertyPath.rfind("Transform.", 0) == 0;
        if (sample.ComponentCount >= 1 && !sample.PropertyPath.empty() && !transformPath)
        {
            auto* light = world.GetComponentForWrite<Light>(target);
            auto* postProcess = world.GetComponentForWrite<PostProcessVolume>(target);
            ApplyScalarPropertyPath(light, postProcess, sample.PropertyPath, sample.Value[0]);
        }

        if (!transformPath)
            return;

        auto* transform = world.GetComponentForWrite<Transform>(target);
        if (!transform)
            return;

        if (sample.PropertyPath == "Transform.position" && sample.ComponentCount >= 3)
        {
            const Vector3 position(sample.Value[0], sample.Value[1], sample.Value[2]);
            *transform = Transform::FromTRS(position, transform->GetRotation(), transform->GetScale());
            return;
        }
        if (sample.PropertyPath == "Transform.scale" && sample.ComponentCount >= 3)
        {
            const Vector3 scale(sample.Value[0], sample.Value[1], sample.Value[2]);
            *transform = Transform::FromTRS(transform->GetPosition(), transform->GetRotation(), scale);
            return;
        }
        if (sample.PropertyPath == Animation::kTransformRotationPropertyPath && sample.ComponentCount >= 3)
        {
            const Quaternion rotation = RotationFromSample(sample);
            *transform = Transform::FromTRS(transform->GetPosition(), rotation, transform->GetScale());
            return;
        }
        return;
    }
    case Animation::TimelineTrackType::BlendShape:
    {
        auto* morph = world.GetComponentForWrite<MorphTargetWeights>(target);
        if (!morph || sample.ComponentCount < 1)
            return;
        if (morph->weightCount > 0)
        {
            morph->weights[0] = sample.Value[0];
            ++morph->version;
            world.AddComponentImmediate(target, *morph);
        }
        return;
    }
    default:
        return;
    }
}

void PlayTimelineAudioEvent(Audio::AudioSystem& audio,
                           const Animation::TimelineAudioEvent& event,
                           ECS::World& world,
                           ECS::EntityHandle rootEntity,
                           std::vector<Audio::AudioEmitterHandle>& activeVoices,
                           uint32_t maxPolyphony)
{
    if (event.AudioGuid.IsNull())
        return;

    Audio::PlayOptions options;
    options.volume = DbToLinear(event.VolumeDb);
    options.pitch = std::max(0.01f, event.PitchScale);
    options.loop = false;

    Audio::AudioEmitterHandle voice = Audio::INVALID_AUDIO_EMITTER_HANDLE;
    if (const auto* worldXf = world.GetComponent<WorldTransform>(rootEntity))
    {
        const Vector3 pos(worldXf->matrix[12], worldXf->matrix[13], worldXf->matrix[14]);
        voice = audio.Play3D(event.AudioGuid, 0, pos, Vector3{}, options);
    }
    else if (const auto* localXf = world.GetComponent<Transform>(rootEntity))
    {
        const Vector3 pos = localXf->GetPosition();
        voice = audio.Play3D(event.AudioGuid, 0, pos, Vector3{}, options);
    }
    else
    {
        voice = audio.Play2D(event.AudioGuid, options);
    }

    if (voice == Audio::INVALID_AUDIO_EMITTER_HANDLE)
        return;

    activeVoices.push_back(voice);
    while (activeVoices.size() > maxPolyphony)
    {
        audio.Stop(activeVoices.front());
        activeVoices.erase(activeVoices.begin());
    }
}

} // namespace

uint32_t ResolveClipIndex(const GUID& clipGuid, AssetManager& assetManager)
{
    if (clipGuid.IsNull())
        return 0;

    auto& clipStore = ClipStore::Instance();
    if (uint32_t idx = clipStore.GetIndexIfPresent(clipGuid); idx != 0)
        return idx;

    return clipStore.GetOrLoadClipIndex(clipGuid, assetManager);
}

SharedPtr<Asset> TryAcquireTimelineAsset(AssetManager& assetManager, const GUID& timelineGuid)
{
    if (timelineGuid.IsNull())
        return nullptr;
    if (SharedPtr<Asset> cached = assetManager.GetAsset(timelineGuid))
        return cached;
    AssetFuture future = assetManager.LoadAssetAsync(timelineGuid, AssetLoadPriority::High);
    if (!future.Valid())
        return nullptr;
    return future.get();
}

bool LoadTimelineFromAsset(const TimelineAsset& timelineAsset, Animation::Timeline& outTimeline, std::string* outError)
{
    return Animation::LoadTimelineFromJson(timelineAsset.GetDocument(), outTimeline, outError);
}

void CollectEntitiesInSubtree(ECS::World& world, ECS::EntityHandle root, std::vector<ECS::EntityHandle>& outSubtree)
{
    outSubtree.clear();
    if (!root.IsValid())
        return;

    outSubtree.push_back(root);
    std::vector<ECS::EntityHandle> descendants;
    DescendantsOf(world, root, descendants);
    outSubtree.insert(outSubtree.end(), descendants.begin(), descendants.end());
}

ECS::EntityHandle ResolveEntityByTargetPath(ECS::World& world,
                                            ECS::EntityHandle rootEntity,
                                            std::string_view targetPath,
                                            const std::vector<ECS::EntityHandle>& subtree,
                                            std::string_view rootNodeOverride)
{
    ECS::EntityHandle bindRoot = rootEntity;
    if (!rootNodeOverride.empty())
    {
        const ECS::EntityHandle resolved = ResolveEntityByTargetPath(world, rootEntity, rootNodeOverride, subtree, {});
        if (resolved.IsValid())
            bindRoot = resolved;
    }

    if (targetPath.empty())
        return bindRoot;

    const std::string path(targetPath);
    size_t start = 0;
    ECS::EntityHandle current = bindRoot;
    while (start <= path.size())
    {
        const size_t slash = path.find('/', start);
        const std::string_view segment = (slash == std::string::npos)
                                             ? std::string_view(path).substr(start)
                                             : std::string_view(path).substr(start, slash - start);
        if (!segment.empty())
        {
            const auto* currentName = world.GetComponent<Name>(current);
            if (currentName && PathSegmentEquals(segment, currentName->View()))
            {
                // Path segment names the current node.
            }
            else
            {
                const ECS::EntityHandle child = FindChildByName(world, current, segment);
                if (!child.IsValid())
                    return ECS::EntityHandle{};
                current = child;
            }
        }
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return current;
}

bool PickDominantAnimationClip(const Animation::TimelineEvaluationResult& evaluation,
                               AssetManager& assetManager,
                               ResolvedTimelineAnimationClip& outClip)
{
    float32 bestWeight = -1.0f;
    ResolvedTimelineAnimationClip best;
    for (const auto& sample : evaluation.ActiveClips)
    {
        if (sample.ClipAssetType != AssetType::Animation)
            continue;
        const uint32_t clipIndex = ResolveClipIndex(sample.ClipGuid, assetManager);
        if (clipIndex == 0)
            continue;
        if (sample.Weight >= bestWeight)
        {
            bestWeight = sample.Weight;
            best.clipIndex = clipIndex;
            best.localTimeSeconds = sample.LocalTime;
            best.weight = sample.Weight;
        }
    }
    if (best.clipIndex == 0)
        return false;
    outClip = best;
    return true;
}

void ApplyTimelineEvaluation(const Animation::TimelineEvaluationResult& evaluation,
                             const TimelinePlaybackApplyContext& context)
{
    if (!context.world || !context.assetManager || !context.subtree)
        return;

    ECS::World& world = *context.world;
    ResolvedTimelineAnimationClip clip;
    if (PickDominantAnimationClip(evaluation, *context.assetManager, clip))
    {
        ApplyAnimatorClipToSubtree(world,
                                   *context.subtree,
                                   clip.clipIndex,
                                   clip.localTimeSeconds,
                                   context.speedScale,
                                   context.loop);
    }

    for (const auto& sample : evaluation.ValueSamples)
    {
        const ECS::EntityHandle target = ResolveEntityByTargetPath(world,
                                                                   context.rootEntity,
                                                                   sample.TargetPath,
                                                                   *context.subtree,
                                                                   context.rootNodePath);
        if (!target.IsValid())
            continue;
        ApplyValueSample(world, target, sample);
    }

    if (context.audioSystem && context.activeVoices)
    {
        for (const auto& audioEvent : evaluation.AudioEvents)
            PlayTimelineAudioEvent(*context.audioSystem,
                                   audioEvent,
                                   world,
                                   context.rootEntity,
                                   *context.activeVoices,
                                   context.audioMaxPolyphony);
    }

    DispatchTimelineMethodEvents(world, context.rootEntity, evaluation.MethodEvents);
}

void RegisterTimelineMethodCallback(std::string methodName, TimelineMethodCallback callback)
{
    std::lock_guard lock(g_MethodCallbackMutex);
    g_MethodCallbacks[std::move(methodName)] = std::move(callback);
}

void UnregisterTimelineMethodCallback(std::string_view methodName)
{
    std::lock_guard lock(g_MethodCallbackMutex);
    g_MethodCallbacks.erase(std::string(methodName));
}

void ClearTimelineMethodCallbacks()
{
    std::lock_guard lock(g_MethodCallbackMutex);
    g_MethodCallbacks.clear();
}

void SetManagedTimelineMethodListener(ManagedTimelineMethodListener listener)
{
    g_ManagedMethodListener = listener;
}

void DispatchTimelineMethodEvents(ECS::World& world,
                                  ECS::EntityHandle rootEntity,
                                  const std::vector<Animation::TimelineMethodEvent>& events)
{
    for (const auto& event : events)
    {
        if (g_ManagedMethodListener)
        {
            g_ManagedMethodListener(static_cast<uint64_t>(rootEntity.id),
                                    event.MethodName.c_str(),
                                    event.Arguments.c_str());
        }
    }

    std::lock_guard lock(g_MethodCallbackMutex);
    for (const auto& event : events)
    {
        const auto it = g_MethodCallbacks.find(event.MethodName);
        if (it != g_MethodCallbacks.end())
            it->second(world, rootEntity, event);
        else if (!g_ManagedMethodListener)
            Logger::Log::Debug("Timeline method event '{}' had no registered callback", event.MethodName);
    }
}

void EnsureTimelinePlaybackState(ECS::World& world, ECS::EntityHandle entity, const TimelinePlaybackState& initial)
{
    if (world.GetComponent<TimelinePlaybackState>(entity))
        world.AddComponentImmediate(entity, initial);
    else
        world.AddComponentImmediate(entity, initial);
}

} // namespace GameEngine::Engine::Renderer
