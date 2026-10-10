#include "Scripting/AnimatorABI.h"

#include "AbiHandles.h"
#include "AbiMainThread.h"
#include "Animation/AnimationEvent.h"
#include "Animation/AnimationEventCollectorStore.h"
#include "Components/Animation/Animator.h"
#include "Core/Engine.h"
#include "DllEngineBootstrap.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Engine/Rendering/AnimatorScenePlayback.h"
#include "Logger/Logger.h"
#include "Types/StringId.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <exception>
#include <span>
#include <string_view>

namespace
{

using GameEngine::Components::Animator;
using GameEngine::DllBootstrap::EnsureEngineInitialized;
using GameEngine::ECS::EntityHandle;
using GameEngine::ECS::World;
using GameEngine::Engine::Renderer::ProcessClipAnimatorCommands;
using GameEngine::ScriptingAbi::EntityFromId;
using GameEngine::ScriptingAbi::GuidFromAbiBytes;
using GameEngine::ScriptingAbi::WorldFromHandle;

static Animator* EnsureAnimator(World& world, EntityHandle entity)
{
    auto* animator = world.GetComponentForWrite<Animator>(entity);
    if (animator)
        return animator;
    world.AddComponentImmediate(entity, Animator{});
    return world.GetComponentForWrite<Animator>(entity);
}

template <typename Stamp>
static GE_Result StampAndApply(World* world, uint64_t entityId, Stamp&& stamp)
{
    if (EnsureEngineInitialized() != GE_Result_Ok)
        return GE_Result_NotInitialized;
    if (!world)
        return GE_Result_InvalidArg;
    const EntityHandle entity = EntityFromId(entityId);
    if (!world->IsValid(entity))
        return GE_Result_NotFound;
    Animator* animator = EnsureAnimator(*world, entity);
    if (!animator)
        return GE_Result_Fail;
    stamp(*animator);
    ProcessClipAnimatorCommands(*world);
    return GE_Result_Ok;
}

} // namespace

GE_API GE_Result GE_CDECL GE_Animator_PlayStateOnEntity(GE_Handle worldHandle,
                                                        uint64_t entityId,
                                                        const char* animationNameUtf8,
                                                        uint32_t animationNameLength)
{
    if (!animationNameUtf8 && animationNameLength > 0)
        return GE_Result_InvalidArg;
    const std::string_view name(animationNameUtf8 ? animationNameUtf8 : "", animationNameLength);
    return StampAndApply(WorldFromHandle(worldHandle), entityId, [&](Animator& animator) {
        animator.PlayState(name);
    });
}

GE_API GE_Result GE_CDECL GE_Animator_PlayStateClipOnEntity(GE_Handle worldHandle,
                                                            uint64_t entityId,
                                                            const uint8_t clipGuidBytes[16])
{
    if (!clipGuidBytes)
        return GE_Result_InvalidArg;
    return StampAndApply(WorldFromHandle(worldHandle), entityId, [&](Animator& animator) {
        animator.PlayState(GuidFromAbiBytes(clipGuidBytes));
    });
}

GE_API GE_Result GE_CDECL GE_Animator_CrossFadeSecondsOnEntity(GE_Handle worldHandle,
                                                               uint64_t entityId,
                                                               const char* animationNameUtf8,
                                                               uint32_t animationNameLength,
                                                               float blendSeconds)
{
    if (!animationNameUtf8 && animationNameLength > 0)
        return GE_Result_InvalidArg;
    const std::string_view name(animationNameUtf8 ? animationNameUtf8 : "", animationNameLength);
    return StampAndApply(WorldFromHandle(worldHandle), entityId, [&](Animator& animator) {
        animator.CrossFadeSeconds(name, blendSeconds);
    });
}

GE_API GE_Result GE_CDECL GE_Animator_CrossFadeSecondsClipOnEntity(GE_Handle worldHandle,
                                                                   uint64_t entityId,
                                                                   const uint8_t clipGuidBytes[16],
                                                                   float blendSeconds)
{
    if (!clipGuidBytes)
        return GE_Result_InvalidArg;
    return StampAndApply(WorldFromHandle(worldHandle), entityId, [&](Animator& animator) {
        animator.CrossFadeSeconds(GuidFromAbiBytes(clipGuidBytes), blendSeconds);
    });
}

GE_API GE_Result GE_CDECL GE_Animator_PauseOnEntity(GE_Handle worldHandle, uint64_t entityId)
{
    return StampAndApply(WorldFromHandle(worldHandle), entityId, [&](Animator& animator) {
        animator.Pause();
    });
}

GE_API GE_Result GE_CDECL GE_Animator_StopOnEntity(GE_Handle worldHandle, uint64_t entityId)
{
    return StampAndApply(WorldFromHandle(worldHandle), entityId, [&](Animator& animator) {
        animator.Stop();
    });
}

GE_API GE_Result GE_CDECL GE_Animator_SeekOnEntity(GE_Handle worldHandle, uint64_t entityId, float timeSeconds)
{
    if (!std::isfinite(timeSeconds))
    {
        Logger::Log::Error("AnimatorApi.Seek ignored non-finite timeSeconds={} for entity={}: "
                           "use a finite time in seconds.", timeSeconds, entityId);
        return GE_Result_InvalidArg;
    }
    return StampAndApply(WorldFromHandle(worldHandle), entityId, [&](Animator& animator) {
        animator.Seek(timeSeconds);
    });
}

namespace
{

template <typename Stamp>
static GE_Result StampOnly(World* world, uint64_t entityId, Stamp&& stamp)
{
    if (EnsureEngineInitialized() != GE_Result_Ok)
        return GE_Result_NotInitialized;
    if (!world)
        return GE_Result_InvalidArg;
    const EntityHandle entity = EntityFromId(entityId);
    if (!world->IsValid(entity))
        return GE_Result_NotFound;
    Animator* animator = EnsureAnimator(*world, entity);
    if (!animator)
        return GE_Result_Fail;
    stamp(*animator);
    return GE_Result_Ok;
}

} // namespace

GE_API GE_Result GE_CDECL GE_Animator_PlayGraphOnEntity(GE_Handle worldHandle,
                                                        uint64_t entityId,
                                                        const uint8_t graphGuidBytes[16])
{
    if (!graphGuidBytes)
        return GE_Result_InvalidArg;
    return StampOnly(WorldFromHandle(worldHandle), entityId, [&](Animator& animator) {
        animator.PlayGraph(GuidFromAbiBytes(graphGuidBytes));
    });
}

GE_API GE_Result GE_CDECL GE_Animator_SetFloatOnEntity(GE_Handle worldHandle,
                                                       uint64_t entityId,
                                                       const char* nameUtf8,
                                                       uint32_t nameLength,
                                                       float value)
{
    if (!nameUtf8 && nameLength > 0)
        return GE_Result_InvalidArg;
    const std::string_view name(nameUtf8 ? nameUtf8 : "", nameLength);
    return StampOnly(WorldFromHandle(worldHandle), entityId, [&](Animator& animator) {
        animator.SetFloat(name, value);
    });
}

GE_API GE_Result GE_CDECL GE_Animator_SetBoolOnEntity(GE_Handle worldHandle,
                                                      uint64_t entityId,
                                                      const char* nameUtf8,
                                                      uint32_t nameLength,
                                                      int32_t value)
{
    if (!nameUtf8 && nameLength > 0)
        return GE_Result_InvalidArg;
    const std::string_view name(nameUtf8 ? nameUtf8 : "", nameLength);
    return StampOnly(WorldFromHandle(worldHandle), entityId, [&](Animator& animator) {
        animator.SetBool(name, value != 0);
    });
}

GE_API GE_Result GE_CDECL GE_Animator_SetTriggerOnEntity(GE_Handle worldHandle,
                                                         uint64_t entityId,
                                                         const char* nameUtf8,
                                                         uint32_t nameLength)
{
    if (!nameUtf8 && nameLength > 0)
        return GE_Result_InvalidArg;
    const std::string_view name(nameUtf8 ? nameUtf8 : "", nameLength);
    return StampOnly(WorldFromHandle(worldHandle), entityId, [&](Animator& animator) {
        animator.SetTrigger(name);
    });
}

namespace
{

static_assert(sizeof(GE_AnimationEventRecord) == 80, "AnimationEventRecord in Managed/Scripting.ABI mirrors 80 bytes");
static_assert(offsetof(GE_AnimationEventRecord, nameId) == 0);
static_assert(offsetof(GE_AnimationEventRecord, timeSeconds) == 8);
static_assert(offsetof(GE_AnimationEventRecord, nameLength) == 12);
static_assert(offsetof(GE_AnimationEventRecord, name) == 16);

// Logs, once per process, that GE_Animator_PollEventsOnEntity was refused off the main thread: a script that polls
// from a worker every frame would otherwise fill the log with the same line.
void ReportPollRefusedOffMainThread()
{
    static std::atomic<bool> s_Reported{false};
    if (s_Reported.exchange(true))
        return;
    Logger::Log::Error("GE_Animator_PollEventsOnEntity (AnimatorApi.PollEvents) was called from a thread other than "
                       "the engine's main thread and was refused, because the animation wave writes the events on "
                       "worker threads. Poll them from a system's update.");
}

GE_AnimationEventRecord ToRecord(const GameEngine::Animation::AnimationEvent& event)
{
    GE_AnimationEventRecord record{};
    record.nameId = GameEngine::HashStringId(event.Name);
    record.timeSeconds = event.Time;
    record.nameLength = static_cast<uint32_t>(event.Name.size());
    std::memcpy(record.name, event.Name.data(), std::min<size_t>(event.Name.size(), sizeof(record.name)));
    return record;
}

GE_Result PollEvents(World* world, uint64_t entityId, GE_AnimationEventRecord* records, int32_t recordCapacity,
                     int32_t* outCount)
{
    if (!world)
        return GE_Result_InvalidArg;
    if (!GameEngine::ScriptingAbi::OnEngineMainThread())
    {
        ReportPollRefusedOffMainThread();
        return GE_Result_Fail;
    }
    const EntityHandle entity = EntityFromId(entityId);
    if (!world->IsValid(entity))
        return GE_Result_NotFound;
    const Animator* animator = world->GetComponent<Animator>(entity);
    if (!animator)
        return GE_Result_NotFound;

    GameEngine::Animation::AnimationEventCollector* collector =
        GameEngine::Animation::AnimationEventCollectorStore::Instance().Get(animator->eventCollectorId);
    if (!collector)
        return GE_Result_Ok;
    const std::span<const GameEngine::Animation::FiredEvent> unpolled = collector->GetUnpolledEvents();
    const size_t count = std::min(unpolled.size(), static_cast<size_t>(recordCapacity));
    for (size_t i = 0; i < count; ++i)
        records[i] = ToRecord(unpolled[i].Event);
    collector->MarkPolled(count);
    *outCount = static_cast<int32_t>(count);
    return GE_Result_Ok;
}

} // namespace

GE_API GE_Result GE_CDECL GE_Animator_PollEventsOnEntity(GE_Handle worldHandle,
                                                         uint64_t entityId,
                                                         GE_AnimationEventRecord* records,
                                                         int32_t recordCapacity,
                                                         int32_t* outCount)
{
    try
    {
        if (outCount)
            *outCount = 0;
        if (!outCount || recordCapacity < 0 || (!records && recordCapacity != 0))
            return GE_Result_InvalidArg;
        return PollEvents(WorldFromHandle(worldHandle), entityId, records, recordCapacity, outCount);
    }
    catch (const std::exception& exception)
    {
        Logger::Log::Error("GE_Animator_PollEventsOnEntity (AnimatorApi.PollEvents) failed: {}", exception.what());
        return GE_Result_Fail;
    }
    catch (...)
    {
        Logger::Log::Error("GE_Animator_PollEventsOnEntity (AnimatorApi.PollEvents) failed with an unknown exception.");
        return GE_Result_Fail;
    }
}
