#include "Particles/Systems/ParticleSimulationSystem.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Components/Name.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "Logger/Logger.h"
#include "Particles/Assets/ParticleStackAsset.h"
#include "PhysicsECS/PhysicsWorldService.h"

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace GameEngine::Particles
{
namespace
{
using namespace Components;
using Clock = std::chrono::steady_clock;

std::shared_ptr<const CompiledParticleStack> DefaultStack()
{
    static const std::shared_ptr<const CompiledParticleStack> stack = []
    {
        std::vector<StackDiagnostic> diagnostics;
        return CompileParticleStack(std::make_shared<const StackDocument>(MakeDefaultStack()), diagnostics);
    }();
    return stack;
}

uint32 Capacity(const ParticleEmitter3D& emitter)
{
    return std::min(emitter.Amount, kMaxParticlesPerEmitter);
}

std::string EntityLabel(const ECS::World& world, ECS::EntityHandle entity)
{
    if (const auto* name = world.GetComponent<Name>(entity); name && name->value[0] != '\0')
        return name->value;
    return "entity " + std::to_string(entity.id);
}

// The seed of an emitter whose Seed is 0, mixed with its entity id.
constexpr uint32 kEntitySeed = 0xC0FFEEu;

// Hands the events a game queued on the emitter's ParticlePlayback to its simulation, which runs the
// stack's game-event rules for them on its next tick.
void DeliverGameEvents(ParticlePlayback& playback, ParticleRuntime& simulation)
{
    const uint32 count = std::min(playback.PendingEventCount, ParticleMaxPendingEvents);
    for (uint32 index = 0; index < count; ++index)
        simulation.SendEvent(playback.PendingEvents[index]);
    playback.PendingEventCount = 0;
}

// A missing stack asset is asked for again at most this often, in frames.
constexpr uint64 kStackLoadRetryFrames = 60;

const ParticleWorldState::StackLoad& RequestStackLoad(ParticleWorldState& state, const GUID& guid)
{
    auto& load = state.StackLoads[guid];
    if (state.Frame < load.RetryFrame)
        return load;
    load.RetryFrame = state.Frame + kStackLoadRetryFrames;
    state.Assets->LoadAsset(
        guid, [failed = load.Failed](Result<SharedPtr<Asset>, AssetError> result)
        { failed->store(!result.IsOk()); },
        AssetLoadPriority::High);
    return load;
}

// The compiled stack the emitter's reference resolves to now, and why there is none when there is
// not: the default stack for no reference, the asset's stack once it is loaded. A stack still
// loading leaves `error` empty: there is nothing wrong to report yet.
std::shared_ptr<const CompiledParticleStack> ResolveStack(ParticleWorldState& state, const GUID& guid, std::string& error)
{
    if (guid.IsNull())
        return DefaultStack();
    if (!state.Assets)
    {
        error = "no asset manager resolves its stack";
        return nullptr;
    }
    const auto loaded = state.Assets->GetAsset(guid);
    const auto asset = std::dynamic_pointer_cast<ParticleStackAsset>(loaded);
    if (loaded && !asset)
    {
        error = "its stack reference names an asset that is not a particle stack";
        return nullptr;
    }
    if (!asset)
    {
        const auto& load = RequestStackLoad(state, guid);
        if (!state.Assets->GetRegistry().IsAssetRegistered(guid))
            error = "its stack asset is missing";
        else if (load.Failed->load())
            error = "its stack asset did not load";
        return nullptr;
    }
    auto compiled = asset->Compiled();
    if (!compiled)
        error = asset->Diagnostics().empty() ? "its stack asset holds no valid stack"
                                             : asset->Diagnostics().front().Path + ": " + asset->Diagnostics().front().Message;
    return compiled;
}

// Binds the stack the emitter references when it changed, keeping live particles when only
// parameters did. Logs once per change when the emitter cannot run a stack.
void UpdateStack(const ECS::World& world, ParticleWorldState& state, ParticleEmitterState& entry)
{
    std::string error;
    auto compiled = ResolveStack(state, entry.Emitter.Stack.ToGuid(), error);
    // Reported before the unchanged check: an emitter whose stack never resolved holds no stack
    // either, and must still say why.
    if (!compiled && !error.empty() && error != entry.StackError)
        Logger::Log::Warning("Particle emitter '{}' emits nothing: {}", EntityLabel(world, entry.Entity), error);
    entry.StackError = compiled ? std::string() : std::move(error);
    const auto capacity = Capacity(entry.Emitter);
    if (compiled == entry.Stack && entry.Simulation.Capacity() == (compiled ? capacity : 0u))
        return;
    // Parameter-only edits keep live particles; anything that changes the layout restarts.
    const bool keep = compiled && entry.Stack && entry.Simulation.Capacity() == capacity &&
                      HaveSameStructure(*entry.Stack->Document, *compiled->Document) &&
                      entry.Simulation.Rebind(compiled);
    if (!keep)
    {
        entry.Simulation.Bind(compiled, capacity);
        entry.ResumeSeek = entry.PreviewSeek.Pending;
        entry.PreviewSeek = {};
    }
    entry.Stack = std::move(compiled);
}

void GatherEmitter(ECS::World& world, ParticleWorldState& state, ECS::EntityHandle entity,
                   const WorldTransform& transform, const ParticleEmitter3D& emitter)
{
    auto found = state.Emitters.find(entity.id);
    if (found != state.Emitters.end() && found->second.Entity != entity)
    {
        state.Emitters.erase(found);
        found = state.Emitters.end();
    }
    if (found == state.Emitters.end())
        found = state.Emitters.try_emplace(entity.id).first;
    auto& entry = found->second;
    const bool restart = entry.Seen != 0 &&
                         (entry.Emitter.LocalSpace != emitter.LocalSpace || entry.Emitter.Dimension != emitter.Dimension);
    entry.Entity = entity;
    entry.Seen = state.Frame;
    entry.Emitter = emitter;
    entry.Transform = Mathematics::Matrix4x4{glm::make_mat4(transform.matrix)};
    UpdateStack(world, state, entry);
    if (restart)
    {
        entry.Simulation.Reset(0);
        entry.ResumeSeek = entry.ResumeSeek || entry.PreviewSeek.Pending;
        entry.PreviewSeek = {};
    }
}

struct CollisionContext
{
    Physics::PhysicsWorld* Physics = nullptr;
};

bool SweepPhysics(const void* context, const Mathematics::Vector3& from, const Mathematics::Vector3& to, float radius,
                  uint32 layerMask, CollisionHit& hit)
{
    auto* physics = static_cast<const CollisionContext*>(context)->Physics;
    if (!physics)
        return false;
    const auto delta = to - from;
    const float distance = delta.Length();
    constexpr float kMinimumSweepDistance = 1e-6f;
    if (distance < kMinimumSweepDistance)
        return false;
    Physics::RayCastQuery query;
    query.ray.origin = from;
    query.ray.direction = delta / distance;
    // Extend the ray by the particle radius; the collision processor places the center off the normal.
    query.maxDistance = distance + radius;
    query.filter.layerMask = layerMask;
    query.filter.ignoreSensors = true;
    Physics::RayCastResult result;
    if (!physics->RayCast(query, result))
        return false;
    hit.Position = {result.hitPoint.x, result.hitPoint.y, result.hitPoint.z};
    hit.Normal = {result.hitNormal.x, result.hitNormal.y, result.hitNormal.z};
    return true;
}

void PublishCollisions(ECS::World& world, const ParticleEmitterState& entry,
                       ParticleCollisionEventsBuffer* fallbackEvents)
{
    const auto collisions = entry.Simulation.Collisions();
    if (collisions.empty())
        return;
    // A switched-off buffer on the emitter reads as absent, as the fallback query skips it.
    ParticleCollisionEventsBuffer* events = nullptr;
    if (ECS::Entity(&world, entry.Entity).IsEnabled<ParticleCollisionEventsBuffer>())
        events = world.GetComponentForWrite<ParticleCollisionEventsBuffer>(entry.Entity);
    if (!events)
        events = fallbackEvents;
    if (!events)
        return;
    for (const auto& collision : collisions)
    {
        ParticleCollisionEvent event{};
        event.Emitter = entry.Entity;
        event.ParticleId = collision.ParticleId;
        event.KilledParticle = collision.Killed;
        event.Position = collision.Hit.Position;
        event.Normal = collision.Hit.Normal;
        event.Velocity = collision.Velocity;
        event.Speed = collision.Velocity.Length();
        if (!events->Push(event))
            break;
    }
}
} // namespace

void ParticleSimulationSystem::RouteSpawnEvents(ECS::World& world, ParticleEmitterState& source)
{
    auto& state = *m_State;
    for (const auto& event : source.Simulation.SpawnEvents())
    {
        // Validation keeps a spawn rule's slot in range; the check keeps the index in bounds anyway.
        if (event.SubEmitter >= ParticleMaxSubEmitters)
            continue;
        const ECS::EntityHandle child = source.Emitter.SubEmitters[event.SubEmitter].Emitter;
        auto target = state.Emitters.find(child.id);
        if (target == state.Emitters.end() || target->second.Entity != child || !world.IsValid(child))
            continue;
        ParticleSpawnRequest request;
        request.Count = event.Count;
        request.Inherit = event.Inherit;
        request.Size = event.Size;
        request.Rotation = event.Rotation;
        request.Lifetime = event.Lifetime;
        request.AlignToNormal = event.AlignToNormal;
        request.Normal = event.Normal;
        request.Color = event.Color;
        request.Position = event.Position;
        request.Velocity = event.Velocity;
        target->second.Simulation.QueueSpawn(request);
    }
}

void ParticleSimulationSystem::Update(ECS::World& world, float32 deltaTime)
{
    const auto begin = Clock::now();
    auto& state = *m_State;
    if (state.WorldId != world.GetWorldId() ||
        state.LifecycleResetGeneration != world.GetLifecycleResetGeneration())
    {
        auto* assets = state.Assets;
        state = ParticleWorldState{};
        state.Assets = assets;
        state.WorldId = world.GetWorldId();
        state.LifecycleResetGeneration = world.GetLifecycleResetGeneration();
    }
    ++state.Frame;
    state.Budget = 65536u;
    world.Query<ECS::Read<ParticleWorldSettings>>().Each(
        [&state](ECS::EntityHandle, const ParticleWorldSettings& settings)
        { state.Budget = std::min(state.Budget, settings.MaxParticles); });

    ParticleCollisionEventsBuffer* fallbackEvents = nullptr;
    world.Query<ECS::Write<ParticleCollisionEventsBuffer>>().Each(
        [&fallbackEvents](ECS::EntityHandle, ParticleCollisionEventsBuffer& events)
        {
            events.Clear();
            if (!fallbackEvents)
                fallbackEvents = &events;
        });

    m_Order.clear();
    // A switched-off emitter is not visited, so its entry is erased below.
    world.Query<ECS::Read<WorldTransform>, ECS::Read<ParticleEmitter3D>>().Each(
        [this, &world, &state](ECS::EntityHandle entity, const WorldTransform& transform, const ParticleEmitter3D& emitter)
        {
            GatherEmitter(world, state, entity, transform, emitter);
            m_Order.push_back(entity.id);
        });
    std::erase_if(state.Emitters, [&state](const auto& pair)
                  { return pair.second.Seen != state.Frame; });
    std::sort(m_Order.begin(), m_Order.end()); // stable global-budget and sub-emitter order
    CollisionContext collisionContext{PhysicsECS::PhysicsWorldService::TryGet()};
    const ParticleCollisionWorld collisionWorld{SweepPhysics, &collisionContext};
    uint32 remaining = state.Budget;
    for (const auto id : m_Order)
    {
        auto& entry = state.Emitters.at(id);
        const auto emitterBegin = Clock::now();
        ParticleSimulationInput input;
        input.Transform = entry.Transform.Data();
        input.Seed = entry.Emitter.Seed != 0 ? entry.Emitter.Seed : kEntitySeed ^ static_cast<uint32>(id);
        input.Budget = std::min(remaining, Capacity(entry.Emitter));
        input.FixedFps = entry.Emitter.TicksPerSecond;
        input.SpeedScale = entry.Emitter.SimulationSpeed;
        input.Interpolate = entry.Emitter.Interpolate;
        input.LocalSpace = entry.Emitter.LocalSpace;
        input.Planar = entry.Emitter.Dimension == ParticleEmitterDimension::World2D;
        input.Emitting = entry.Emitter.Emitting;
        input.PrewarmSeconds = entry.Emitter.PrewarmSeconds;
        input.Collision = &collisionWorld;
        auto* playback = world.GetComponentForWrite<ParticlePlayback>(entry.Entity);
        if (playback)
            DeliverGameEvents(*playback, entry.Simulation);
        if (playback && playback->Restart != entry.Restart)
        {
            entry.Simulation.Reset(input.Seed);
            entry.PreviewSeek = {};
            entry.ResumeSeek = false;
            entry.Restart = playback->Restart;
        }
        if (playback && (playback->Seek != entry.Seek || entry.ResumeSeek))
        {
            // Replace unfinished work immediately when the slider moves again.
            entry.Simulation.BeginSeek(input, playback->SeekTime, entry.PreviewSeek);
            entry.Seek = playback->Seek;
            entry.ResumeSeek = false;
            entry.Simulation.ContinueSeek(input, entry.PreviewSeek);
        }
        else if (playback && playback->SingleStep != entry.SingleStep)
        {
            entry.PreviewSeek = {};
            entry.Simulation.Step(input);
            entry.SingleStep = playback->SingleStep;
        }
        else if (playback && entry.PreviewSeek.Pending)
            entry.Simulation.ContinueSeek(input, entry.PreviewSeek);
        else
        {
            entry.PreviewSeek = {};
            entry.Simulation.Advance(input, playback && playback->Paused ? 0.0 : deltaTime);
        }
        remaining -= std::min(remaining, entry.Simulation.Count());
        PublishCollisions(world, entry, fallbackEvents);
        if (playback)
        {
            playback->LiveCount = entry.Simulation.Count();
            playback->SimulatedSteps = entry.Simulation.StepsLastFrame();
            playback->SimulatedTime = entry.Simulation.Elapsed();
            playback->DroppedTime = entry.Simulation.DroppedTime();
            playback->SimulationMs = std::chrono::duration<float, std::milli>(Clock::now() - emitterBegin).count();
        }
    }
    // Children receive their requests on their next update; cycles cannot recurse within a frame,
    // and both the event count and the world's particle budget bound the work.
    for (const auto id : m_Order)
        RouteSpawnEvents(world, state.Emitters.at(id));
    state.LiveParticles = state.Budget - remaining;
    state.SimulationMs = std::chrono::duration<float, std::milli>(Clock::now() - begin).count();
}
} // namespace GameEngine::Particles
