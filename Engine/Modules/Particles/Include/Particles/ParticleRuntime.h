#pragma once

#include "Mathematics/Vector3.h"
#include "Mathematics/Vector4.h"
#include "Particles/CompiledParticleStack.h"
#include "Particles/ParticleChannels.h"
#include "Particles/ParticleProcessorRegistry.h"
#include "Particles/ParticleTrailStore.h"
#include "Particles/Processors/ParticleEventProcessor.h"
#include "Types/StringId.h"
#include "Types/Types.h"

#include <array>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace GameEngine::Particles
{

/// Most particles one emitter simulates.
inline constexpr uint32 kMaxParticlesPerEmitter = 4096;
/// Addresses every particle of an emitter in SendEvent.
inline constexpr uint32 kAllParticles = 0xFFFFFFFFu;

/// A contact a collision query reports, in world space.
struct CollisionHit
{
    Mathematics::Vector3 Position{};
    Mathematics::Vector3 Normal{0.0f, 1.0f, 0.0f};
};

/// A swept-sphere query into the world the emitter lives in. The caller owns Context; Sweep is
/// called once per particle and collision processor per tick, so it is a plain function pointer.
struct ParticleCollisionWorld
{
    bool (*Sweep)(const void* context, const Mathematics::Vector3& from, const Mathematics::Vector3& to, float radius,
                  uint32 layerMask, CollisionHit& hit) = nullptr;
    const void* Context = nullptr;
};

/// A first contact of a particle this tick, in world space, for gameplay listeners.
struct ParticleCollisionRecord
{
    uint32 ParticleId = 0;
    CollisionHit Hit{};
    Mathematics::Vector3 Velocity{};
    bool Killed = false;
};

/// An Emit action: `Count` particles for the sub-emitter slot the rule names, carrying what the child
/// inherits. Positions and velocities are in world space.
struct ParticleSpawnEvent
{
    ParticleEventTrigger Trigger = ParticleEventTrigger::Birth;
    uint32 ParticleId = 0;
    uint32 Count = 1;
    /// The emitter's sub-emitter slot the rule spawns into.
    uint32 SubEmitter = 0;
    Mathematics::Vector3 Position{};
    Mathematics::Vector3 Velocity{};
    Mathematics::Vector3 Normal{0.0f, 1.0f, 0.0f};
    float Size = 1.0f;
    Mathematics::Vector4 Color{1.0f, 1.0f, 1.0f, 1.0f};
    float Rotation = 0.0f;
    float Lifetime = 1.0f;
    uint32 Inherit = 0;
    bool AlignToNormal = false;
};

/// A parent's spawn event queued on the child emitter for its next tick.
struct ParticleSpawnRequest
{
    Mathematics::Vector3 Position{};
    Mathematics::Vector3 Velocity{};
    Mathematics::Vector3 Normal{0.0f, 1.0f, 0.0f};
    uint32 Count = 1;
    float Size = 1.0f;
    Mathematics::Vector4 Color{1.0f, 1.0f, 1.0f, 1.0f};
    float Rotation = 0.0f;
    float Lifetime = 1.0f;
    uint32 Inherit = 0;
    bool AlignToNormal = false;
};

/// What one emitter's simulation reads this frame.
struct ParticleSimulationInput
{
    /// Column-major world transform of the emitter; null is the identity.
    const float* Transform = nullptr;
    uint32 Seed = 1;
    /// Live particles the world budget allows this emitter this frame.
    uint32 Budget = kMaxParticlesPerEmitter;
    /// Ticks per second; 0 advances in bounded variable steps.
    uint32 FixedFps = 30;
    double SpeedScale = 1.0;
    bool Interpolate = true;
    bool LocalSpace = false;
    /// Keep particles on the emitter's XY plane.
    bool Planar = false;
    bool Emitting = true;
    /// Seconds simulated before the effect is shown, spread over the first frames.
    double PrewarmSeconds = 0.0;
    const ParticleCollisionWorld* Collision = nullptr;
};

/// Chunked seek state: the target time and how many of its ticks have run.
struct PreviewSeek
{
    double Target = 0.0;
    double Tick = 0.0;
    uint32 Completed = 0;
    uint32 Total = 0;
    bool Pending = false;
};

class ParticleRuntime;

/// The moments processors report to the runtime: kills and collisions. Only the runtime builds one.
class ParticleEventSink
{
  public:
    /// Kills particle `index` after running its Death rules at `hit` (or at its position).
    void Kill(uint32 index, const CollisionHit* hit = nullptr);
    /// Runs the Collision rules of particle `index` and records the contact for listeners.
    /// Returns false when the particle died or is leaving its phase.
    bool RaiseCollision(uint32 index, const CollisionHit& hit, bool killing);

  private:
    friend class ParticleRuntime;
    explicit ParticleEventSink(ParticleRuntime& runtime) : m_Runtime(runtime) {}
    ParticleRuntime& m_Runtime;
};

/// The CPU simulation of one emitter: fixed-step, deterministic for a seed, and bounded in work per
/// frame. It owns no graphics resources and runs in a headless world. Particle storage is one array
/// per channel the bound stack declares, sized to the emitter's capacity once; processors run once
/// per phase and stage over the particles in that phase.
class ParticleRuntime
{
  public:
    ParticleRuntime();
    ParticleRuntime(const ParticleRuntime&) = delete;
    ParticleRuntime& operator=(const ParticleRuntime&) = delete;

    /// Binds `stack` for up to `capacity` particles (at most kMaxParticlesPerEmitter) and resets.
    void Bind(std::shared_ptr<const CompiledParticleStack> stack, uint32 capacity);
    /// Replaces the stack with one of the same channels, phases and trail layout, keeping live
    /// particles. Returns false (and changes nothing) when `stack` needs a fresh Bind.
    bool Rebind(std::shared_ptr<const CompiledParticleStack> stack);
    const CompiledParticleStack* Stack() const { return m_Stack.get(); }
    uint32 Capacity() const { return m_Capacity; }

    /// Clears particles, trails and events and restarts emission. The next Advance prewarms again.
    void Reset(uint32 seed);

    /// Advances by `deltaTime` seconds of frame time in complete fixed ticks (at most eight per
    /// frame); the remainder carries over and drives interpolation without consuming randomness.
    void Advance(const ParticleSimulationInput& input, double deltaTime);
    /// Runs exactly one tick.
    void Step(const ParticleSimulationInput& input);
    /// Restarts and simulates to `time` over later ContinueSeek calls, without gameplay effects.
    void BeginSeek(const ParticleSimulationInput& input, double time, PreviewSeek& seek);
    void ContinueSeek(const ParticleSimulationInput& input, PreviewSeek& seek, uint32 maxTicks = 64,
                      double budgetSeconds = 0.002);
    /// BeginSeek and ContinueSeek to completion.
    void Seek(const ParticleSimulationInput& input, double time);

    /// Queues a named event for External rules on the next tick; kAllParticles broadcasts.
    bool SendEvent(StringId name, uint32 particleId = kAllParticles);
    /// Queues particles a parent emitter spawns into this one, for the next tick.
    bool QueueSpawn(const ParticleSpawnRequest& request);

    const ParticleChannels& Channels() const { return m_Channels; }
    uint32 Count() const { return m_Channels.Count(); }
    const ParticleTrailStore& Trails() const { return m_Trails; }
    std::span<const ParticleSpawnEvent> SpawnEvents() const { return m_SpawnEvents; }
    std::span<const ParticleCollisionRecord> Collisions() const { return m_Collisions; }

    double Elapsed() const { return m_Elapsed; }
    double DroppedTime() const { return m_DroppedTime; }
    /// Seconds the last tick simulated: the time the previous and current channels are apart.
    float LastTickSeconds() const { return m_LastTickSeconds; }
    uint32 StepsLastFrame() const { return m_StepsLastFrame; }
    float Interpolation() const { return m_Interpolation; }
    bool IsPrewarming() const { return m_Prewarm.Pending; }

    /// Position of particle `index` between the last two ticks, in simulation space.
    Mathematics::Vector3 InterpolatedPosition(uint32 index) const;

  private:
    friend class ParticleEventSink;
    struct PendingTransition
    {
        uint32 Index = 0;
        uint32 Destination = 0;
    };

    void ResetState(uint32 seed);
    void Configure();
    bool BeginFrame(const ParticleSimulationInput& input);
    void UpdateFrame(const ParticleSimulationInput& input);
    void SetFrameTransform(const float* transform);
    void SetFrameBetweenFrames(const float* current, double fraction);
    void RestartMotion(const ParticleSimulationInput& input);
    uint32 Limit(const ParticleSimulationInput& input) const;
    void TrimTo(uint32 count);
    double TickDuration(const ParticleSimulationInput& input) const;
    void Tick(const ParticleSimulationInput& input, float dt);

    void SnapshotPrevious();
    void BuildPhaseBatches();
    std::span<const uint32> PhaseBatch(uint32 phase) const;
    std::span<const uint32> ActiveSubset(const CompiledProcessor& processor, std::span<const uint32> batch);
    void RunProcessors(std::span<const CompiledProcessor> processors, std::span<const uint32> batch, float dt);
    void RunUpdate(ParticleUpdateOrder order, float dt);
    void Integrate(float dt);
    void ProcessExternalEvents();
    void ProcessMotionRules(float dt);
    void ProcessAgeRules();
    void ProcessDeaths();
    void Compact();
    void Spawn(const ParticleSimulationInput& input, float dt);
    uint32 EmissionCount(float dt);
    void SpawnBatch(uint32 count, const ParticleSpawnRequest* request);
    void SnapshotBasis(ParticleBasis basis, std::span<const uint32> particles);

    bool ApplyRules(uint32 index, ParticleEventTrigger trigger, const CollisionHit* hit = nullptr,
                    StringId externalName = 0);
    void EmitSpawnEvent(uint32 index, const CompiledEventRule& rule, ParticleEventTrigger trigger,
                        const CollisionHit* hit, const Mathematics::Vector3* position);
    void KillParticle(uint32 index, const CollisionHit* hit);
    void QueueTransition(uint32 index, uint32 destination);
    void ResolveTransitions();
    void EnterPhases(std::span<const PendingTransition> transitions);

    std::shared_ptr<const CompiledParticleStack> m_Stack;
    ParticleChannels m_Channels;
    ParticleScratch m_Scratch;
    ParticleTrailStore m_Trails;
    ParticleEmitterFrame m_Frame;
    ParticleEventSink m_Events;
    const ParticleCollisionWorld* m_Collision = nullptr;
    bool m_RecordEffects = true;
    uint32 m_Capacity = 0;

    // Per-particle scratch the runtime owns, sized to capacity at Bind: state flags within a tick,
    // phase-sorted indices, processor subsets, kill and transition lists.
    std::vector<uint8> m_Flags;
    std::vector<uint32> m_Identity;
    std::vector<uint32> m_PhaseOrder;
    std::vector<uint32> m_PhaseBegin;
    std::vector<uint32> m_Subset;
    std::vector<uint32> m_Group;
    std::vector<uint32> m_Kills;
    std::vector<PendingTransition> m_Pending;
    std::vector<PendingTransition> m_Resolving;

    std::vector<ParticleSpawnEvent> m_SpawnEvents;
    std::vector<ParticleSpawnRequest> m_Requests;
    std::vector<ParticleCollisionRecord> m_Collisions;
    std::vector<std::pair<uint32, StringId>> m_ExternalEvents;
    std::vector<std::pair<uint32, StringId>> m_ExternalEventsProcessing;
    std::vector<double> m_EmissionState;

    double m_Accumulator = 0.0;
    double m_Elapsed = 0.0;
    double m_EmissionAge = 0.0;
    double m_DroppedTime = 0.0;
    double m_SpawnAccumulator = 0.0;
    uint32 m_Seed = 1;
    uint32 m_SpawnCounter = 0;
    uint32 m_StepsLastFrame = 0;
    float m_Interpolation = 1.0f;
    float m_LastTickSeconds = 0.0f;
    bool m_Initialized = false;
    // The emitter's transform at the end of the previous frame, and its origin where the last tick
    // ended: the ticks a frame runs place the emitter between the two frames' transforms, and a
    // tick's emitted particles spread from the last tick's origin to its own.
    std::array<float, 16> m_PreviousTransform{};
    Mathematics::Vector3 m_TickOrigin{};
    PreviewSeek m_Prewarm;
};

/// View-space depth of a world position; this engine's views are left-handed, +Z forward.
float ViewDepth(const float view[16], const Mathematics::Vector3& position);

} // namespace GameEngine::Particles
