#include "Particles/ParticleRuntime.h"

#include "Mathematics/Quaternion.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>

namespace GameEngine::Particles
{
using Mathematics::Vector3;
using Mathematics::Vector4;

namespace
{
constexpr uint32 kMaxStepsPerFrame = 8;
constexpr uint32 kMaxSpawnEvents = 4096;
constexpr uint32 kMaxSpawnRequests = 4096;
constexpr uint32 kMaxExternalEvents = 256;
constexpr uint32 kMaxTransitionRounds = 8;
constexpr uint32 kMaxPhaseEntries = 1024;
constexpr uint32 kMaxRepeatsPerTick = 4096;
constexpr double kMaxSeekTime = 60.0;
constexpr double kMaxFrameTime = 3600.0;
constexpr double kMaxVariableFrame = 0.25;
constexpr double kVariableStep = 1.0 / 30.0;
constexpr uint32 kPrewarmTicksPerFrame = 64;
constexpr double kPrewarmBudgetSeconds = 0.002;
constexpr float kTimeEpsilon = 1e-6f;
constexpr float kMinimumLifetime = 0.001f;
constexpr uint32 kProbabilityStream = 5;
constexpr uint32 kEmissionStateSlots = 2;
constexpr float kSingularDeterminant = 1e-10f;
// Shortest axis a frame transform may scale to and still be split into rotation and scale.
constexpr float kSmallestScale = 1e-6f;
// A move of more than this many of the emitter's own scale units in one frame is a teleport: the emitter
// starts its path at the new place instead of laying the frame's particles along the jump.
constexpr float kTeleportDistance = 50.0f;

// A frame transform split into what interpolates on its own terms: where it is, how it is turned and
// how it is scaled. A mirrored transform carries its reflection as a negative X scale.
struct TransformParts
{
    glm::vec3 Translation{0.0f};
    glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 Scale{1.0f};
};

// Splits a column-major affine transform without shear. False when an axis has collapsed, which leaves
// no rotation to recover.
bool SplitTransform(const float* m, TransformParts& parts)
{
    glm::vec3 axes[3] = {{m[0], m[1], m[2]}, {m[4], m[5], m[6]}, {m[8], m[9], m[10]}};
    for (int axis = 0; axis < 3; ++axis)
    {
        parts.Scale[axis] = glm::length(axes[axis]);
        if (!(parts.Scale[axis] >= kSmallestScale))
            return false;
        axes[axis] /= parts.Scale[axis];
    }
    if (glm::dot(glm::cross(axes[0], axes[1]), axes[2]) < 0.0f)
    {
        parts.Scale.x = -parts.Scale.x;
        axes[0] = -axes[0];
    }
    parts.Rotation = glm::quat_cast(glm::mat3(axes[0], axes[1], axes[2]));
    parts.Translation = {m[12], m[13], m[14]};
    return true;
}

// The emitter's scale in world units per emitter unit: its longest axis.
float EmitterScale(const float* m)
{
    const float longest = std::max({glm::length(glm::vec3(m[0], m[1], m[2])), glm::length(glm::vec3(m[4], m[5], m[6])),
                                    glm::length(glm::vec3(m[8], m[9], m[10]))});
    return std::max(longest, kSmallestScale);
}
constexpr std::array<float, 16> kIdentityTransform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

// Column-major world transform of the emitter; a null transform is the identity.
const float* EmitterTransform(const ParticleSimulationInput& input)
{
    return input.Transform ? input.Transform : kIdentityTransform.data();
}

enum ParticleFlag : uint8
{
    kFlagKilled = 1u << 0,
    kFlagLeaving = 1u << 1,
    kFlagDying = 1u << 2,
};

// The attributes with birth and phase-entry snapshot channels (Alpha shares Color's).
constexpr ParticleAttribute kSnapshotAttributes[] = {
    ParticleAttribute::Position, ParticleAttribute::Velocity, ParticleAttribute::Size,
    ParticleAttribute::Scale, ParticleAttribute::Color, ParticleAttribute::Rotation,
    ParticleAttribute::Spin, ParticleAttribute::Lifetime, ParticleAttribute::AnimationSpeed,
    ParticleAttribute::AnimationOffset, ParticleAttribute::Custom0, ParticleAttribute::Custom1,
    ParticleAttribute::Custom2, ParticleAttribute::Custom3};

// Channels copied into their previous-tick counterparts at the start of every tick.
constexpr std::pair<ParticleChannel, ParticleChannel> kPreviousChannels[] = {
    {ParticleChannel::PreviousPosition, ParticleChannel::Position},
    {ParticleChannel::PreviousAge, ParticleChannel::Age},
    {ParticleChannel::PreviousSize, ParticleChannel::Size},
    {ParticleChannel::PreviousScale, ParticleChannel::Scale},
    {ParticleChannel::PreviousColor, ParticleChannel::Color},
    {ParticleChannel::PreviousRotation, ParticleChannel::Rotation},
};

bool IsFinite(const Vector3& vector)
{
    return std::isfinite(vector.x) && std::isfinite(vector.y) && std::isfinite(vector.z);
}
} // namespace

void ParticleEventSink::Kill(uint32 index, const CollisionHit* hit)
{
    m_Runtime.KillParticle(index, hit);
}

bool ParticleEventSink::RaiseCollision(uint32 index, const CollisionHit& hit, bool killing)
{
    auto& runtime = m_Runtime;
    if (runtime.m_RecordEffects && runtime.m_Collisions.size() < runtime.m_Capacity)
    {
        ParticleCollisionRecord record;
        record.ParticleId = runtime.m_Channels.SpawnIndices()[index];
        record.Hit = hit;
        record.Velocity = runtime.m_Frame.SimulationToWorldVector(runtime.m_Channels.Velocities()[index]);
        record.Killed = killing;
        runtime.m_Collisions.push_back(record);
    }
    return runtime.ApplyRules(index, ParticleEventTrigger::Collision, &hit);
}

ParticleRuntime::ParticleRuntime() : m_Events(*this)
{
}

void ParticleRuntime::Bind(std::shared_ptr<const CompiledParticleStack> stack, uint32 capacity)
{
    m_Stack = std::move(stack);
    m_Capacity = m_Stack ? std::min(capacity, kMaxParticlesPerEmitter) : 0u;
    Configure();
    m_Requests.clear();
    ResetState(m_Seed);
    m_Initialized = false;
}

bool ParticleRuntime::Rebind(std::shared_ptr<const CompiledParticleStack> stack)
{
    if (!m_Stack || !stack || stack->Channels != m_Stack->Channels || stack->Phases.size() != m_Stack->Phases.size() ||
        stack->Emission.size() != m_Stack->Emission.size() || stack->HasTrails != m_Stack->HasTrails ||
        (stack->HasTrails && stack->Trail.Points != m_Stack->Trail.Points))
        return false;
    m_Stack = std::move(stack);
    return true;
}

void ParticleRuntime::Configure()
{
    const uint32 capacity = m_Capacity;
    const size_t phases = m_Stack ? m_Stack->Phases.size() : 0;
    m_Channels.Configure(m_Stack ? m_Stack->Channels : 0, capacity);
    m_Scratch.Configure(capacity);
    if (m_Stack && m_Stack->HasTrails)
        m_Trails.Configure(2 * capacity, std::clamp(m_Stack->Trail.Points, 2u, kMaxTrailPoints));
    else
        m_Trails.Configure(0, 0);
    m_Flags.assign(capacity, 0);
    m_Identity.resize(capacity);
    std::iota(m_Identity.begin(), m_Identity.end(), 0u);
    m_PhaseOrder.assign(capacity, 0);
    m_PhaseBegin.assign(phases + 1, 0);
    m_Subset.assign(capacity, 0);
    m_Group.assign(capacity, 0);
    m_Kills.clear();
    m_Kills.reserve(capacity);
    m_Pending.clear();
    m_Pending.reserve(capacity);
    m_Resolving.clear();
    m_Resolving.reserve(capacity);
    m_SpawnEvents.reserve(kMaxSpawnEvents);
    m_Requests.reserve(kMaxSpawnRequests);
    m_Collisions.reserve(capacity);
    m_ExternalEvents.reserve(kMaxExternalEvents);
    m_ExternalEventsProcessing.reserve(kMaxExternalEvents);
    m_EmissionState.assign(m_Stack ? m_Stack->Emission.size() * kEmissionStateSlots : 0, 0.0);
}

void ParticleRuntime::ResetState(uint32 seed)
{
    m_Channels.Clear();
    m_Trails.Clear();
    std::fill(m_Flags.begin(), m_Flags.end(), uint8{0});
    m_Kills.clear();
    m_Pending.clear();
    m_SpawnEvents.clear();
    m_Collisions.clear();
    m_ExternalEvents.clear();
    std::fill(m_EmissionState.begin(), m_EmissionState.end(), 0.0);
    m_Accumulator = m_Elapsed = m_EmissionAge = m_DroppedTime = m_SpawnAccumulator = 0.0;
    m_Seed = seed;
    m_SpawnCounter = 0;
    m_StepsLastFrame = 0;
    m_Interpolation = 1.0f;
    m_Prewarm = {};
    m_Frame.Velocity = {};
}

void ParticleRuntime::Reset(uint32 seed)
{
    m_Requests.clear();
    ResetState(seed);
    m_Initialized = false;
}

uint32 ParticleRuntime::Limit(const ParticleSimulationInput& input) const
{
    return std::min({m_Capacity, input.Budget, kMaxParticlesPerEmitter});
}

double ParticleRuntime::TickDuration(const ParticleSimulationInput& input) const
{
    return 1.0 / static_cast<double>(input.FixedFps ? std::clamp(input.FixedFps, 1u, 240u) : 60u);
}

void ParticleRuntime::UpdateFrame(const ParticleSimulationInput& input)
{
    SetFrameTransform(EmitterTransform(input));
    m_Frame.LocalSpace = input.LocalSpace;
    m_Frame.Planar = input.Planar;
    m_Frame.Seed = m_Seed;
    m_Frame.Elapsed = m_Elapsed;
    m_Collision = input.Collision;
}

void ParticleRuntime::SetFrameTransform(const float* m)
{
    m_Frame.Axis[0] = {m[0], m[1], m[2]};
    m_Frame.Axis[1] = {m[4], m[5], m[6]};
    m_Frame.Axis[2] = {m[8], m[9], m[10]};
    m_Frame.Origin = {m[12], m[13], m[14]};
    // The inverse is taken once per frame here; processors never invert per particle.
    const glm::mat3 linear(m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]);
    m_Frame.Invertible = std::fabs(glm::determinant(linear)) >= kSingularDeterminant;
    const glm::mat3 inverse = m_Frame.Invertible ? glm::inverse(linear) : glm::mat3(1.0f);
    for (int column = 0; column < 3; ++column)
        m_Frame.InverseAxis[column] = {inverse[column][0], inverse[column][1], inverse[column][2]};
}

void ParticleRuntime::SetFrameBetweenFrames(const float* current, double fraction)
{
    const float t = static_cast<float>(std::clamp(fraction, 0.0, 1.0));
    std::array<float, 16> between{};
    TransformParts from;
    TransformParts to;
    if (!SplitTransform(m_PreviousTransform.data(), from) || !SplitTransform(current, to))
    {
        // A collapsed axis has no rotation to turn through: blend the matrices themselves.
        for (size_t element = 0; element < between.size(); ++element)
            between[element] = m_PreviousTransform[element] + (current[element] - m_PreviousTransform[element]) * t;
        SetFrameTransform(between.data());
        return;
    }
    // Position moves in a straight line, rotation turns at a constant rate along the shorter arc and
    // scale changes linearly, so an emitter spinning between two frames keeps its shape's size.
    const glm::quat rotation = Mathematics::Quaternion::Slerp(from.Rotation, to.Rotation, t).GetGLM();
    const glm::mat3 linear = glm::mat3_cast(rotation);
    const glm::vec3 scale = glm::mix(from.Scale, to.Scale, t);
    const glm::vec3 translation = glm::mix(from.Translation, to.Translation, t);
    for (int axis = 0; axis < 3; ++axis)
    {
        const glm::vec3 column = linear[axis] * scale[axis];
        between[axis * 4 + 0] = column.x;
        between[axis * 4 + 1] = column.y;
        between[axis * 4 + 2] = column.z;
    }
    between[12] = translation.x;
    between[13] = translation.y;
    between[14] = translation.z;
    between[15] = 1.0f;
    SetFrameTransform(between.data());
}

void ParticleRuntime::RestartMotion(const ParticleSimulationInput& input)
{
    std::copy_n(EmitterTransform(input), m_PreviousTransform.size(), m_PreviousTransform.begin());
    m_TickOrigin = m_Frame.Origin;
}

void ParticleRuntime::TrimTo(uint32 count)
{
    if (m_Channels.Count() <= count)
        return;
    if (m_Channels.Has(ParticleChannel::TrailSlot))
    {
        const auto slots = m_Channels.Get<uint32>(ParticleChannel::TrailSlot);
        for (uint32 index = count; index < m_Channels.Count(); ++index)
            m_Trails.ReleaseOwner(slots[index]);
    }
    m_Channels.Truncate(count);
}

bool ParticleRuntime::BeginFrame(const ParticleSimulationInput& input)
{
    if (!m_Stack)
        return false;
    if (!m_Initialized || m_Seed != input.Seed)
    {
        auto requests = std::move(m_Requests);
        ResetState(input.Seed);
        m_Requests = std::move(requests);
        UpdateFrame(input);
        RestartMotion(input);
        m_Initialized = true;
        if (std::isfinite(input.PrewarmSeconds) && input.PrewarmSeconds > 0.0)
        {
            m_Prewarm.Target = std::min(input.PrewarmSeconds, kMaxSeekTime);
            m_Prewarm.Tick = TickDuration(input);
            m_Prewarm.Total =
                static_cast<uint32>(std::floor((m_Prewarm.Target + m_Prewarm.Tick * 1e-6) / m_Prewarm.Tick));
            m_Prewarm.Pending = m_Prewarm.Total > 0;
        }
    }
    UpdateFrame(input);
    m_SpawnEvents.clear();
    m_Collisions.clear();
    m_StepsLastFrame = 0;
    const uint32 limit = Limit(input);
    if (limit == 0)
    {
        auto requests = std::move(m_Requests);
        ResetState(input.Seed);
        m_Requests = std::move(requests);
        return false;
    }
    TrimTo(limit);
    return true;
}

void ParticleRuntime::Advance(const ParticleSimulationInput& input, double deltaTime)
{
    if (!BeginFrame(input))
        return;
    if (m_Prewarm.Pending)
    {
        // Prewarm runs in bounded chunks over the first frames instead of stalling one frame.
        ContinueSeek(input, m_Prewarm, kPrewarmTicksPerFrame, kPrewarmBudgetSeconds);
        RestartMotion(input);
        return;
    }
    if (!std::isfinite(deltaTime) || deltaTime <= 0.0)
        return;
    const double speed = std::isfinite(input.SpeedScale) ? std::max(0.0, input.SpeedScale) : 1.0;
    if (speed == 0.0)
        return;
    const float* current = EmitterTransform(input);
    const Vector3 previousOrigin{m_PreviousTransform[12], m_PreviousTransform[13], m_PreviousTransform[14]};
    // A jump no emitter makes in one frame (a pooled effect placed again, an owner that respawned) is
    // a teleport: the frame's particles start at the new place with the emitter at rest.
    if ((m_Frame.Origin - previousOrigin).Length() > kTeleportDistance * EmitterScale(current))
        RestartMotion(input);
    const Vector3 startOrigin{m_PreviousTransform[12], m_PreviousTransform[13], m_PreviousTransform[14]};
    m_Frame.Velocity = (m_Frame.Origin - startOrigin) * static_cast<float>(1.0 / deltaTime);
    const double scaled = std::min(deltaTime * speed, kMaxFrameTime);
    // Each tick sees the emitter where it was when that tick ended, between the previous frame's
    // transform and this one's, so the ticks of one frame do not share where the frame ends and a
    // moving emitter leaves the same particles at every display rate.
    if (input.FixedFps == 0u)
    {
        const double accepted = std::min(scaled, kMaxVariableFrame);
        const auto steps = static_cast<uint32>(
            std::clamp(std::ceil(accepted / kVariableStep), 1.0, static_cast<double>(kMaxStepsPerFrame)));
        for (uint32 step = 0; step < steps; ++step)
        {
            SetFrameBetweenFrames(current, static_cast<double>(step + 1) / steps);
            Tick(input, static_cast<float>(accepted / steps));
        }
        SetFrameTransform(current);
        std::copy_n(current, m_PreviousTransform.size(), m_PreviousTransform.begin());
        m_StepsLastFrame = steps;
        m_DroppedTime += scaled - accepted;
        m_Accumulator = 0.0;
        m_Interpolation = 1.0f;
        return;
    }
    const double tick = TickDuration(input);
    const double carried = m_Accumulator;
    m_Accumulator += scaled;
    const double available = std::floor((m_Accumulator + tick * 1e-6) / tick);
    const auto steps = static_cast<uint32>(std::min(available, static_cast<double>(kMaxStepsPerFrame)));
    for (uint32 step = 0; step < steps; ++step)
    {
        SetFrameBetweenFrames(current, (static_cast<double>(step + 1) * tick - carried) / scaled);
        Tick(input, static_cast<float>(tick));
    }
    SetFrameTransform(current);
    std::copy_n(current, m_PreviousTransform.size(), m_PreviousTransform.begin());
    m_StepsLastFrame = steps;
    m_DroppedTime += std::max(0.0, available - steps) * tick;
    m_Accumulator = std::max(0.0, m_Accumulator - available * tick);
    m_Interpolation = input.Interpolate ? static_cast<float>(std::clamp(m_Accumulator / tick, 0.0, 1.0)) : 1.0f;
}

void ParticleRuntime::Step(const ParticleSimulationInput& input)
{
    if (!BeginFrame(input))
        return;
    m_Prewarm = {};
    Tick(input, static_cast<float>(TickDuration(input)));
    m_StepsLastFrame = 1;
    m_Accumulator = 0.0;
    m_Interpolation = 1.0f;
}

void ParticleRuntime::BeginSeek(const ParticleSimulationInput& input, double time, PreviewSeek& seek)
{
    seek = {};
    if (!m_Stack)
        return;
    auto requests = std::move(m_Requests);
    ResetState(input.Seed);
    m_Requests = std::move(requests);
    UpdateFrame(input);
    RestartMotion(input);
    m_Initialized = true;
    if (!std::isfinite(time) || time <= 0.0 || Limit(input) == 0)
        return;
    seek.Target = std::min(time, kMaxSeekTime);
    seek.Tick = TickDuration(input);
    seek.Total = static_cast<uint32>(std::floor((seek.Target + seek.Tick * 1e-6) / seek.Tick));
    seek.Pending = true;
}

void ParticleRuntime::ContinueSeek(const ParticleSimulationInput& input, PreviewSeek& seek, uint32 maxTicks,
                                   double budgetSeconds)
{
    m_StepsLastFrame = 0;
    if (!seek.Pending || !m_Stack)
        return;
    const uint32 limit = Limit(input);
    if (limit == 0)
    {
        ResetState(input.Seed);
        seek = {};
        return;
    }
    const auto begin = std::chrono::steady_clock::now();
    UpdateFrame(input);
    TrimTo(limit);
    // A seek shows where the effect would be; it raises no gameplay events and spawns no children.
    m_RecordEffects = false;
    while (seek.Completed < seek.Total && m_StepsLastFrame < maxTicks)
    {
        Tick(input, static_cast<float>(seek.Tick));
        ++seek.Completed;
        ++m_StepsLastFrame;
        m_SpawnEvents.clear();
        m_Collisions.clear();
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count() >= budgetSeconds)
            break;
    }
    m_RecordEffects = true;
    seek.Pending = seek.Completed < seek.Total;
    m_Accumulator = seek.Pending ? 0.0 : std::max(0.0, seek.Target - seek.Total * seek.Tick);
    m_Interpolation = seek.Pending || !input.Interpolate ? 1.0f : static_cast<float>(m_Accumulator / seek.Tick);
}

void ParticleRuntime::Seek(const ParticleSimulationInput& input, double time)
{
    PreviewSeek seek;
    BeginSeek(input, time, seek);
    ContinueSeek(input, seek, std::numeric_limits<uint32>::max(), std::numeric_limits<double>::infinity());
}

bool ParticleRuntime::SendEvent(StringId name, uint32 particleId)
{
    if (name == 0 || m_ExternalEvents.size() >= kMaxExternalEvents)
        return false;
    m_ExternalEvents.emplace_back(particleId, name);
    return true;
}

bool ParticleRuntime::QueueSpawn(const ParticleSpawnRequest& request)
{
    if (m_Requests.size() >= kMaxSpawnRequests)
        return false;
    m_Requests.push_back(request);
    return true;
}

Vector3 ParticleRuntime::InterpolatedPosition(uint32 index) const
{
    const auto current = m_Channels.Positions()[index];
    const auto previous = m_Channels.Get<Vector3>(ParticleChannel::PreviousPosition)[index];
    return previous + (current - previous) * m_Interpolation;
}

void ParticleRuntime::Tick(const ParticleSimulationInput& input, float dt)
{
    m_LastTickSeconds = dt;
    m_Frame.Elapsed = m_Elapsed;
    SnapshotPrevious();
    if (m_Stack->HasLights)
    {
        auto intensities = m_Channels.Get<float>(ParticleChannel::LightIntensity);
        auto ranges = m_Channels.Get<float>(ParticleChannel::LightRange);
        std::fill_n(intensities.begin(), m_Channels.Count(), 0.0f);
        std::fill_n(ranges.begin(), m_Channels.Count(), 0.0f);
    }
    ProcessExternalEvents();
    ResolveTransitions();
    BuildPhaseBatches();
    RunUpdate(ParticleUpdateOrder::BeforeIntegration, dt);
    Integrate(dt);
    RunUpdate(ParticleUpdateOrder::AfterIntegration, dt);
    ResolveTransitions();
    ProcessMotionRules(dt);
    ResolveTransitions();
    ProcessAgeRules();
    ResolveTransitions();
    ProcessDeaths();
    Compact();
    Spawn(input, dt);
    m_TickOrigin = m_Frame.Origin;
    m_Elapsed += dt;
    m_Trails.Expire(m_Elapsed);
}

void ParticleRuntime::SnapshotPrevious()
{
    const uint32 count = m_Channels.Count();
    for (const auto& [previous, current] : kPreviousChannels)
        m_Channels.CopyChannel(previous, current, 0, count);
}

void ParticleRuntime::BuildPhaseBatches()
{
    const size_t phases = m_Stack->Phases.size();
    if (phases <= 1)
        return;
    const uint32 count = m_Channels.Count();
    const auto phaseOf = m_Channels.Phases();
    std::fill(m_PhaseBegin.begin(), m_PhaseBegin.end(), 0u);
    for (uint32 index = 0; index < count; ++index)
        ++m_PhaseBegin[phaseOf[index] + 1];
    for (size_t phase = 1; phase <= phases; ++phase)
        m_PhaseBegin[phase] += m_PhaseBegin[phase - 1];
    // Counting sort into phase order; m_Group holds the running write position of each phase.
    std::copy_n(m_PhaseBegin.begin(), phases, m_Group.begin());
    for (uint32 index = 0; index < count; ++index)
        m_PhaseOrder[m_Group[phaseOf[index]]++] = index;
}

std::span<const uint32> ParticleRuntime::PhaseBatch(uint32 phase) const
{
    if (m_Stack->Phases.size() <= 1)
        return {m_Identity.data(), m_Channels.Count()};
    return {m_PhaseOrder.data() + m_PhaseBegin[phase], m_PhaseBegin[phase + 1] - m_PhaseBegin[phase]};
}

std::span<const uint32> ParticleRuntime::ActiveSubset(const CompiledProcessor& processor,
                                                      std::span<const uint32> batch)
{
    if (!processor.Bounded)
        return batch;
    const auto clock = processor.Clock == ParticleClock::Age ? m_Channels.Ages() : m_Channels.PhaseAges();
    uint32 count = 0;
    for (const uint32 index : batch)
    {
        const float age = clock[index];
        if (age + kTimeEpsilon >= processor.Start && age < processor.End - kTimeEpsilon)
            m_Subset[count++] = index;
    }
    return {m_Subset.data(), count};
}

void ParticleRuntime::RunProcessors(std::span<const CompiledProcessor> processors, std::span<const uint32> batch,
                                    float dt)
{
    if (batch.empty())
        return;
    for (const auto& processor : processors)
    {
        const auto particles = ActiveSubset(processor, batch);
        if (particles.empty())
            continue;
        ParticleProcessorContext context{.Channels = m_Channels,
                                         .Particles = particles,
                                         .Emitter = m_Frame,
                                         .DeltaTime = dt,
                                         .ProcessorId = processor.Id,
                                         .ProcessorOrdinal = processor.Ordinal,
                                         .Parameters = processor.Parameters,
                                         .Compiled = processor.Compiled.get(),
                                         .Scratch = m_Scratch,
                                         .Events = &m_Events,
                                         .Collision = m_Collision,
                                         .Trails = &m_Trails};
        m_Channels.SetReader(processor.Descriptor->Id);
        processor.Descriptor->Execute(context);
        m_Channels.SetReader({});
    }
}

void ParticleRuntime::RunUpdate(ParticleUpdateOrder order, float dt)
{
    for (uint32 phase = 0; phase < m_Stack->Phases.size(); ++phase)
    {
        const auto& compiled = m_Stack->Phases[phase];
        const auto& processors = order == ParticleUpdateOrder::BeforeIntegration ? compiled.UpdateBeforeIntegration
                                                                                 : compiled.UpdateAfterIntegration;
        if (!processors.empty())
            RunProcessors(processors, PhaseBatch(phase), dt);
    }
}

void ParticleRuntime::Integrate(float dt)
{
    const uint32 count = m_Channels.Count();
    auto positions = m_Channels.Positions();
    auto velocities = m_Channels.Velocities();
    auto rotations = m_Channels.Rotations();
    const auto spins = m_Channels.Spins();
    auto ages = m_Channels.Ages();
    auto phaseAges = m_Channels.PhaseAges();
    const bool settles = m_Channels.Has(ParticleChannel::Settled);
    const auto settled = settles ? m_Channels.Get<uint32>(ParticleChannel::Settled) : std::span<const uint32>{};
    const auto previous = m_Channels.Get<Vector3>(ParticleChannel::PreviousPosition);
    for (uint32 index = 0; index < count; ++index)
    {
        if (settles && settled[index])
        {
            // A settled particle keeps changing appearance, but nothing moves it again.
            positions[index] = previous[index];
            velocities[index] = {};
        }
        else
        {
            if (m_Frame.Planar)
                velocities[index].z = 0.0f;
            positions[index] = positions[index] + velocities[index] * dt;
            rotations[index] += spins[index] * dt;
        }
        ages[index] += dt;
        phaseAges[index] += dt;
    }
}

void ParticleRuntime::ProcessExternalEvents()
{
    if (m_ExternalEvents.empty())
        return;
    m_ExternalEventsProcessing.clear();
    std::swap(m_ExternalEvents, m_ExternalEventsProcessing);
    const auto spawns = m_Channels.SpawnIndices();
    for (const auto& [particle, name] : m_ExternalEventsProcessing)
        for (uint32 index = 0; index < m_Channels.Count(); ++index)
            if (particle == kAllParticles || spawns[index] == particle)
                ApplyRules(index, ParticleEventTrigger::External, nullptr, name);
    m_ExternalEventsProcessing.clear();
}

void ParticleRuntime::ProcessMotionRules(float dt)
{
    bool anyRules = false;
    for (const auto& phase : m_Stack->Phases)
        anyRules |= !phase.RulesFor(ParticleEventTrigger::Distance).empty() ||
                    !phase.RulesFor(ParticleEventTrigger::Interval).empty();
    if (!anyRules)
        return;
    const uint32 count = m_Channels.Count();
    const auto phaseOf = m_Channels.Phases();
    const auto positions = m_Channels.Positions();
    const auto previous = m_Channels.Get<Vector3>(ParticleChannel::PreviousPosition);
    const auto phaseAges = m_Channels.PhaseAges();
    const auto spawns = m_Channels.SpawnIndices();
    const auto entries = m_Channels.Get<uint32>(ParticleChannel::PhaseEntries);
    const bool tracksDistance = m_Channels.Has(ParticleChannel::Distance);
    auto distances = tracksDistance ? m_Channels.Get<double>(ParticleChannel::Distance) : std::span<double>{};
    auto previousDistances =
        tracksDistance ? m_Channels.Get<double>(ParticleChannel::PreviousDistance) : std::span<double>{};
    for (uint32 index = 0; index < count; ++index)
    {
        if (m_Flags[index] != 0)
            continue;
        const auto& phase = m_Stack->Phases[phaseOf[index]];
        const auto& distanceRules = phase.RulesFor(ParticleEventTrigger::Distance);
        const auto& intervalRules = phase.RulesFor(ParticleEventTrigger::Interval);
        if (distanceRules.empty() && intervalRules.empty())
            continue;
        const Vector3 start = m_Frame.SimulationToWorldPoint(previous[index]);
        const Vector3 end = m_Frame.SimulationToWorldPoint(positions[index]);
        if (!distanceRules.empty())
        {
            previousDistances[index] = distances[index];
            distances[index] += (end - start).Length();
            const double travelled = distances[index] - previousDistances[index];
            for (const auto& rule : distanceRules)
            {
                const auto& parameters = *rule.Parameters;
                if (ParticleRandom(m_Seed ^ entries[index], spawns[index], rule.ProcessorId, kProbabilityStream) >=
                    parameters.Probability)
                    continue;
                // Events land at every multiple of the spacing along this tick's segment.
                const double first = std::floor(previousDistances[index] / parameters.Spacing);
                const auto repeats = static_cast<uint32>(std::clamp(std::floor(distances[index] / parameters.Spacing) - first,
                                                                    0.0, static_cast<double>(kMaxRepeatsPerTick)));
                for (uint32 repeat = 0; repeat < repeats; ++repeat)
                {
                    const auto fraction = static_cast<float>(
                        ((first + repeat + 1) * parameters.Spacing - previousDistances[index]) / std::max(1e-12, travelled));
                    const Vector3 at = start + (end - start) * fraction;
                    EmitSpawnEvent(index, rule, ParticleEventTrigger::Distance, nullptr, &at);
                }
            }
        }
        for (const auto& rule : intervalRules)
        {
            const auto& parameters = *rule.Parameters;
            if (ParticleRandom(m_Seed ^ entries[index], spawns[index], rule.ProcessorId, kProbabilityStream) >=
                parameters.Probability)
                continue;
            const float age = phaseAges[index];
            const float before = std::max(0.0f, age - dt);
            const auto repeats = static_cast<uint32>(std::clamp(std::floor(age / parameters.Seconds) - std::floor(before / parameters.Seconds),
                                                                0.0f, static_cast<float>(kMaxRepeatsPerTick)));
            for (uint32 repeat = 0; repeat < repeats; ++repeat)
                EmitSpawnEvent(index, rule, ParticleEventTrigger::Interval, nullptr, &end);
        }
    }
}

void ParticleRuntime::ProcessAgeRules()
{
    const uint32 count = m_Channels.Count();
    const auto phaseOf = m_Channels.Phases();
    for (uint32 index = 0; index < count; ++index)
    {
        const auto& phase = m_Stack->Phases[phaseOf[index]];
        if (!phase.RulesFor(ParticleEventTrigger::Age).empty() && !ApplyRules(index, ParticleEventTrigger::Age))
            continue;
        if (!phase.RulesFor(ParticleEventTrigger::PhaseAge).empty())
            ApplyRules(index, ParticleEventTrigger::PhaseAge);
    }
}

void ParticleRuntime::ProcessDeaths()
{
    const uint32 count = m_Channels.Count();
    const auto ages = m_Channels.Ages();
    const auto lifetimes = m_Channels.Lifetimes();
    const auto positions = m_Channels.Positions();
    const auto sizes = m_Channels.Sizes();
    for (uint32 index = 0; index < count; ++index)
    {
        if ((m_Flags[index] & kFlagKilled) != 0)
            continue;
        if (!IsFinite(positions[index]) || !std::isfinite(sizes[index]))
        {
            // A diverged particle is removed without running its rules at a meaningless position.
            m_Flags[index] |= kFlagKilled;
            m_Kills.push_back(index);
        }
        else if (ages[index] >= lifetimes[index] - kTimeEpsilon)
            KillParticle(index, nullptr);
    }
}

void ParticleRuntime::Compact()
{
    if (m_Kills.empty())
        return;
    // Flags belong to slots, not particles: clear every slot this pass used, including the tail slots
    // the removals vacate, or a particle later spawned into a vacated slot inherits its predecessor's
    // Killed flag and never dies.
    const uint32 previousCount = m_Channels.Count();
    std::sort(m_Kills.begin(), m_Kills.end(), std::greater<>());
    m_Kills.erase(std::unique(m_Kills.begin(), m_Kills.end()), m_Kills.end());
    const bool trails = m_Channels.Has(ParticleChannel::TrailSlot);
    // Descending order: the particle moved into a removed slot always comes from above it and is
    // never one still to be removed.
    for (const uint32 index : m_Kills)
    {
        if (trails)
            m_Trails.ReleaseOwner(m_Channels.Get<uint32>(ParticleChannel::TrailSlot)[index]);
        m_Channels.RemoveSwapBack(index);
    }
    m_Kills.clear();
    std::fill_n(m_Flags.begin(), previousCount, uint8{0});
}

uint32 ParticleRuntime::EmissionCount(float dt)
{
    const double begin = m_EmissionAge;
    const double end = m_EmissionAge + dt;
    for (size_t index = 0; index < m_Stack->Emission.size(); ++index)
    {
        const auto& processor = m_Stack->Emission[index];
        double windowBegin = begin;
        double windowEnd = end;
        if (processor.Bounded)
        {
            windowBegin = std::max(windowBegin, static_cast<double>(processor.Start));
            windowEnd = std::min(windowEnd, static_cast<double>(processor.End));
        }
        if (windowEnd <= windowBegin)
            continue;
        ParticleEmissionContext context;
        context.Parameters = processor.Parameters;
        context.Begin = windowBegin;
        context.Duration = windowEnd - windowBegin;
        context.State = m_EmissionState.data() + index * kEmissionStateSlots;
        context.Seed = m_Seed;
        context.ProcessorId = processor.Id;
        context.EmitterSpeed = m_Frame.Velocity.Length();
        m_SpawnAccumulator += processor.Descriptor->Emit(context);
    }
    const auto count = static_cast<uint32>(
        std::min(static_cast<double>(kMaxParticlesPerEmitter), std::floor(m_SpawnAccumulator + 1e-6)));
    m_SpawnAccumulator = std::clamp(m_SpawnAccumulator - count, 0.0, static_cast<double>(kMaxParticlesPerEmitter));
    return count;
}

void ParticleRuntime::Spawn(const ParticleSimulationInput& input, float dt)
{
    const uint32 limit = Limit(input);
    for (const auto& request : m_Requests)
    {
        const uint32 room = limit - std::min(limit, m_Channels.Count());
        const uint32 count = std::min(request.Count, room);
        if (count > 0)
            SpawnBatch(count, &request);
    }
    m_Requests.clear();
    if (!input.Emitting)
    {
        // Emission restarts from its beginning when the emitter is switched back on.
        m_EmissionAge = 0.0;
        m_SpawnAccumulator = 0.0;
        std::fill(m_EmissionState.begin(), m_EmissionState.end(), 0.0);
        return;
    }
    const uint32 wanted = EmissionCount(dt);
    m_EmissionAge += dt;
    const uint32 room = limit - std::min(limit, m_Channels.Count());
    const uint32 count = std::min(wanted, room);
    if (count > 0)
        SpawnBatch(count, nullptr);
}

void ParticleRuntime::SnapshotBasis(ParticleBasis basis, std::span<const uint32> particles)
{
    for (const auto attribute : kSnapshotAttributes)
    {
        const auto snapshot = AttributeChannel(attribute, basis);
        if (m_Channels.Has(snapshot))
            m_Channels.CopyChannel(snapshot, AttributeChannel(attribute, ParticleBasis::Current), particles);
    }
}

void ParticleRuntime::SpawnBatch(uint32 count, const ParticleSpawnRequest* request)
{
    const uint32 first = m_Channels.Append(count);
    const std::span<const uint32> batch(m_Identity.data() + first, count);
    auto spawns = m_Channels.SpawnIndices();
    auto phases = m_Channels.Phases();
    auto lifetimes = m_Channels.Lifetimes();
    auto positions = m_Channels.Positions();
    auto velocities = m_Channels.Velocities();
    auto sizes = m_Channels.Sizes();
    auto scales = m_Channels.Scales();
    auto colors = m_Channels.Colors();
    auto rotations = m_Channels.Rotations();
    auto animationSpeeds = m_Channels.Get<float>(ParticleChannel::AnimationSpeed);
    const Vector3 origin = request ? m_Frame.WorldToSimulationPoint(request->Position)
                                   : (m_Frame.LocalSpace ? Vector3{} : m_Frame.Origin);
    const Vector3 velocity = request ? m_Frame.WorldToSimulationVector(request->Velocity) : Vector3{};
    // An emitted batch spreads over the stretch the emitter covered during this tick, the newest
    // particle at the emitter, so a moving emitter lays an even trail instead of a clump per tick.
    const bool spread = request == nullptr && !m_Frame.LocalSpace;
    const Vector3 path = spread ? origin - m_TickOrigin : Vector3{};
    for (uint32 slot = 0; slot < count; ++slot)
    {
        const uint32 index = batch[slot];
        spawns[index] = m_SpawnCounter++;
        phases[index] = m_Stack->EntryPhase;
        lifetimes[index] = m_Stack->Lifetime;
        positions[index] = origin - path * (static_cast<float>(count - 1u - slot) / static_cast<float>(count));
        velocities[index] = velocity;
        sizes[index] = 1.0f;
        scales[index] = {1.0f, 1.0f, 1.0f};
        colors[index] = {1.0f, 1.0f, 1.0f, 1.0f};
        animationSpeeds[index] = 1.0f;
    }
    const auto& entry = m_Stack->Phases[m_Stack->EntryPhase];
    SnapshotBasis(ParticleBasis::Birth, batch);
    SnapshotBasis(ParticleBasis::Entry, batch);
    RunProcessors(entry.Birth, batch, 0.0f);
    // The child initializes first; what it inherits becomes its birth state, so lifetime
    // processors with a birth basis keep the parent's appearance instead of the child's defaults.
    if (request)
        for (const uint32 index : batch)
        {
            if (request->Inherit & kParticleInheritSize)
                sizes[index] = request->Size;
            if (request->Inherit & kParticleInheritColor)
                colors[index] = request->Color;
            if (request->Inherit & kParticleInheritRotation)
                rotations[index] = request->Rotation;
            if (request->Inherit & kParticleInheritLifetime)
                lifetimes[index] = request->Lifetime;
            if (request->AlignToNormal)
            {
                const Vector3 normal = m_Frame.WorldToSimulationVector(request->Normal);
                const float length = normal.Length();
                if (length > 1e-6f)
                    velocities[index] = normal * (velocities[index].Length() / length);
            }
        }
    SnapshotBasis(ParticleBasis::Birth, batch);
    SnapshotBasis(ParticleBasis::Entry, batch);
    RunProcessors(entry.Enter, batch, 0.0f);
    SnapshotBasis(ParticleBasis::Entry, batch);
    // A newborn is drawn before the tick that would first update it. The update processors that
    // run before integration take their age-0 value now (a fade-in curve starts at its first key),
    // so the first frame does not show the birth value those processors have yet to scale. Force
    // processors scale by the tick length, which is 0 here. A processor that works on the current
    // value compounds once here and once on every tick after.
    RunProcessors(entry.UpdateBeforeIntegration, batch, 0.0f);
    for (const auto& [previous, current] : kPreviousChannels)
        m_Channels.CopyChannel(previous, current, first, count);
    for (const uint32 index : batch)
    {
        if (lifetimes[index] <= 0.0f)
        {
            // A particle that could not be placed (an empty mesh shape) is never born.
            m_Flags[index] |= kFlagKilled;
            m_Kills.push_back(index);
            continue;
        }
        lifetimes[index] = std::max(lifetimes[index], kMinimumLifetime);
        if (ApplyRules(index, ParticleEventTrigger::Birth))
            ApplyRules(index, ParticleEventTrigger::Enter);
    }
    ResolveTransitions();
    Compact();
}

bool ParticleRuntime::ApplyRules(uint32 index, ParticleEventTrigger trigger, const CollisionHit* hit,
                                 StringId externalName)
{
    const uint8 flags = m_Flags[index];
    if ((flags & kFlagKilled) != 0)
        return false;
    if ((flags & kFlagDying) != 0 && trigger != ParticleEventTrigger::Death)
        return false;
    if ((flags & kFlagLeaving) != 0 && trigger != ParticleEventTrigger::Exit)
        return false;
    const auto& phase = m_Stack->Phases[m_Channels.Phases()[index]];
    const auto& rules = phase.RulesFor(trigger);
    if (rules.empty())
        return true;
    const auto spawn = m_Channels.SpawnIndices()[index];
    const auto entries = m_Channels.Get<uint32>(ParticleChannel::PhaseEntries)[index];
    auto& fired = m_Channels.Get<uint64>(ParticleChannel::FiredEvents)[index];
    const bool once = trigger == ParticleEventTrigger::Age || trigger == ParticleEventTrigger::PhaseAge;
    const float clock =
        trigger == ParticleEventTrigger::Age ? m_Channels.Ages()[index] : m_Channels.PhaseAges()[index];
    for (const auto& rule : rules)
    {
        const auto& parameters = *rule.Parameters;
        if (trigger == ParticleEventTrigger::External && rule.EventName != externalName)
            continue;
        if (once)
        {
            const uint64 bit = uint64{1} << rule.Bit;
            if ((fired & bit) != 0 || clock + kTimeEpsilon < parameters.Seconds)
                continue;
            fired |= bit;
        }
        if (ParticleRandom(m_Seed ^ entries, spawn, rule.ProcessorId, kProbabilityStream) >= parameters.Probability)
            continue;
        switch (parameters.Action)
        {
        case ParticleEventAction::Kill:
            if (trigger != ParticleEventTrigger::Death)
                KillParticle(index, hit);
            return false;
        case ParticleEventAction::Emit:
            EmitSpawnEvent(index, rule, trigger, hit, nullptr);
            if (parameters.KillSource)
            {
                if (trigger != ParticleEventTrigger::Death)
                    KillParticle(index, hit);
                return false;
            }
            break;
        case ParticleEventAction::Transition:
            // The rules of the phase the particle leaves do not continue after a transition.
            QueueTransition(index, rule.DestinationPhase);
            return false;
        }
    }
    return true;
}

void ParticleRuntime::EmitSpawnEvent(uint32 index, const CompiledEventRule& rule, ParticleEventTrigger trigger,
                                     const CollisionHit* hit, const Vector3* position)
{
    if (!m_RecordEffects || m_SpawnEvents.size() >= kMaxSpawnEvents)
        return;
    const auto& parameters = *rule.Parameters;
    ParticleSpawnEvent event;
    event.Trigger = trigger;
    event.ParticleId = m_Channels.SpawnIndices()[index];
    event.Count = parameters.Count;
    event.SubEmitter = parameters.SubEmitter;
    event.Position = position ? *position
                     : hit    ? hit->Position
                              : m_Frame.SimulationToWorldPoint(m_Channels.Positions()[index]);
    if (hit)
        event.Normal = hit->Normal;
    if (parameters.Inherit & kParticleInheritVelocity)
        event.Velocity = m_Frame.SimulationToWorldVector(m_Channels.Velocities()[index]) * parameters.VelocityScale;
    event.Size = m_Channels.Sizes()[index];
    event.Color = m_Channels.Colors()[index];
    event.Rotation = m_Channels.Rotations()[index];
    event.Lifetime = m_Channels.Lifetimes()[index];
    event.Inherit = parameters.Inherit;
    event.AlignToNormal = parameters.AlignToNormal;
    m_SpawnEvents.push_back(event);
}

void ParticleRuntime::KillParticle(uint32 index, const CollisionHit* hit)
{
    if ((m_Flags[index] & (kFlagKilled | kFlagDying)) != 0)
        return;
    m_Flags[index] |= kFlagDying;
    ApplyRules(index, ParticleEventTrigger::Death, hit);
    m_Flags[index] |= kFlagKilled;
    m_Kills.push_back(index);
}

void ParticleRuntime::QueueTransition(uint32 index, uint32 destination)
{
    // A dying particle never transitions: its index is compacted away before the queue resolves.
    if ((m_Flags[index] & (kFlagKilled | kFlagLeaving | kFlagDying)) != 0)
        return;
    m_Flags[index] |= kFlagLeaving;
    m_Pending.push_back({index, destination});
}

void ParticleRuntime::ResolveTransitions()
{
    const auto phaseOf = m_Channels.Phases();
    for (uint32 round = 0; round < kMaxTransitionRounds && !m_Pending.empty(); ++round)
    {
        m_Resolving.clear();
        std::swap(m_Pending, m_Resolving);
        std::sort(m_Resolving.begin(), m_Resolving.end(),
                  [&phaseOf](const PendingTransition& a, const PendingTransition& b)
                  { return phaseOf[a.Index] != phaseOf[b.Index] ? phaseOf[a.Index] < phaseOf[b.Index] : a.Index < b.Index; });
        // Exit processors and rules of each source phase, over the particles leaving it.
        for (size_t begin = 0; begin < m_Resolving.size();)
        {
            const uint32 source = phaseOf[m_Resolving[begin].Index];
            size_t end = begin;
            uint32 group = 0;
            while (end < m_Resolving.size() && phaseOf[m_Resolving[end].Index] == source)
                m_Group[group++] = m_Resolving[end++].Index;
            RunProcessors(m_Stack->Phases[source].Exit, {m_Group.data(), group}, 0.0f);
            for (size_t item = begin; item < end; ++item)
                ApplyRules(m_Resolving[item].Index, ParticleEventTrigger::Exit);
            begin = end;
        }
        EnterPhases(m_Resolving);
    }
    // Transitions still pending after the round limit form a cycle; the particles are removed.
    for (const auto& transition : m_Pending)
    {
        m_Flags[transition.Index] = static_cast<uint8>((m_Flags[transition.Index] & ~kFlagLeaving) | kFlagKilled);
        m_Kills.push_back(transition.Index);
    }
    m_Pending.clear();
}

void ParticleRuntime::EnterPhases(std::span<const PendingTransition> transitions)
{
    auto phaseOf = m_Channels.Phases();
    auto phaseAges = m_Channels.PhaseAges();
    auto fired = m_Channels.Get<uint64>(ParticleChannel::FiredEvents);
    auto entries = m_Channels.Get<uint32>(ParticleChannel::PhaseEntries);
    const bool contacts = m_Channels.Has(ParticleChannel::Contacts);
    const bool settles = m_Channels.Has(ParticleChannel::Settled);
    const bool distance = m_Channels.Has(ParticleChannel::Distance);
    uint32 entered = 0;
    for (const auto& transition : transitions)
    {
        const uint32 index = transition.Index;
        if ((m_Flags[index] & kFlagKilled) != 0)
            continue;
        m_Flags[index] &= static_cast<uint8>(~kFlagLeaving);
        if (entries[index] >= kMaxPhaseEntries)
        {
            m_Flags[index] |= kFlagKilled;
            m_Kills.push_back(index);
            continue;
        }
        phaseOf[index] = transition.Destination;
        phaseAges[index] = 0.0f;
        fired[index] = 0;
        ++entries[index];
        if (contacts)
            m_Channels.Get<uint64>(ParticleChannel::Contacts)[index] = 0;
        if (settles)
            m_Channels.Get<uint32>(ParticleChannel::Settled)[index] = 0;
        if (distance)
        {
            m_Channels.Get<double>(ParticleChannel::Distance)[index] = 0.0;
            m_Channels.Get<double>(ParticleChannel::PreviousDistance)[index] = 0.0;
        }
        m_Group[entered++] = index;
    }
    // Enter processors of each destination phase, over the particles arriving in it.
    std::sort(m_Group.begin(), m_Group.begin() + entered,
              [&phaseOf](uint32 a, uint32 b)
              { return phaseOf[a] != phaseOf[b] ? phaseOf[a] < phaseOf[b] : a < b; });
    for (uint32 begin = 0; begin < entered;)
    {
        const uint32 destination = phaseOf[m_Group[begin]];
        uint32 end = begin;
        while (end < entered && phaseOf[m_Group[end]] == destination)
            ++end;
        const std::span<const uint32> group(m_Group.data() + begin, end - begin);
        SnapshotBasis(ParticleBasis::Entry, group);
        RunProcessors(m_Stack->Phases[destination].Enter, group, 0.0f);
        SnapshotBasis(ParticleBasis::Entry, group);
        begin = end;
    }
    for (uint32 item = 0; item < entered; ++item)
        ApplyRules(m_Group[item], ParticleEventTrigger::Enter);
}

float ViewDepth(const float view[16], const Vector3& position)
{
    return view[2] * position.x + view[6] * position.y + view[10] * position.z + view[14];
}

} // namespace GameEngine::Particles
