// The particle simulation and the stack formats, without an engine: the runtime over compiled stacks
// built in code or read from their JSON and cooked forms.

#include "Components/Rendering/ParticleRenderer.h"
#include "Components/Rendering/Particles.h"
#include "ECS/ModuleRegistration.h"
#include "ECS/Reflection.h"
#include "Particles/CompiledParticleStack.h"
#include "Particles/ParticleFlipbook.h"
#include "Particles/ParticleProcessorRegistry.h"
#include "Particles/ParticleRuntime.h"
#include "Particles/ParticleStackAuthoring.h"
#include "Particles/ParticleStackBinary.h"
#include "Particles/ParticleStackDocument.h"
#include "Particles/Processors/ParticleAccelerationProcessor.h"
#include "Particles/Processors/ParticleCollisionProcessor.h"
#include "Particles/Processors/ParticleDragProcessor.h"
#include "Particles/Processors/ParticleEmitBurstProcessor.h"
#include "Particles/Processors/ParticleEmitRateProcessor.h"
#include "Particles/Processors/ParticleEventProcessor.h"
#include "Particles/Processors/ParticleLightProcessor.h"
#include "Particles/Processors/ParticleLimitSpeedProcessor.h"
#include "Particles/Processors/ParticleNoiseProcessor.h"
#include "Particles/Processors/ParticleOrbitProcessor.h"
#include "Particles/Processors/ParticlePropertyProcessor.h"
#include "Particles/Processors/ParticleShapeProcessor.h"
#include "Particles/Processors/ParticleTrailProcessor.h"
#include "Particles/Processors/ParticleVelocityConeProcessor.h"
#include "Types/StringId.h"
#include "Memory/AllocationCountScope.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <vector>

namespace ParticleRuntimeTestTypes
{
/// Parameters of a processor type the particle module does not define.
struct SpinParameters
{
    float Rate = 1.0f;
};

/// The spin parameters with a field added: a different layout under the same processor id.
struct GustySpinParameters
{
    float Rate = 1.0f;
    float Gust = 0.0f;
};
} // namespace ParticleRuntimeTestTypes

GE_REFLECT(ParticleRuntimeTestTypes::SpinParameters, Rate);
GE_REFLECT(ParticleRuntimeTestTypes::GustySpinParameters, Rate, Gust);

using namespace GameEngine;
using namespace GameEngine::Particles;
using Mathematics::Vector3;
using Mathematics::Vector4;

namespace
{
constexpr double kFrame = 1.0 / 60.0;
constexpr uint32 kSeed = 42;

constexpr ParticleParameterField kSpinFields[] = {
    {.Name = "Rate", .Label = "Spin Rate", .Tooltip = "Radians per second", .Kind = ParticleParameterKind::Float, .Minimum = 0.0f},
};

constexpr ParticleParameterField kGustySpinFields[] = {
    {.Name = "Rate", .Label = "Spin Rate", .Kind = ParticleParameterKind::Float},
    {.Name = "Gust", .Label = "Gust", .Kind = ParticleParameterKind::Float},
};

void ExecuteSpin(ParticleProcessorContext& context)
{
    const auto& parameters = context.Params<ParticleRuntimeTestTypes::SpinParameters>();
    auto spins = context.Channels.Spins();
    for (const uint32 index : context.Particles)
        spins[index] = parameters.Rate;
}

const ParticleProcessorDescriptor& RegisterSpinProcessor()
{
    static const ParticleProcessorDescriptor* registered = []
    {
        ParticleProcessorDescriptor descriptor;
        descriptor.Id = "test.spin";
        descriptor.DisplayName = "Test Spin";
        descriptor.Category = "Tests";
        descriptor.Stages = StageBit(ParticleStage::Birth) | StageBit(ParticleStage::Update);
        descriptor.DefaultStage = ParticleStage::Update;
        descriptor.Writes = ChannelBits(ParticleChannel::Spin);
        descriptor.Execute = ExecuteSpin;
        BindParticleParameters<ParticleRuntimeTestTypes::SpinParameters>(descriptor, kSpinFields);
        ParticleProcessorRegistry::Register(descriptor);
        return ParticleProcessorRegistry::Find("test.spin");
    }();
    return *registered;
}

std::string Describe(const std::vector<StackDiagnostic>& diagnostics)
{
    std::string text;
    for (const auto& diagnostic : diagnostics)
        text += diagnostic.Path + ": " + diagnostic.Message + "\n";
    return text;
}

template <typename T>
ParticleProcessorInstance Processor(const ParticleProcessorDescriptor& descriptor, const T& parameters,
                                    std::optional<ParticleStage> stage = {})
{
    auto processor = MakeProcessorInstance(descriptor);
    processor.Params<T>() = parameters;
    if (stage)
        processor.Stage = *stage;
    return processor;
}

uint32 Append(StackDocument& document, ParticleProcessorInstance processor, uint32 phase = 1)
{
    uint32 id = 0;
    std::string error;
    EXPECT_TRUE(AddProcessor(document, phase, std::move(processor), id, error)) << error;
    return id;
}

StackDocument EmptyStack(float lifetime)
{
    StackDocument document;
    document.Lifetime = lifetime;
    document.Phases.emplace_back();
    return document;
}

ParticlePropertyParameters SetVector(ParticleAttribute target, const Vector3& value)
{
    ParticlePropertyParameters parameters{.Target = target, .Operation = ParticleOperation::Set};
    for (uint32 component = 0; component < 3; ++component)
        parameters.Value[component] = value[component];
    return parameters;
}

ParticlePropertyParameters Scalar(ParticleAttribute target, ParticleOperation operation, ParticleBasis basis,
                                  ParticleValue value)
{
    ParticlePropertyParameters parameters{.Target = target, .Operation = operation, .Basis = basis};
    parameters.Value[0] = value;
    return parameters;
}

// Continuous emission from a sphere with a random upward velocity, gravity, drag and noise: the
// processors most effects combine.
StackDocument Fountain()
{
    auto document = EmptyStack(2.0f);
    Append(document, Processor(ParticleEmitRateProcessor(), ParticleEmitRateParameters{.Rate = 90.0f}));
    Append(document, Processor(ParticleShapeProcessor(), ParticleShapeParameters{.Radius = 0.5f}));
    ParticlePropertyParameters velocity{.Target = ParticleAttribute::Velocity, .Operation = ParticleOperation::Set};
    velocity.Value[0] = {-1.0f, 1.0f};
    velocity.Value[1] = {4.0f, 6.0f};
    velocity.Value[2] = {-1.0f, 1.0f};
    Append(document, Processor(ParticlePropertyProcessor(), velocity, ParticleStage::Birth));
    Append(document, Processor(ParticleAccelerationProcessor(), ParticleAccelerationParameters{}));
    Append(document, Processor(ParticleDragProcessor(), ParticleDragParameters{.Drag = 0.3f}));
    Append(document, Processor(ParticleNoiseProcessor(), ParticleNoiseParameters{.Strength = 0.5f}));
    return document;
}

std::shared_ptr<const CompiledParticleStack> Compile(const StackDocument& document)
{
    std::vector<StackDiagnostic> diagnostics;
    auto compiled = CompileParticleStack(std::make_shared<const StackDocument>(document), diagnostics);
    EXPECT_NE(compiled, nullptr) << Describe(diagnostics);
    return compiled;
}

ParticleSimulationInput Input()
{
    ParticleSimulationInput input;
    input.Seed = kSeed;
    input.FixedFps = 60;
    return input;
}

void AdvanceFrames(ParticleRuntime& runtime, const ParticleSimulationInput& input, int frames)
{
    for (int frame = 0; frame < frames; ++frame)
        runtime.Advance(input, kFrame);
}

struct ParticleState
{
    uint32 Spawn = 0;
    float Age = 0.0f;
    Vector3 Position{};
    Vector3 Velocity{};
    float Size = 0.0f;
};

std::vector<ParticleState> Snapshot(const ParticleRuntime& runtime)
{
    const auto& channels = runtime.Channels();
    std::vector<ParticleState> particles(runtime.Count());
    for (uint32 index = 0; index < runtime.Count(); ++index)
        particles[index] = {channels.SpawnIndices()[index], channels.Ages()[index], channels.Positions()[index],
                            channels.Velocities()[index], channels.Sizes()[index]};
    std::sort(particles.begin(), particles.end(),
              [](const ParticleState& a, const ParticleState& b)
              { return a.Spawn < b.Spawn; });
    return particles;
}

void ExpectSameParticles(const std::vector<ParticleState>& actual, const std::vector<ParticleState>& expected)
{
    ASSERT_EQ(actual.size(), expected.size());
    for (size_t index = 0; index < actual.size(); ++index)
    {
        EXPECT_EQ(actual[index].Spawn, expected[index].Spawn);
        EXPECT_FLOAT_EQ(actual[index].Age, expected[index].Age);
        EXPECT_FLOAT_EQ(actual[index].Size, expected[index].Size);
        for (uint32 axis = 0; axis < 3; ++axis)
        {
            EXPECT_FLOAT_EQ(actual[index].Position[axis], expected[index].Position[axis]);
            EXPECT_FLOAT_EQ(actual[index].Velocity[axis], expected[index].Velocity[axis]);
        }
    }
}

void ExpectSameParticles(const ParticleRuntime& actual, const ParticleRuntime& expected)
{
    ExpectSameParticles(Snapshot(actual), Snapshot(expected));
}

// A floor the physics collision queries: every segment ending within `radius` of it hits it.
struct RecordedSweep
{
    Vector3 From{};
    Vector3 To{};
    float Radius = 0.0f;
    uint32 Layers = 0;
};

struct FloorWorld
{
    float Height = -1.0f;
    mutable std::vector<RecordedSweep> Sweeps;
};

bool SweepFloor(const void* context, const Vector3& from, const Vector3& to, float radius, uint32 layers,
                CollisionHit& hit)
{
    const auto& floor = *static_cast<const FloorWorld*>(context);
    floor.Sweeps.push_back({from, to, radius, layers});
    if (to.y - radius > floor.Height)
        return false;
    hit.Position = {to.x, floor.Height, to.z};
    hit.Normal = {0.0f, 1.0f, 0.0f};
    return true;
}

std::vector<uint8> Cook(const StackDocument& document)
{
    std::vector<uint8> bytes;
    std::string error;
    EXPECT_TRUE(WriteParticleStackBinary(document, bytes, error)) << error;
    return bytes;
}

void WriteUInt32(std::vector<uint8>& bytes, size_t offset, uint32 value)
{
    for (size_t byte = 0; byte < 4; ++byte)
        bytes[offset + byte] = static_cast<uint8>(value >> (8 * byte));
}

size_t FindFloat(const std::vector<uint8>& bytes, float value)
{
    uint8 pattern[4];
    std::memcpy(pattern, &value, sizeof(pattern));
    const auto found = std::search(bytes.begin(), bytes.end(), std::begin(pattern), std::end(pattern));
    return found == bytes.end() ? bytes.size() : static_cast<size_t>(found - bytes.begin());
}

ParticleProcessorGeometry TwoTriangles()
{
    ParticleProcessorGeometry geometry;
    // Areas 1 and 3, both on the plane z = 0.
    geometry.Vertices = {{0, 0, 0}, {2, 0, 0}, {0, 1, 0}, {10, 0, 0}, {16, 0, 0}, {10, 1, 0}};
    geometry.Indices = {0, 1, 2, 3, 4, 5};
    return geometry;
}

// One instance of every processor type the module registers, parameters off their defaults, so a
// codec that drops or reorders a field changes the document.
StackDocument EveryBuiltInProcessor()
{
    auto document = EmptyStack(3.0f);
    document.Phases.push_back(StackPhase{.Id = 20, .Label = "Fall"});
    Append(document, Processor(ParticleEmitRateProcessor(),
                               ParticleEmitRateParameters{.InputMaximum = 2.0f,
                                                          .Rate = ParticleValue::MakeCurve({{0.0f, 10.0f}, {1.0f, 40.0f}})}));
    Append(document, Processor(ParticleEmitBurstProcessor(),
                               ParticleEmitBurstParameters{.Time = 0.25f, .Count = 7, .RepeatInterval = 1.5f}));
    auto mesh = Processor(ParticleShapeProcessor(), ParticleShapeParameters{.Shape = ParticleShapeKind::MeshSurface,
                                                                            .Offset = {0.5f, 0.0f, -0.5f},
                                                                            .AlignToNormal = true});
    mesh.Geometry = TwoTriangles();
    Append(document, std::move(mesh));
    ParticlePropertyParameters color{.Target = ParticleAttribute::Color, .Operation = ParticleOperation::Set};
    color.Value[0] = {0.2f, 0.8f};
    color.Value[3] = 0.5f;
    Append(document, Processor(ParticlePropertyProcessor(), color, ParticleStage::Birth));
    Append(document, Processor(ParticleAccelerationProcessor(),
                               ParticleAccelerationParameters{.Space = ParticleSpace::Local,
                                                              .Acceleration = {1.0f, 2.0f, 3.0f}}));
    auto drag = Processor(ParticleDragProcessor(), ParticleDragParameters{.Mode = ParticleDragMode::Linear, .Drag = {0.1f, 0.4f}});
    drag.Label = "Drift";
    drag.Enabled = false;
    drag.Clock = ParticleClock::Age;
    drag.Start = 0.2f;
    drag.End = 1.3f;
    Append(document, std::move(drag));
    Append(document, Processor(ParticleVelocityConeProcessor(),
                               ParticleVelocityConeParameters{.Space = ParticleSpace::World, .Direction = {1.0f, 0.0f, 0.0f}, .Spread = 60.0f, .Speed = 4.0f, .InheritEmitterVelocity = 0.25f}));
    Append(document, Processor(ParticleLimitSpeedProcessor(), ParticleLimitSpeedParameters{.Speed = 7.0f}));
    Append(document, Processor(ParticleOrbitProcessor(),
                               ParticleOrbitParameters{.Axis = {0.0f, 0.0f, 1.0f}, .Radial = {-1.0f, 1.0f}, .Tangential = 2.0f}));
    Append(document, Processor(ParticleNoiseProcessor(),
                               ParticleNoiseParameters{.Algorithm = ParticleNoiseAlgorithm::Gradient,
                                                       .Octaves = 3,
                                                       .Axes = {1.0f, 0.5f, 1.0f}}));
    Append(document, Processor(ParticleCollisionProcessor(),
                               ParticleCollisionParameters{.PlaneNormal = {0.0f, 0.0f, 1.0f}, .PlaneOffset = 2.0f, .Bounce = 0.3f}));
    Append(document, Processor(ParticleLightProcessor(), ParticleLightParameters{.Intensity = 2.0f, .Range = 3.0f}));
    Append(document, Processor(ParticleTrailProcessor(), ParticleTrailParameters{.Points = 32, .Spacing = 0.1f}));
    Append(document, Processor(ParticleEventProcessor(),
                               ParticleEventParameters{.Trigger = ParticleEventTrigger::Age, .Seconds = 0.5f, .Destination = 20}));
    ParticleEventParameters flash{.Trigger = ParticleEventTrigger::External, .Action = ParticleEventAction::Emit, .Count = 5, .Inherit = kParticleInheritAll};
    std::strcpy(flash.EventName, "Flash");
    flash.SubEmitter = 1;
    Append(document, Processor(ParticleEventProcessor(), flash), 20);
    return document;
}

bool HoldsType(const StackDocument& document, std::string_view type)
{
    for (const auto& phase : document.Phases)
        for (const auto& processor : phase.Processors)
            if (processor.Descriptor->Id == type)
                return true;
    return false;
}

bool IsValid(const std::function<void(StackDocument&)>& build)
{
    auto document = EmptyStack(1.0f);
    build(document);
    std::vector<StackDiagnostic> diagnostics;
    return ValidateStack(document, diagnostics);
}

bool IsValidDrag(const ParticleValue& drag)
{
    return IsValid([&drag](StackDocument& document)
                   { Append(document, Processor(ParticleDragProcessor(), ParticleDragParameters{.Drag = drag})); });
}

float SizeAfterBirth(bool setBeforeTriple)
{
    auto document = EmptyStack(5.0f);
    Append(document, Processor(ParticleEmitBurstProcessor(), ParticleEmitBurstParameters{.Count = 4}));
    const auto set = Processor(ParticlePropertyProcessor(),
                               Scalar(ParticleAttribute::Size, ParticleOperation::Set, ParticleBasis::Current, 2.0f),
                               ParticleStage::Birth);
    const auto triple =
        Processor(ParticlePropertyProcessor(),
                  Scalar(ParticleAttribute::Size, ParticleOperation::Multiply, ParticleBasis::Current, 3.0f),
                  ParticleStage::Birth);
    Append(document, setBeforeTriple ? set : triple);
    Append(document, setBeforeTriple ? triple : set);
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 8);
    runtime.Advance(Input(), kFrame);
    EXPECT_EQ(runtime.Count(), 4u);
    return runtime.Count() > 0 ? runtime.Channels().Sizes()[0] : 0.0f;
}
} // namespace

TEST(ParticleRuntime, UnboundAndZeroBudgetEmittersHoldNoParticles)
{
    ParticleRuntime unbound;
    unbound.Advance(Input(), kFrame);
    EXPECT_EQ(unbound.Count(), 0u);

    ParticleRuntime runtime;
    runtime.Bind(Compile(Fountain()), 100000);
    EXPECT_EQ(runtime.Capacity(), kMaxParticlesPerEmitter);
    auto input = Input();
    input.Budget = 0;
    AdvanceFrames(runtime, input, 30);
    EXPECT_EQ(runtime.Count(), 0u);
    input.Budget = 5;
    AdvanceFrames(runtime, input, 30);
    EXPECT_EQ(runtime.Count(), 5u);
}

TEST(ParticleRuntime, FixedTicksGiveTheSameResultAtEveryDisplayRate)
{
    const auto stack = Compile(Fountain());
    ParticleRuntime reference;
    reference.Bind(stack, 512);
    AdvanceFrames(reference, Input(), 120);
    ASSERT_GT(reference.Count(), 100u);
    for (const int displayRate : {30, 144})
    {
        ParticleRuntime runtime;
        runtime.Bind(stack, 512);
        for (int frame = 0; frame < displayRate * 2; ++frame)
            runtime.Advance(Input(), 1.0 / displayRate);
        EXPECT_DOUBLE_EQ(runtime.Elapsed(), reference.Elapsed()) << displayRate << " frames per second";
        ExpectSameParticles(runtime, reference);
    }
}

namespace
{
// Where the particles of an emitter moving along +X at `speed` were born, oldest first, after one
// second shown at `displayRate` frames per second from the origin.
std::vector<float> BirthPositionsAlongAMove(const std::shared_ptr<const CompiledParticleStack>& stack,
                                            int displayRate, float speed)
{
    ParticleRuntime runtime;
    runtime.Bind(stack, 1024);
    auto input = Input();
    input.FixedFps = 30;
    float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    input.Transform = transform;
    runtime.Advance(input, 0.0);
    for (int frame = 1; frame <= displayRate; ++frame)
    {
        transform[12] = speed * static_cast<float>(frame) / static_cast<float>(displayRate);
        runtime.Advance(input, 1.0 / displayRate);
    }
    std::vector<float> positions;
    for (const ParticleState& particle : Snapshot(runtime))
        positions.push_back(particle.Position[0]);
    return positions;
}
} // namespace

// A moving emitter lays its particles along its path one spacing apart, wherever the display frames
// fall: each tick sees the emitter where that tick ended, and a tick's particles spread over the
// stretch the emitter covered during it.
TEST(ParticleRuntime, AMovingEmitterLaysTheSameTrailAtEveryDisplayRate)
{
    constexpr float kSpeed = 3.0f;
    constexpr float kRate = 300.0f;
    auto document = EmptyStack(10.0f);
    Append(document, Processor(ParticleEmitRateProcessor(), ParticleEmitRateParameters{.Rate = kRate}));
    const auto stack = Compile(document);
    const std::vector<float> reference = BirthPositionsAlongAMove(stack, 60, kSpeed);
    ASSERT_EQ(reference.size(), 300u) << "one second at 300 a second";
    for (size_t index = 1; index < reference.size(); ++index)
        EXPECT_NEAR(reference[index] - reference[index - 1], kSpeed / kRate, 1e-4f)
            << "particle " << index << " is not one spacing from the one before";
    for (const int displayRate : {24, 144})
    {
        const std::vector<float> positions = BirthPositionsAlongAMove(stack, displayRate, kSpeed);
        ASSERT_EQ(positions.size(), reference.size()) << displayRate << " frames per second";
        for (size_t index = 0; index < positions.size(); ++index)
            EXPECT_NEAR(positions[index], reference[index], 1e-4f)
                << displayRate << " frames per second, particle " << index;
    }
}

// Particles born at one point on different ticks meet different turbulence: the default noise field
// moves with the emitter's time, so the newborns do not all leave in one direction.
TEST(ParticleRuntime, TheDefaultNoiseSendsNewbornsAtOnePointDifferentWays)
{
    auto document = EmptyStack(10.0f);
    Append(document, Processor(ParticleEmitRateProcessor(), ParticleEmitRateParameters{.Rate = 60.0f}));
    Append(document, Processor(ParticleNoiseProcessor(), ParticleNoiseParameters{}));
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 256);
    std::vector<Vector3> firstPushes;
    for (int frame = 0; frame < 40; ++frame)
    {
        runtime.Advance(Input(), kFrame);
        const auto particles = Snapshot(runtime);
        // The second-youngest particle has had exactly one noise update since its birth.
        if (particles.size() >= 2)
            firstPushes.push_back(particles[particles.size() - 2].Velocity);
    }
    ASSERT_GT(firstPushes.size(), 30u);
    float widest = 0.0f;
    for (const Vector3& push : firstPushes)
    {
        ASSERT_GT(push.Length(), 0.0f) << "positive control: the noise pushes every newborn";
        const float cosine = Vector3::Dot(push, firstPushes.front()) / (push.Length() * firstPushes.front().Length());
        widest = std::max(widest, std::acos(std::clamp(cosine, -1.0f, 1.0f)));
    }
    EXPECT_GT(widest, 0.35f) << "every newborn left within " << widest << " radians of the first";
}

// The analytic field is sampled at a seeded offset: emitters that differ only in their seed push their
// first particle different ways, where an unseeded field pushes every emitter at one point alike.
TEST(ParticleRuntime, TheDefaultNoiseDiffersBetweenEmitterSeeds)
{
    auto document = EmptyStack(10.0f);
    Append(document, Processor(ParticleEmitRateProcessor(), ParticleEmitRateParameters{.Rate = 60.0f}));
    Append(document, Processor(ParticleNoiseProcessor(), ParticleNoiseParameters{}));
    const auto stack = Compile(document);
    std::vector<Vector3> firstPushes;
    for (uint32 seed = 1; seed <= 8; ++seed)
    {
        ParticleRuntime runtime;
        runtime.Bind(stack, 16);
        auto input = Input();
        input.Seed = seed;
        runtime.Advance(input, kFrame);
        runtime.Advance(input, kFrame);
        const auto particles = Snapshot(runtime);
        ASSERT_FALSE(particles.empty());
        firstPushes.push_back(particles.front().Velocity);
    }
    float widest = 0.0f;
    for (const Vector3& push : firstPushes)
    {
        ASSERT_GT(push.Length(), 0.0f) << "positive control: the noise pushes the first particle";
        const float cosine = Vector3::Dot(push, firstPushes.front()) / (push.Length() * firstPushes.front().Length());
        widest = std::max(widest, std::acos(std::clamp(cosine, -1.0f, 1.0f)));
    }
    EXPECT_GT(widest, 0.35f) << "eight seeds pushed their first particle within " << widest << " radians of each other";
}

// A scroll each particle picks for itself wraps the analytic field's clock in float, from a clock split once
// per tick; one every particle shares is wrapped once per tick in double. The two agree across the
// field's period boundaries: a range of one value moves a cloud exactly as the constant does.
TEST(ParticleRuntime, APerParticleScrollWrapsTheFieldClockAsASharedOneDoes)
{
    constexpr uint32 kCloud = 64;
    constexpr float kScroll = 0.37f;
    // 800 ticks at 60 per second cross the clock's 2 pi boundary twice.
    constexpr int kTicks = 800;
    const auto cloud = [](const ParticleValue& scroll)
    {
        auto document = EmptyStack(20.0f);
        Append(document, Processor(ParticleEmitBurstProcessor(), ParticleEmitBurstParameters{.Count = kCloud}));
        Append(document, Processor(ParticleShapeProcessor(), ParticleShapeParameters{.Radius = 3.0f}));
        ParticleNoiseParameters noise{.Mode = ParticleNoiseMode::Displacement, .Strength = 0.1f};
        noise.Scroll = scroll;
        Append(document, Processor(ParticleNoiseProcessor(), noise));
        return Compile(document);
    };
    ParticleRuntime shared;
    shared.Bind(cloud(ParticleValue{kScroll}), kCloud);
    ParticleRuntime perParticle;
    perParticle.Bind(cloud(ParticleValue{kScroll, kScroll}), kCloud);
    float widest = 0.0f;
    Vector3 start{};
    for (int tick = 0; tick < kTicks; ++tick)
    {
        shared.Advance(Input(), kFrame);
        perParticle.Advance(Input(), kFrame);
        const auto a = shared.Channels().Positions();
        const auto b = perParticle.Channels().Positions();
        ASSERT_EQ(a.size(), static_cast<size_t>(kCloud));
        ASSERT_EQ(b.size(), a.size());
        for (size_t index = 0; index < a.size(); ++index)
            widest = std::max(widest, (a[index] - b[index]).Length());
        if (tick == 0)
            start = a[0];
    }
    EXPECT_GT((shared.Channels().Positions()[0] - start).Length(), 0.01f) << "positive control: the noise moves the cloud";
    EXPECT_LT(widest, 1e-4f) << "a per-particle scroll moved the cloud up to " << widest << " away from a shared one";
}

namespace
{
// A column-major rotation about +Y by `degrees`, at `translationX` along X.
void RotationAboutY(float degrees, float translationX, float* matrix)
{
    const float radians = degrees * 3.14159265358979f / 180.0f;
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    const float values[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, translationX, 0, 0, 1};
    std::copy_n(values, 16, matrix);
}
} // namespace

// Between two frames the emitter turns rather than shrinks: a particle born one unit from a
// spinning emitter's origin is born one unit away on every tick of the frame, not pulled toward the
// axis by blending the two frames' matrices element by element.
TEST(ParticleRuntime, ASpinningEmitterKeepsItsShapeOffsetAtEveryTick)
{
    auto document = EmptyStack(10.0f);
    Append(document, Processor(ParticleEmitRateProcessor(), ParticleEmitRateParameters{.Rate = 120.0f}));
    Append(document, Processor(ParticleShapeProcessor(),
                               ParticleShapeParameters{.Radius = 0.0f, .Offset = {1.0f, 0.0f, 0.0f}}));
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 1024);
    auto input = Input();
    float matrix[16];
    RotationAboutY(0.0f, 0.0f, matrix);
    input.Transform = matrix;
    runtime.Advance(input, 0.0);
    // 30 display frames a second at a 60 Hz tick, 90 degrees a frame: two ticks per frame.
    for (int frame = 1; frame <= 8; ++frame)
    {
        RotationAboutY(90.0f * static_cast<float>(frame), 0.0f, matrix);
        runtime.Advance(input, 1.0 / 30.0);
    }
    ASSERT_GT(runtime.Count(), 8u) << "positive control: particles were born on the in-between ticks";
    float nearest = 1.0f;
    for (const ParticleState& particle : Snapshot(runtime))
        nearest = std::min(nearest, std::hypot(particle.Position.x, particle.Position.z));
    EXPECT_GT(nearest, 0.99f) << "a particle was born " << nearest << " units from the spin axis";
}

// A jump no emitter makes in one frame (a pooled effect placed again, an owner that respawned) is a
// teleport: the frame's particles are born where the emitter arrived, not along the line between the
// two places; a fast move within the bound still lays its particles along the path.
TEST(ParticleRuntime, ATeleportedEmitterDoesNotSpawnAlongTheJump)
{
    auto document = EmptyStack(5.0f);
    Append(document, Processor(ParticleEmitRateProcessor(), ParticleEmitRateParameters{.Rate = 600.0f}));
    const auto stack = Compile(document);
    const auto bornBetween = [&stack](float destination)
    {
        ParticleRuntime runtime;
        runtime.Bind(stack, 4096);
        float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        auto input = Input();
        input.Transform = matrix;
        for (int frame = 0; frame < 10; ++frame)
            runtime.Advance(input, 1.0 / 30.0);
        const uint32 before = runtime.Count();
        matrix[12] = destination;
        runtime.Advance(input, 1.0 / 30.0);
        uint32 between = 0;
        for (uint32 index = before; index < runtime.Count(); ++index)
        {
            const float x = runtime.Channels().Positions()[index].x;
            between += (x > 0.01f * destination && x < 0.99f * destination) ? 1u : 0u;
        }
        return std::pair{runtime.Count() - before, between};
    };
    const auto [bornInJump, betweenJump] = bornBetween(100.0f);
    ASSERT_GT(bornInJump, 10u);
    EXPECT_EQ(betweenJump, 0u) << "a 100-unit jump in one frame laid particles along it";
    const auto [bornInMove, betweenMove] = bornBetween(10.0f);
    ASSERT_GT(bornInMove, 10u);
    EXPECT_GT(betweenMove, bornInMove / 2) << "a 10-unit move in one frame is motion, and lays its particles along the path";
}

// A velocity-facing mesh divides the previous-to-current move by the last tick's length, so that
// length has to be the tick's at any rate, the bounded variable steps included.
TEST(ParticleRuntime, TheLastTickIsTheTimeTheChannelsMovedOver)
{
    const auto stack = Compile(Fountain());
    ParticleRuntime fixed;
    fixed.Bind(stack, 64);
    auto input = Input();
    input.FixedFps = 30;
    fixed.Advance(input, 1.0 / 30.0);
    EXPECT_FLOAT_EQ(fixed.LastTickSeconds(), 1.0f / 30.0f);

    ParticleRuntime variable;
    variable.Bind(stack, 64);
    input.FixedFps = 0;
    variable.Advance(input, 0.02);
    EXPECT_FLOAT_EQ(variable.LastTickSeconds(), 0.02f) << "one bounded step covers the whole short frame";
    variable.Advance(input, 0.05);
    EXPECT_FLOAT_EQ(variable.LastTickSeconds(), 0.025f) << "a longer frame splits into equal steps";
}

TEST(ParticleRuntime, FramesShorterThanATickOnlyInterpolate)
{
    const auto stack = Compile(Fountain());
    ParticleRuntime whole;
    ParticleRuntime split;
    whole.Bind(stack, 512);
    split.Bind(stack, 512);
    for (int tick = 0; tick < 60; ++tick)
    {
        whole.Advance(Input(), kFrame);
        split.Advance(Input(), kFrame / 3.0);
        EXPECT_EQ(split.StepsLastFrame(), 0u);
        EXPECT_NEAR(split.Interpolation(), 1.0f / 3.0f, 1e-3f);
        split.Advance(Input(), 0.0);
        split.Advance(Input(), kFrame / 3.0);
        split.Advance(Input(), kFrame / 3.0);
    }
    ExpectSameParticles(split, whole);
}

TEST(ParticleRuntime, HitchesCatchUpAtMostEightTicksAndSpeedScaleScalesTime)
{
    ParticleRuntime runtime;
    runtime.Bind(Compile(Fountain()), 512);
    runtime.Advance(Input(), 1.0);
    EXPECT_EQ(runtime.StepsLastFrame(), 8u);
    EXPECT_NEAR(runtime.DroppedTime(), 52.0 / 60.0, 1e-9);
    EXPECT_NEAR(runtime.Elapsed(), 8.0 / 60.0, 1e-6);
    auto fast = Input();
    fast.SpeedScale = 2.0;
    runtime.Advance(fast, kFrame);
    EXPECT_EQ(runtime.StepsLastFrame(), 2u);
    auto stopped = Input();
    stopped.SpeedScale = 0.0;
    const double before = runtime.Elapsed();
    runtime.Advance(stopped, kFrame);
    EXPECT_EQ(runtime.Elapsed(), before);
}

TEST(ParticleRuntime, ResetAndSeekReproduceTheSimulation)
{
    const auto stack = Compile(Fountain());
    ParticleRuntime played;
    played.Bind(stack, 512);
    AdvanceFrames(played, Input(), 90);
    ParticleRuntime sought;
    sought.Bind(stack, 512);
    sought.Seek(Input(), 1.5);
    ExpectSameParticles(sought, played);
    const auto before = Snapshot(played);
    played.Reset(kSeed);
    EXPECT_EQ(played.Count(), 0u);
    AdvanceFrames(played, Input(), 90);
    ExpectSameParticles(Snapshot(played), before);
}

TEST(ParticleRuntime, ChunkedSeekMatchesTheWholeSeekAndRaisesNoGameplayEffects)
{
    auto document = Fountain();
    ParticleEventParameters sparks{.Trigger = ParticleEventTrigger::Birth, .Action = ParticleEventAction::Emit};
    Append(document, Processor(ParticleEventProcessor(), sparks));
    Append(document, Processor(ParticleCollisionProcessor(), ParticleCollisionParameters{.PlaneOffset = 0.5f}));
    const auto stack = Compile(document);
    ParticleRuntime whole;
    whole.Bind(stack, 512);
    whole.Seek(Input(), 2.0);
    ParticleRuntime chunked;
    chunked.Bind(stack, 512);
    PreviewSeek seek;
    chunked.BeginSeek(Input(), 2.0, seek);
    uint32 chunks = 0;
    while (seek.Pending && chunks < 1000)
    {
        chunked.ContinueSeek(Input(), seek, 7);
        EXPECT_LE(chunked.StepsLastFrame(), 7u);
        EXPECT_TRUE(chunked.SpawnEvents().empty());
        EXPECT_TRUE(chunked.Collisions().empty());
        ++chunks;
    }
    EXPECT_FALSE(seek.Pending);
    EXPECT_GE(chunks, 18u);
    ExpectSameParticles(chunked, whole);

    // Played in real time, the same stack raises both.
    ParticleRuntime played;
    played.Bind(stack, 512);
    bool spawned = false;
    bool collided = false;
    for (int frame = 0; frame < 120; ++frame)
    {
        played.Advance(Input(), kFrame);
        spawned |= !played.SpawnEvents().empty();
        collided |= !played.Collisions().empty();
    }
    EXPECT_TRUE(spawned);
    EXPECT_TRUE(collided);
}

TEST(ParticleRuntime, PrewarmSpreadsOverFramesAndEndsWhereASeekWould)
{
    const auto stack = Compile(Fountain());
    auto input = Input();
    input.PrewarmSeconds = 3.0;
    ParticleRuntime prewarmed;
    prewarmed.Bind(stack, 512);
    uint32 frames = 0;
    do
    {
        prewarmed.Advance(input, kFrame);
        ++frames;
    } while (prewarmed.IsPrewarming() && frames < 1000);
    EXPECT_FALSE(prewarmed.IsPrewarming());
    // 180 ticks at most 64 per frame.
    EXPECT_GE(frames, 3u);
    ParticleRuntime sought;
    sought.Bind(stack, 512);
    sought.Seek(Input(), 3.0);
    ExpectSameParticles(prewarmed, sought);
}

TEST(ParticleRuntime, BurstsFireOnTheirScheduleUntilTheEmitterRestarts)
{
    auto once = EmptyStack(5.0f);
    Append(once, Processor(ParticleEmitBurstProcessor(), ParticleEmitBurstParameters{.Count = 10}));
    ParticleRuntime runtime;
    runtime.Bind(Compile(once), 64);
    runtime.Advance(Input(), kFrame);
    EXPECT_EQ(runtime.Count(), 10u);
    AdvanceFrames(runtime, Input(), 120);
    EXPECT_EQ(runtime.Count(), 10u);
    runtime.Reset(kSeed);
    runtime.Advance(Input(), kFrame);
    ASSERT_EQ(runtime.Count(), 10u);
    const auto spawns = runtime.Channels().SpawnIndices();
    EXPECT_EQ(*std::max_element(spawns.begin(), spawns.begin() + runtime.Count()), 9u);

    auto repeating = EmptyStack(5.0f);
    Append(repeating, Processor(ParticleEmitBurstProcessor(),
                                ParticleEmitBurstParameters{.Count = 10, .RepeatInterval = 0.5f}));
    ParticleRuntime repeated;
    repeated.Bind(Compile(repeating), 64);
    AdvanceFrames(repeated, Input(), 70);
    EXPECT_EQ(repeated.Count(), 30u);
}

// A burst whose particles all die in one tick frees the top slots, and the next burst reuses them:
// a reused slot starts clean, so its particle lives its own lifetime and runs its birth rules.
TEST(ParticleRuntime, ReusedSlotsLiveTheirLifetimeAndRunTheirBirthRules)
{
    constexpr float kLifetime = 0.25f;
    auto document = EmptyStack(kLifetime);
    Append(document, Processor(ParticleEmitBurstProcessor(),
                               ParticleEmitBurstParameters{.Count = 10, .RepeatInterval = 0.5f}));
    Append(document, Processor(ParticleEventProcessor(),
                               ParticleEventParameters{.Trigger = ParticleEventTrigger::Birth,
                                                       .Action = ParticleEventAction::Emit, .Count = 1}));
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 64);
    uint32 most = 0;
    float oldest = 0.0f;
    std::vector<uint32> birthsPerBurst;
    // Just under three seconds: the bursts at 0, 0.5, ... 2.5 s.
    for (int frame = 0; frame < 170; ++frame)
    {
        runtime.Advance(Input(), kFrame);
        uint32 births = 0;
        for (const auto& event : runtime.SpawnEvents())
            births += event.Trigger == ParticleEventTrigger::Birth ? 1u : 0u;
        if (births > 0)
            birthsPerBurst.push_back(births);
        most = std::max(most, runtime.Count());
        const auto ages = runtime.Channels().Ages();
        for (uint32 index = 0; index < runtime.Count(); ++index)
            oldest = std::max(oldest, ages[index]);
    }
    EXPECT_LE(most, 10u);
    EXPECT_LE(oldest, kLifetime + static_cast<float>(kFrame));
    EXPECT_EQ(birthsPerBurst, std::vector<uint32>(6, 10u));
}

// One particle a burst, gone before the next: the emitter holds one particle, then none, every cycle.
TEST(ParticleRuntime, ARepeatingOneParticleBurstComesAndGoes)
{
    auto document = EmptyStack(0.25f);
    Append(document, Processor(ParticleEmitBurstProcessor(),
                               ParticleEmitBurstParameters{.Count = 1, .RepeatInterval = 0.5f}));
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 4);
    std::vector<uint32> counts;
    for (int frame = 1; frame <= 300; ++frame)
    {
        runtime.Advance(Input(), kFrame);
        // 0.1 s and 0.4 s into each half-second cycle.
        if (frame % 30 == 6 || frame % 30 == 24)
            counts.push_back(runtime.Count());
    }
    std::vector<uint32> expected;
    for (int cycle = 0; cycle < 10; ++cycle)
    {
        expected.push_back(1u);
        expected.push_back(0u);
    }
    EXPECT_EQ(counts, expected);
}

// A fade-in over the lifetime must already hold at birth: the frame a particle first exists draws
// the curve's value at age 0, not the birth color the curve has yet to scale. Both the current
// and the previous color carry it, so the draw interpolates between equal values.
TEST(ParticleRuntime, ANewbornHoldsTheValueItsLifetimeCurvesGiveAtAgeZero)
{
    auto document = EmptyStack(2.0f);
    Append(document, Processor(ParticleEmitRateProcessor(), ParticleEmitRateParameters{.Rate = 30.0f}));
    ParticlePropertyParameters fadeIn{.Target = ParticleAttribute::Color,
                                      .Operation = ParticleOperation::Multiply,
                                      .Basis = ParticleBasis::Birth};
    for (uint32 component = 0; component < 3; ++component)
        fadeIn.Value[component] = 1.0f;
    fadeIn.Value[3] = ParticleValue::MakeCurve({{0.0f, 0.0f}, {1.0f, 1.0f}});
    Append(document, Processor(ParticlePropertyProcessor(), fadeIn));
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 64);

    uint32 newborns = 0;
    for (int frame = 1; frame <= 120; ++frame)
    {
        runtime.Advance(Input(), kFrame);
        const auto& channels = runtime.Channels();
        const auto previousColors = channels.Get<Mathematics::Vector4>(ParticleChannel::PreviousColor);
        for (uint32 index = 0; index < runtime.Count(); ++index)
        {
            if (channels.Ages()[index] != 0.0f)
                continue;
            ++newborns;
            EXPECT_FLOAT_EQ(channels.Colors()[index].w, 0.0f) << "frame " << frame;
            EXPECT_FLOAT_EQ(previousColors[index].w, 0.0f) << "frame " << frame;
        }
    }
    EXPECT_GE(newborns, 40u) << "the emitter did not spawn, so nothing was checked";
    const auto colors = runtime.Channels().Colors();
    for (uint32 index = 0; index < runtime.Count(); ++index)
        if (runtime.Channels().Ages()[index] > 0.5f)
            EXPECT_GT(colors[index].w, 0.0f) << "the curve did not fade the particle in";
}

// The pass runs after the Enter processors and sees what they wrote: an Enter value is the entry
// basis the Update processor scales on the first frame.
TEST(ParticleRuntime, TheBirthPassSeesWhatTheEnterProcessorsWrote)
{
    auto document = EmptyStack(5.0f);
    Append(document, Processor(ParticleEmitBurstProcessor(), ParticleEmitBurstParameters{.Count = 1}));
    Append(document, Processor(ParticlePropertyProcessor(),
                               Scalar(ParticleAttribute::Size, ParticleOperation::Set, ParticleBasis::Current, 3.0f),
                               ParticleStage::Enter));
    Append(document, Processor(ParticlePropertyProcessor(),
                               Scalar(ParticleAttribute::Size, ParticleOperation::Multiply, ParticleBasis::Entry, 2.0f)));
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 4);
    runtime.Advance(Input(), kFrame);
    ASSERT_EQ(runtime.Count(), 1u);
    EXPECT_FLOAT_EQ(runtime.Channels().Sizes()[0], 6.0f);
}

// An update processor on the current value compounds once at birth and then once per tick, and
// what a child inherited is the value it starts from.
TEST(ParticleRuntime, ACurrentBasisUpdateStartsFromTheInheritedValueAndCompoundsEveryTick)
{
    auto document = EmptyStack(10.0f);
    Append(document, Processor(ParticlePropertyProcessor(),
                               Scalar(ParticleAttribute::Size, ParticleOperation::Set, ParticleBasis::Current, 1.75f),
                               ParticleStage::Birth));
    Append(document, Processor(ParticlePropertyProcessor(),
                               Scalar(ParticleAttribute::Size, ParticleOperation::Multiply, ParticleBasis::Current, 2.0f)));
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 4);
    ParticleSpawnRequest parent;
    parent.Inherit = kParticleInheritSize;
    parent.Size = 0.35f;
    ASSERT_TRUE(runtime.QueueSpawn(parent));
    runtime.Advance(Input(), kFrame);
    ASSERT_EQ(runtime.Count(), 1u);
    EXPECT_FLOAT_EQ(runtime.Channels().Sizes()[0], 0.7f);
    runtime.Advance(Input(), kFrame);
    EXPECT_FLOAT_EQ(runtime.Channels().Sizes()[0], 1.4f);
}

TEST(ParticleRuntime, SwitchedOffEmissionSpawnsNothingAndRestartsFromTheBeginning)
{
    auto document = EmptyStack(10.0f);
    Append(document, Processor(ParticleEmitBurstProcessor(), ParticleEmitBurstParameters{.Time = 0.5f, .Count = 4}));
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 64);
    auto input = Input();
    input.Emitting = false;
    AdvanceFrames(runtime, input, 60);
    EXPECT_EQ(runtime.Count(), 0u);
    input.Emitting = true;
    AdvanceFrames(runtime, input, 29);
    EXPECT_EQ(runtime.Count(), 0u);
    AdvanceFrames(runtime, input, 2);
    EXPECT_EQ(runtime.Count(), 4u);
}

TEST(ParticleRuntime, ViewDepthDoesNotConfuseSidewaysDistanceWithDepth)
{
    const float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const Vector3 nearSideways{100, 0, 2};
    const Vector3 farCentered{0, 0, 10};
    EXPECT_LT(ViewDepth(view, nearSideways), ViewDepth(view, farCentered));
    const float turned[16] = {0, 0, 1, 0, 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, -5, 1};
    EXPECT_GT(ViewDepth(turned, nearSideways), ViewDepth(turned, farCentered));
}

TEST(ParticleRuntime, FlipbookSpeedLoopStartAndLifetimePlayback)
{
    Components::ParticleRenderer renderer;
    renderer.Columns = 4;
    renderer.Rows = 2;
    renderer.FrameCount = 6;
    renderer.FrameRate = 4;
    renderer.StartFrame = 1;
    EXPECT_FLOAT_EQ(FlipbookFrame(renderer, 0.5f, 10), 3);
    EXPECT_FLOAT_EQ(FlipbookFrame(renderer, 2, 10), 3);
    EXPECT_FLOAT_EQ(FlipbookFrame(renderer, 0.5f, 10, 2), 5);
    EXPECT_FLOAT_EQ(FlipbookFrame(renderer, 0, 10, 1, 0.5f), 4);
    renderer.Loop = false;
    EXPECT_FLOAT_EQ(FlipbookFrame(renderer, 2, 10), 5);
    renderer.FrameRate = 0;
    renderer.StartFrame = 0;
    EXPECT_FLOAT_EQ(FlipbookFrame(renderer, 5, 10), 3.0f);
    EXPECT_FLOAT_EQ(FlipbookFrame(renderer, 10, 10), 5);
    EXPECT_EQ(static_cast<int>(std::floor(FlipbookFrame(renderer, 9.999f, 10))), 5);
    renderer.Columns = 0;
    renderer.Rows = 0;
    EXPECT_FLOAT_EQ(FlipbookFrame(renderer, 5, 10), 0);
}

TEST(ParticleRuntime, LifetimeFlipbookAllocatesTimeToEveryFrame)
{
    Components::ParticleRenderer renderer;
    renderer.Columns = 4;
    renderer.Rows = 4;
    renderer.FrameRate = 0;
    constexpr float lifetime = 1.25f;
    for (int frame = 0; frame < 16; ++frame)
    {
        const float age = (frame + 0.5f) * lifetime / 16;
        EXPECT_EQ(static_cast<int>(std::floor(FlipbookFrame(renderer, age, lifetime))), frame);
    }
}

TEST(ParticleRuntime, PhysicsCollisionSweepsInWorldSpaceAndReportsTheContact)
{
    auto document = EmptyStack(5.0f);
    Append(document, Processor(ParticleEmitBurstProcessor(), ParticleEmitBurstParameters{.Count = 1}));
    Append(document, Processor(ParticlePropertyProcessor(), SetVector(ParticleAttribute::Velocity, {0, -6, 0}),
                               ParticleStage::Birth));
    Append(document, Processor(ParticleCollisionProcessor(),
                               ParticleCollisionParameters{.Source = ParticleCollisionSource::Physics,
                                                           .Radius = 0.25f,
                                                           .KillOnContact = true,
                                                           .LayerMask = 0x4u}));
    const FloorWorld floor;
    const ParticleCollisionWorld world{SweepFloor, &floor};
    // Simulated in the emitter's space, which sits at (10, 2, 0).
    const float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 10, 2, 0, 1};
    auto input = Input();
    input.Transform = transform;
    input.LocalSpace = true;
    input.Collision = &world;
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 8);
    std::optional<ParticleCollisionRecord> contact;
    for (int frame = 0; frame < 60 && !contact; ++frame)
    {
        runtime.Advance(input, kFrame);
        if (!runtime.Collisions().empty())
            contact = runtime.Collisions().front();
    }
    ASSERT_TRUE(contact.has_value());
    EXPECT_TRUE(contact->Killed);
    EXPECT_FLOAT_EQ(contact->Hit.Position.x, 10.0f);
    EXPECT_FLOAT_EQ(contact->Hit.Position.y, -1.0f);
    EXPECT_NEAR(contact->Velocity.y, -6.0f, 1e-4f);
    EXPECT_EQ(runtime.Count(), 0u);
    ASSERT_FALSE(floor.Sweeps.empty());
    EXPECT_FLOAT_EQ(floor.Sweeps.front().From.y, 2.0f);
    for (const auto& sweep : floor.Sweeps)
    {
        EXPECT_FLOAT_EQ(sweep.From.x, 10.0f);
        EXPECT_FLOAT_EQ(sweep.To.x, 10.0f);
        EXPECT_FLOAT_EQ(sweep.Radius, 0.25f);
        EXPECT_EQ(sweep.Layers, 0x4u);
    }
}

TEST(ParticleRuntime, ADeathOnContactSpawnsChildrenAtTheContactPoint)
{
    auto document = EmptyStack(5.0f);
    Append(document, Processor(ParticleEmitBurstProcessor(), ParticleEmitBurstParameters{.Count = 1}));
    Append(document, Processor(ParticlePropertyProcessor(), SetVector(ParticleAttribute::Position, {0, 3, 0}),
                               ParticleStage::Birth));
    // Fast enough to end a tick well inside the plane.
    Append(document, Processor(ParticlePropertyProcessor(), SetVector(ParticleAttribute::Velocity, {0, -30, 0}),
                               ParticleStage::Birth));
    Append(document, Processor(ParticleCollisionProcessor(),
                               ParticleCollisionParameters{.Radius = 0.1f, .KillOnContact = true}));
    ParticleEventParameters sparks{.Trigger = ParticleEventTrigger::Death, .Action = ParticleEventAction::Emit, .Count = 3};
    Append(document, Processor(ParticleEventProcessor(), sparks));
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 8);
    std::optional<ParticleSpawnEvent> death;
    for (int frame = 0; frame < 60 && !death; ++frame)
    {
        runtime.Advance(Input(), kFrame);
        for (const auto& event : runtime.SpawnEvents())
            if (event.Trigger == ParticleEventTrigger::Death)
                death = event;
    }
    ASSERT_TRUE(death.has_value());
    EXPECT_FLOAT_EQ(death->Position.y, 0.0f);
    EXPECT_FLOAT_EQ(death->Normal.y, 1.0f);
    EXPECT_EQ(death->Count, 3u);
    EXPECT_EQ(death->SubEmitter, 0u);
}

TEST(ParticleRuntime, PlaneCollisionKeepsParticlesAboveThePlane)
{
    auto document = Fountain();
    Append(document, Processor(ParticleCollisionProcessor(),
                               ParticleCollisionParameters{.Radius = 0.1f, .Bounce = 0.5f}));
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 512);
    uint32 checked = 0;
    for (int frame = 0; frame < 180; ++frame)
    {
        runtime.Advance(Input(), kFrame);
        const auto& channels = runtime.Channels();
        for (uint32 index = 0; index < runtime.Count(); ++index)
        {
            // Newborn particles have not been through a tick yet.
            if (channels.Ages()[index] <= 0.0f)
                continue;
            EXPECT_GE(channels.Positions()[index].y, 0.1f - 1e-4f);
            ++checked;
        }
    }
    EXPECT_GT(checked, 1000u);
}

TEST(ParticleRuntime, AFullyDampedContactSettlesTheParticleUntilItDies)
{
    auto document = EmptyStack(1.5f);
    Append(document, Processor(ParticleEmitBurstProcessor(), ParticleEmitBurstParameters{.Count = 1}));
    Append(document, Processor(ParticlePropertyProcessor(), SetVector(ParticleAttribute::Position, {0, 1, 0}),
                               ParticleStage::Birth));
    Append(document, Processor(ParticlePropertyProcessor(), SetVector(ParticleAttribute::Velocity, {0, -5, 0}),
                               ParticleStage::Birth));
    Append(document, Processor(ParticleNoiseProcessor(), ParticleNoiseParameters{.Strength = 3.0f}));
    Append(document, Processor(ParticleCollisionProcessor(),
                               ParticleCollisionParameters{.Radius = 0.1f, .Damping = 1.0f}));
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 8);
    AdvanceFrames(runtime, Input(), 30);
    ASSERT_EQ(runtime.Count(), 1u);
    const Vector3 rest = runtime.Channels().Positions()[0];
    EXPECT_FLOAT_EQ(rest.y, 0.1f);
    float age = runtime.Channels().Ages()[0];
    for (int frame = 0; frame < 30; ++frame)
    {
        runtime.Advance(Input(), kFrame);
        ASSERT_EQ(runtime.Count(), 1u);
        const Vector3 position = runtime.Channels().Positions()[0];
        EXPECT_FLOAT_EQ(position.x, rest.x);
        EXPECT_FLOAT_EQ(position.y, rest.y);
        EXPECT_FLOAT_EQ(position.z, rest.z);
        EXPECT_GT(runtime.Channels().Ages()[0], age);
        age = runtime.Channels().Ages()[0];
    }
    AdvanceFrames(runtime, Input(), 60);
    EXPECT_EQ(runtime.Count(), 0u);
}

TEST(ParticleRuntime, NoiseIsDeterministicForASeedAndNothingAtZeroStrength)
{
    const auto stack = Compile(Fountain());
    ParticleRuntime first;
    ParticleRuntime second;
    first.Bind(stack, 512);
    second.Bind(stack, 512);
    AdvanceFrames(first, Input(), 60);
    AdvanceFrames(second, Input(), 60);
    ExpectSameParticles(first, second);

    auto calm = Fountain();
    auto quiet = calm;
    for (auto& processor : quiet.Phases[0].Processors)
        if (processor.Descriptor == &ParticleNoiseProcessor())
            processor.Params<ParticleNoiseParameters>().Strength = 0.0f;
    std::erase_if(calm.Phases[0].Processors, [](const ParticleProcessorInstance& processor)
                  { return processor.Descriptor == &ParticleNoiseProcessor(); });
    ParticleRuntime without;
    ParticleRuntime silent;
    without.Bind(Compile(calm), 512);
    silent.Bind(Compile(quiet), 512);
    AdvanceFrames(without, Input(), 60);
    AdvanceFrames(silent, Input(), 60);
    ExpectSameParticles(silent, without);
    const auto moved = Snapshot(first);
    const auto still = Snapshot(without);
    ASSERT_EQ(moved.size(), still.size());
    float difference = 0.0f;
    for (size_t index = 0; index < moved.size(); ++index)
        difference += (moved[index].Position - still[index].Position).Length();
    EXPECT_GT(difference, 0.1f);
}

TEST(ParticleRuntime, TrailsStayWithinTheirPointBudgetAndFollowTheirParticle)
{
    auto document = Fountain();
    Append(document, Processor(ParticleTrailProcessor(), ParticleTrailParameters{.Lifetime = 0.5f, .Points = 8}));
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 128);
    AdvanceFrames(runtime, Input(), 90);
    const auto& trails = runtime.Trails();
    const auto& channels = runtime.Channels();
    ASSERT_GT(trails.ActiveCount(), 0u);
    uint32 followed = 0;
    for (uint32 slot = 0; slot < trails.SlotCount(); ++slot)
    {
        if (!trails.IsActive(slot))
            continue;
        EXPECT_LE(trails.PointCount(slot), 8u);
        for (uint32 index = 0; index < runtime.Count(); ++index)
        {
            if (channels.SpawnIndices()[index] != trails.ParticleId(slot) || trails.PointCount(slot) == 0)
                continue;
            const auto& head = trails.Point(slot, trails.PointCount(slot) - 1).Position;
            EXPECT_FLOAT_EQ(head.x, channels.Positions()[index].x);
            EXPECT_FLOAT_EQ(head.y, channels.Positions()[index].y);
            ++followed;
        }
    }
    EXPECT_GT(followed, 0u);
    // Trails outlive their particles only until their last point expires.
    auto stopped = Input();
    stopped.Emitting = false;
    AdvanceFrames(runtime, stopped, 150);
    EXPECT_EQ(runtime.Count(), 0u);
    EXPECT_EQ(runtime.Trails().ActiveCount(), 0u);
}

TEST(ParticleRuntime, LongTrailsKeepTheirWholeHistory)
{
    auto document = EmptyStack(10.0f);
    Append(document, Processor(ParticleEmitBurstProcessor(), ParticleEmitBurstParameters{.Count = 1}));
    Append(document, Processor(ParticlePropertyProcessor(), SetVector(ParticleAttribute::Velocity, {1, 0, 0}),
                               ParticleStage::Birth));
    Append(document, Processor(ParticleTrailProcessor(), ParticleTrailParameters{.Lifetime = 10.0f, .Points = 200}));
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 4);
    AdvanceFrames(runtime, Input(), 150);
    const auto& trails = runtime.Trails();
    ASSERT_EQ(trails.ActiveCount(), 1u);
    uint32 slot = 0;
    while (!trails.IsActive(slot))
        ++slot;
    EXPECT_GT(trails.PointCount(slot), 64u);
    EXPECT_LE(trails.PointCount(slot), 200u);
    for (uint32 point = 1; point < trails.PointCount(slot); ++point)
        EXPECT_GT(trails.Point(slot, point).Position.x, trails.Point(slot, point - 1).Position.x);
}

TEST(ParticleRuntime, SteadyStateFramesAllocateNothing)
{
    auto document = Fountain();
    Append(document, Processor(ParticlePropertyProcessor(),
                               Scalar(ParticleAttribute::Size, ParticleOperation::Multiply, ParticleBasis::Birth,
                                      ParticleValue::MakeCurve({{0.0f, 0.5f}, {1.0f, 2.0f}}))));
    Append(document, Processor(ParticlePropertyProcessor(),
                               Scalar(ParticleAttribute::Alpha, ParticleOperation::Set, ParticleBasis::Current,
                                      ParticleValue::MakeCurve({{0.0f, 1.0f}, {1.0f, 0.0f}}))));
    Append(document, Processor(ParticleLimitSpeedProcessor(), ParticleLimitSpeedParameters{}));
    Append(document, Processor(ParticleCollisionProcessor(), ParticleCollisionParameters{.Bounce = 0.4f}));
    Append(document, Processor(ParticleLightProcessor(), ParticleLightParameters{}));
    Append(document, Processor(ParticleTrailProcessor(), ParticleTrailParameters{}));
    ParticleEventParameters sparks{.Trigger = ParticleEventTrigger::Distance, .Action = ParticleEventAction::Emit, .Spacing = 0.5f};
    Append(document, Processor(ParticleEventProcessor(), sparks));
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 1024);
    AdvanceFrames(runtime, Input(), 240);
    ASSERT_GT(runtime.Count(), 100u);
    // The particle module links statically, so the executable's allocation hook sees it.
    const GameEngine::Memory::AllocationCountScope allocations(GameEngine::Memory::CountWindow::ThisThread);
    AdvanceFrames(runtime, Input(), 120);
    runtime.SendEvent("Flash"_sid);
    AdvanceFrames(runtime, Input(), 2);
    EXPECT_EQ(allocations.Count(), 0u);
}

TEST(ParticleStack, JsonAndCookedFormsRoundTripEveryBuiltInProcessor)
{
    const auto document = EveryBuiltInProcessor();
    std::vector<StackDiagnostic> diagnostics;
    ASSERT_TRUE(ValidateStack(document, diagnostics)) << Describe(diagnostics);
    std::string missing;
    ParticleProcessorRegistry::ForEach(
        [&document, &missing](const ParticleProcessorDescriptor& descriptor)
        {
            if (descriptor.Id.find('.') == std::string_view::npos && !HoldsType(document, descriptor.Id))
                missing += std::string(descriptor.Id) + " ";
        });
    EXPECT_TRUE(missing.empty()) << missing;

    const auto json = SerializeParticleStack(document, 2);
    StackDocument parsed;
    ASSERT_TRUE(ParseParticleStack(json, parsed, diagnostics)) << Describe(diagnostics);
    EXPECT_EQ(SerializeParticleStack(parsed, 2), json);

    const auto bytes = Cook(document);
    StackDocument read;
    ASSERT_TRUE(ReadParticleStackBinary(bytes, read, diagnostics)) << Describe(diagnostics);
    EXPECT_EQ(SerializeParticleStack(read, 2), json);
    EXPECT_EQ(Cook(read), bytes);
}

TEST(ParticleStack, CookedReaderRefusesTrailingBytes)
{
    auto bytes = Cook(EveryBuiltInProcessor());
    bytes.push_back(0);
    WriteUInt32(bytes, 8, static_cast<uint32>(bytes.size()));
    auto output = MakeDefaultStack();
    const auto before = SerializeParticleStack(output, -1);
    std::vector<StackDiagnostic> diagnostics;
    EXPECT_FALSE(ReadParticleStackBinary(bytes, output, diagnostics));
    ASSERT_FALSE(diagnostics.empty());
    EXPECT_NE(diagnostics.front().Message.find("Trailing bytes"), std::string::npos) << Describe(diagnostics);
    EXPECT_EQ(SerializeParticleStack(output, -1), before);
}

TEST(ParticleStack, CookedReaderRefusesAVertexCountLargerThanItsData)
{
    auto document = EmptyStack(1.0f);
    auto mesh = Processor(ParticleShapeProcessor(), ParticleShapeParameters{.Shape = ParticleShapeKind::MeshVertices});
    mesh.Geometry.Vertices = {{1234.5f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    Append(document, std::move(mesh));
    auto bytes = Cook(document);
    const size_t firstVertex = FindFloat(bytes, 1234.5f);
    ASSERT_LT(firstVertex, bytes.size());
    // Within the vertex limit, far past the bytes that follow: the element check refuses it before
    // the vertices are allocated, where reading them one by one would fail only at the end.
    WriteUInt32(bytes, firstVertex - 4, 60000);
    StackDocument output;
    std::vector<StackDiagnostic> diagnostics;
    EXPECT_FALSE(ReadParticleStackBinary(bytes, output, diagnostics));
    ASSERT_FALSE(diagnostics.empty());
    EXPECT_EQ(diagnostics.front().Message, "Truncated particle stack array");
}

TEST(ParticleStack, CookedReaderRefusesAPhaseCountBeforeAllocating)
{
    auto bytes = Cook(MakeDefaultStack());
    WriteUInt32(bytes, 20, 0xFFFFFFFFu);
    StackDocument output;
    std::vector<StackDiagnostic> diagnostics;
    EXPECT_FALSE(ReadParticleStackBinary(bytes, output, diagnostics));
    ASSERT_FALSE(diagnostics.empty());
    EXPECT_NE(diagnostics.front().Message.find("phases"), std::string::npos) << Describe(diagnostics);
}

TEST(ParticleStack, ValidationRefusesAMeshIndexPastItsVertices)
{
    auto document = EmptyStack(1.0f);
    auto mesh = Processor(ParticleShapeProcessor(), ParticleShapeParameters{.Shape = ParticleShapeKind::MeshSurface});
    mesh.Geometry.Vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    mesh.Geometry.Indices = {0, 1, 3};
    Append(document, std::move(mesh));
    std::vector<StackDiagnostic> diagnostics;
    EXPECT_FALSE(ValidateStack(document, diagnostics));
    ASSERT_FALSE(diagnostics.empty());
    EXPECT_EQ(diagnostics.front().Message, "A geometry index is out of range");
    std::vector<uint8> bytes;
    std::string error;
    EXPECT_FALSE(WriteParticleStackBinary(document, bytes, error));
    EXPECT_TRUE(bytes.empty());
}

TEST(ParticleStack, UnknownTypesParametersAndKeysAreRefused)
{
    const std::string prefix = R"({"version":1,"entryPhase":1,"phases":[{"id":1,"label":"Spawn","processors":[)";
    const std::string suffix = "]}]}";
    for (const char* processor : {R"({"type":"unheardOf","id":2})", R"({"type":"drag","id":2,"parameters":{"friction":1}})",
                                  R"({"type":"drag","id":2,"colour":1})", R"({"type":"drag","id":2,"stage":"sometimes"})",
                                  R"({"type":"drag","id":2,"parameters":{"drag":{"mode":"sometimes","value":1}}})"})
    {
        StackDocument output;
        std::vector<StackDiagnostic> diagnostics;
        EXPECT_FALSE(ParseParticleStack(prefix + processor + suffix, output, diagnostics)) << processor;
        EXPECT_FALSE(diagnostics.empty()) << processor;
    }
    StackDocument output;
    std::vector<StackDiagnostic> diagnostics;
    const std::string accepted = R"({"type":"drag","id":2,"parameters":{"drag":{"mode":"constant","value":2}}})";
    EXPECT_TRUE(ParseParticleStack(prefix + accepted + suffix, output, diagnostics)) << Describe(diagnostics);
}

TEST(ParticleStack, DeeplyNestedDocumentIsRefusedWithoutExhaustingTheStack)
{
    // An unknown field nested far past any stack document's depth, in a document of a size a stack
    // file easily reaches: copying or walking it recursively overflows the thread's stack.
    const std::string prefix = R"({"version":1,"entryPhase":1,"phases":[{"id":1,"processors":[]}],"extra":)";
    const std::string suffix = "}";
    constexpr size_t kDocumentBytes = 32 * 1024;
    const size_t depth = (kDocumentBytes - prefix.size() - suffix.size()) / 2;
    const std::string source = prefix + std::string(depth, '[') + std::string(depth, ']') + suffix;
    ASSERT_LE(source.size(), kDocumentBytes);
    StackDocument output = MakeDefaultStack();
    const auto before = SerializeParticleStack(output, -1);
    std::vector<StackDiagnostic> diagnostics;
    EXPECT_FALSE(ParseParticleStack(source, output, diagnostics));
    ASSERT_FALSE(diagnostics.empty());
    EXPECT_NE(diagnostics.front().Message.find("nested"), std::string::npos) << diagnostics.front().Message;
    EXPECT_EQ(SerializeParticleStack(output, -1), before);
}

TEST(ParticleStack, MalformedInputLeavesTheOutputUntouched)
{
    StackDocument output = MakeDefaultStack();
    const auto before = SerializeParticleStack(output, -1);
    for (const char* source : {"", "{", "[]", R"({"version":1})", R"({"version":1,"entryPhase":1,"phases":[]})",
                               R"({"version":1,"entryPhase":9,"phases":[{"id":1,"processors":[]}]})"})
    {
        std::vector<StackDiagnostic> diagnostics;
        EXPECT_FALSE(ParseParticleStack(source, output, diagnostics)) << source;
        EXPECT_FALSE(diagnostics.empty()) << source;
        EXPECT_EQ(SerializeParticleStack(output, -1), before) << source;
    }
}

TEST(ParticleStack, ValidationRefusesStagesAndRulesAProcessorCannotRunWith)
{
    EXPECT_FALSE(IsValid([](StackDocument& document)
                         { Append(document, Processor(ParticleShapeProcessor(), ParticleShapeParameters{}, ParticleStage::Update)); }));
    EXPECT_FALSE(IsValid(
        [](StackDocument& document)
        {
            Append(document, Processor(ParticlePropertyProcessor(),
                                       Scalar(ParticleAttribute::Lifetime, ParticleOperation::Set,
                                              ParticleBasis::Current, 2.0f),
                                       ParticleStage::Update));
        }));
    EXPECT_FALSE(IsValid(
        [](StackDocument& document)
        {
            Append(document, Processor(ParticleEventProcessor(),
                                       ParticleEventParameters{.Trigger = ParticleEventTrigger::Exit, .Destination = 1}));
        }));
    EXPECT_FALSE(IsValid(
        [](StackDocument& document)
        {
            Append(document, Processor(ParticleEventProcessor(),
                                       ParticleEventParameters{.Trigger = ParticleEventTrigger::Birth, .Destination = 99}));
        }));
    EXPECT_FALSE(IsValid([](StackDocument& document)
                         { Append(document, Processor(ParticleNoiseProcessor(), ParticleNoiseParameters{.Octaves = 0})); }));
    EXPECT_FALSE(IsValidDrag(-1.0f));
    EXPECT_FALSE(IsValidDrag(std::numeric_limits<float>::quiet_NaN()));
    EXPECT_TRUE(IsValidDrag(1.0f));
}

TEST(ParticleStack, CurvesNeedOrderedFiniteKeys)
{
    EXPECT_TRUE(IsValidDrag(ParticleValue::MakeCurve({{0.0f, 1.0f}, {1.0f, 0.0f}})));
    ParticleValue empty;
    empty.Mode = ParticleValueMode::Curve;
    EXPECT_FALSE(IsValidDrag(empty));
    auto unordered = ParticleValue::MakeCurve({{0.0f, 1.0f}, {1.0f, 0.0f}});
    unordered.MinimumCurve.Keys[1].Time = -1.0f;
    EXPECT_FALSE(IsValidDrag(unordered));
    auto infinite = ParticleValue::MakeCurve({{0.0f, 1.0f}, {1.0f, 0.0f}});
    infinite.MinimumCurve.Keys[0].Value = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(IsValidDrag(infinite));
}

TEST(ParticleStack, AnInvalidDocumentCooksToNothing)
{
    auto invalid = EmptyStack(1.0f);
    Append(invalid, Processor(ParticleShapeProcessor(), ParticleShapeParameters{}, ParticleStage::Update));
    std::vector<uint8> bytes{1, 2, 3};
    std::string error;
    EXPECT_FALSE(WriteParticleStackBinary(invalid, bytes, error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(bytes, (std::vector<uint8>{1, 2, 3}));
}

TEST(ParticleStack, ProcessorsOfAStageRunInTheirOrder)
{
    EXPECT_FLOAT_EQ(SizeAfterBirth(true), 6.0f);
    EXPECT_FLOAT_EQ(SizeAfterBirth(false), 2.0f);
}

TEST(ParticleStack, RandomPicksAreStablePerParticleAndIndependentOfOtherProcessors)
{
    auto document = EmptyStack(5.0f);
    Append(document, Processor(ParticleEmitBurstProcessor(), ParticleEmitBurstParameters{.Count = 32}));
    Append(document, Processor(ParticlePropertyProcessor(),
                               Scalar(ParticleAttribute::Size, ParticleOperation::Set, ParticleBasis::Current,
                                      ParticleValue{1.0f, 5.0f}),
                               ParticleStage::Birth));
    // Varies per particle but not per tick: it multiplies the birth size, never the current one.
    Append(document, Processor(ParticlePropertyProcessor(),
                               Scalar(ParticleAttribute::Size, ParticleOperation::Multiply, ParticleBasis::Birth,
                                      ParticleValue{0.5f, 1.5f})));
    // A random curve input gives every component of one processor the same pick.
    ParticlePropertyParameters tint{.Target = ParticleAttribute::Color, .Operation = ParticleOperation::Set, .Input = {.Driver = ParticleDriver::Random}};
    for (uint32 component = 0; component < 3; ++component)
        tint.Value[component] = ParticleValue::MakeCurve({{0.0f, 0.0f}, {1.0f, 1.0f}});
    Append(document, Processor(ParticlePropertyProcessor(), tint, ParticleStage::Birth));

    auto reordered = document;
    auto spin = Processor(ParticlePropertyProcessor(),
                          Scalar(ParticleAttribute::Spin, ParticleOperation::Set, ParticleBasis::Current,
                                 ParticleValue{-2.0f, 2.0f}),
                          ParticleStage::Birth);
    const uint32 phase = reordered.Phases[0].Id;
    const uint32 first = reordered.Phases[0].Processors.front().Id;
    uint32 spinId = 0;
    std::string error;
    ASSERT_TRUE(AddProcessor(reordered, phase, std::move(spin), spinId, error)) << error;
    ASSERT_TRUE(ReorderProcessor(reordered, phase, spinId, first, false));
    ASSERT_EQ(reordered.Phases[0].Processors.front().Id, spinId);

    ParticleRuntime runtime;
    ParticleRuntime other;
    runtime.Bind(Compile(document), 64);
    other.Bind(Compile(reordered), 64);
    AdvanceFrames(runtime, Input(), 10);
    AdvanceFrames(other, Input(), 10);
    const auto early = Snapshot(runtime);
    ExpectSameParticles(Snapshot(other), early);
    AdvanceFrames(runtime, Input(), 20);
    const auto later = Snapshot(runtime);
    ASSERT_EQ(later.size(), early.size());
    float smallest = std::numeric_limits<float>::max();
    float largest = 0.0f;
    for (size_t index = 0; index < later.size(); ++index)
    {
        EXPECT_FLOAT_EQ(later[index].Size, early[index].Size);
        smallest = std::min(smallest, later[index].Size);
        largest = std::max(largest, later[index].Size);
    }
    EXPECT_GE(smallest, 0.5f);
    EXPECT_LE(largest, 7.5f);
    EXPECT_GT(largest - smallest, 1.0f);
    const auto colors = runtime.Channels().Colors();
    for (uint32 index = 0; index < runtime.Count(); ++index)
    {
        EXPECT_FLOAT_EQ(colors[index].x, colors[index].y);
        EXPECT_FLOAT_EQ(colors[index].y, colors[index].z);
    }
}

TEST(ParticleStack, TransitionsKeepIdentityAndAgeAndStartFromTheEntryValue)
{
    auto document = EmptyStack(5.0f);
    document.Phases.push_back(StackPhase{.Id = 10, .Label = "Second"});
    Append(document, Processor(ParticleEmitBurstProcessor(), ParticleEmitBurstParameters{.Count = 3}));
    Append(document, Processor(ParticlePropertyProcessor(),
                               Scalar(ParticleAttribute::Size, ParticleOperation::Set, ParticleBasis::Current, 2.0f),
                               ParticleStage::Birth));
    Append(document, Processor(ParticleEventProcessor(),
                               ParticleEventParameters{.Trigger = ParticleEventTrigger::PhaseAge, .Seconds = 0.5f, .Destination = 10}));
    // On entry the size triples from what it was; every update then halves the entry size once.
    Append(document,
           Processor(ParticlePropertyProcessor(),
                     Scalar(ParticleAttribute::Size, ParticleOperation::Multiply, ParticleBasis::Entry, 3.0f),
                     ParticleStage::Enter),
           10);
    Append(document,
           Processor(ParticlePropertyProcessor(),
                     Scalar(ParticleAttribute::Size, ParticleOperation::Multiply, ParticleBasis::Entry, 0.5f)),
           10);
    const auto stack = Compile(document);
    ParticleRuntime runtime;
    runtime.Bind(stack, 8);
    AdvanceFrames(runtime, Input(), 45);
    ASSERT_EQ(runtime.Count(), 3u);
    const auto& channels = runtime.Channels();
    std::vector<uint32> spawns;
    for (uint32 index = 0; index < runtime.Count(); ++index)
    {
        EXPECT_EQ(channels.Phases()[index], stack->PhaseIndex(10));
        EXPECT_NEAR(channels.Ages()[index], 44.0f / 60.0f, 1e-4f);
        EXPECT_NEAR(channels.PhaseAges()[index], 14.0f / 60.0f, 1e-4f);
        EXPECT_FLOAT_EQ(channels.Sizes()[index], 3.0f);
        spawns.push_back(channels.SpawnIndices()[index]);
    }
    std::sort(spawns.begin(), spawns.end());
    EXPECT_EQ(spawns, (std::vector<uint32>{0, 1, 2}));
}

TEST(ParticleStack, TransitionCyclesAreCut)
{
    auto document = EmptyStack(5.0f);
    document.Phases.push_back(StackPhase{.Id = 10, .Label = "Back"});
    Append(document, Processor(ParticleEmitBurstProcessor(), ParticleEmitBurstParameters{.Count = 2}));
    Append(document, Processor(ParticleEventProcessor(),
                               ParticleEventParameters{.Trigger = ParticleEventTrigger::Enter, .Destination = 10}));
    Append(document,
           Processor(ParticleEventProcessor(),
                     ParticleEventParameters{.Trigger = ParticleEventTrigger::Enter, .Destination = 1}),
           10);
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 8);
    AdvanceFrames(runtime, Input(), 3);
    EXPECT_EQ(runtime.Count(), 0u);
}

TEST(ParticleStack, ALightIsClearedWhenItsIntervalEnds)
{
    auto document = EmptyStack(5.0f);
    Append(document, Processor(ParticleEmitBurstProcessor(), ParticleEmitBurstParameters{.Count = 1}));
    auto light = Processor(ParticleLightProcessor(), ParticleLightParameters{.Intensity = 2.0f, .Range = 3.0f});
    light.End = 0.5f;
    Append(document, std::move(light));
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 4);
    AdvanceFrames(runtime, Input(), 10);
    ASSERT_EQ(runtime.Count(), 1u);
    EXPECT_FLOAT_EQ(runtime.Channels().Get<float>(ParticleChannel::LightIntensity)[0], 2.0f);
    EXPECT_FLOAT_EQ(runtime.Channels().Get<float>(ParticleChannel::LightRange)[0], 3.0f);
    AdvanceFrames(runtime, Input(), 50);
    ASSERT_EQ(runtime.Count(), 1u);
    EXPECT_FLOAT_EQ(runtime.Channels().Get<float>(ParticleChannel::LightIntensity)[0], 0.0f);
    EXPECT_FLOAT_EQ(runtime.Channels().Get<float>(ParticleChannel::LightRange)[0], 0.0f);
}

TEST(ParticleStack, MeshSurfaceSamplingIsAreaWeightedAndDeterministic)
{
    auto document = EmptyStack(5.0f);
    Append(document, Processor(ParticleEmitBurstProcessor(), ParticleEmitBurstParameters{.Count = 4000}));
    auto mesh = Processor(ParticleShapeProcessor(), ParticleShapeParameters{.Shape = ParticleShapeKind::MeshSurface});
    mesh.Geometry = TwoTriangles();
    Append(document, std::move(mesh));
    const auto stack = Compile(document);
    ParticleRuntime runtime;
    ParticleRuntime again;
    runtime.Bind(stack, 4096);
    again.Bind(stack, 4096);
    runtime.Advance(Input(), kFrame);
    again.Advance(Input(), kFrame);
    ASSERT_EQ(runtime.Count(), 4000u);
    ExpectSameParticles(runtime, again);
    uint32 onLarger = 0;
    for (uint32 index = 0; index < runtime.Count(); ++index)
    {
        const Vector3 position = runtime.Channels().Positions()[index];
        EXPECT_FLOAT_EQ(position.z, 0.0f);
        onLarger += position.x >= 10.0f ? 1u : 0u;
    }
    EXPECT_NEAR(static_cast<float>(onLarger) / 4000.0f, 0.75f, 0.03f);
}

TEST(ParticleStack, ShapeOffsetsFollowTheEmitterRotation)
{
    auto document = EmptyStack(5.0f);
    Append(document, Processor(ParticleEmitBurstProcessor(), ParticleEmitBurstParameters{.Count = 4}));
    Append(document, Processor(ParticleShapeProcessor(),
                               ParticleShapeParameters{.Radius = 0.0f, .Offset = {1.0f, 0.0f, 0.0f}}));
    // Columns: the emitter's X axis points along world -Z; the emitter sits at (5, 0, 0).
    const float transform[16] = {0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 5, 0, 0, 1};
    auto input = Input();
    input.Transform = transform;
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 8);
    runtime.Advance(input, kFrame);
    ASSERT_EQ(runtime.Count(), 4u);
    for (uint32 index = 0; index < runtime.Count(); ++index)
    {
        const Vector3 position = runtime.Channels().Positions()[index];
        EXPECT_NEAR(position.x, 5.0f, 1e-5f);
        EXPECT_NEAR(position.y, 0.0f, 1e-5f);
        EXPECT_NEAR(position.z, -1.0f, 1e-5f);
    }
}

TEST(ParticleStack, EmissionHonoursItsIntervalAndGameEventsCarryInheritance)
{
    auto document = EmptyStack(10.0f);
    auto rate = Processor(ParticleEmitRateProcessor(), ParticleEmitRateParameters{.Rate = 60.0f});
    rate.Start = 1.0f;
    rate.End = 2.0f;
    Append(document, std::move(rate));
    Append(document, Processor(ParticlePropertyProcessor(), SetVector(ParticleAttribute::Velocity, {0, 2, 0}),
                               ParticleStage::Birth));
    ParticleEventParameters flash{.Trigger = ParticleEventTrigger::External, .Action = ParticleEventAction::Emit, .Count = 2, .Inherit = kParticleInheritColor | kParticleInheritVelocity, .VelocityScale = 0.5f};
    std::strcpy(flash.EventName, "Flash");
    flash.SubEmitter = 2;
    Append(document, Processor(ParticleEventProcessor(), flash));
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 256);
    AdvanceFrames(runtime, Input(), 60);
    EXPECT_EQ(runtime.Count(), 0u);
    AdvanceFrames(runtime, Input(), 60);
    const uint32 emitted = runtime.Count();
    EXPECT_NEAR(static_cast<float>(emitted), 60.0f, 1.0f);
    AdvanceFrames(runtime, Input(), 60);
    EXPECT_EQ(runtime.Count(), emitted);

    EXPECT_FALSE(runtime.SendEvent(0));
    ASSERT_TRUE(runtime.SendEvent("Flash"_sid));
    runtime.Advance(Input(), kFrame);
    ASSERT_EQ(runtime.SpawnEvents().size(), emitted);
    for (const auto& event : runtime.SpawnEvents())
    {
        EXPECT_EQ(event.Trigger, ParticleEventTrigger::External);
        EXPECT_EQ(event.Count, 2u);
        EXPECT_EQ(event.Inherit, kParticleInheritColor | kParticleInheritVelocity);
        EXPECT_NEAR(event.Velocity.y, 1.0f, 1e-5f);
        EXPECT_EQ(event.SubEmitter, 2u);
    }
    ASSERT_TRUE(runtime.SendEvent("Flash"_sid, 3));
    ASSERT_TRUE(runtime.SendEvent("Other"_sid));
    runtime.Advance(Input(), kFrame);
    ASSERT_EQ(runtime.SpawnEvents().size(), 1u);
    EXPECT_EQ(runtime.SpawnEvents().front().ParticleId, 3u);
}

namespace
{
// How far one tick of displacement noise moves a particle, on average over a cloud spread across
// many wavelengths of the field.
float MeanNoiseStep(ParticleNoiseAlgorithm algorithm, float wavelength)
{
    constexpr uint32 kCloud = 512;
    auto document = EmptyStack(10.0f);
    Append(document, Processor(ParticleEmitBurstProcessor(), ParticleEmitBurstParameters{.Count = kCloud}));
    Append(document, Processor(ParticleShapeProcessor(), ParticleShapeParameters{.Radius = 20.0f}));
    ParticleNoiseParameters noise{.Mode = ParticleNoiseMode::Displacement, .Algorithm = algorithm};
    noise.Wavelength = ParticleValue{wavelength};
    Append(document, Processor(ParticleNoiseProcessor(), noise));
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), kCloud);
    runtime.Advance(Input(), kFrame);
    const auto before = std::vector<Vector3>(runtime.Channels().Positions().begin(), runtime.Channels().Positions().end());
    runtime.Advance(Input(), kFrame);
    const auto after = runtime.Channels().Positions();
    EXPECT_EQ(after.size(), before.size());
    float total = 0.0f;
    for (size_t index = 0; index < before.size() && index < after.size(); ++index)
        total += (after[index] - before[index]).Length();
    return total / static_cast<float>(std::max<size_t>(before.size(), 1));
}
} // namespace

// A shorter wavelength makes the field finer, not stronger: both fields keep their strength when the
// wavelength shrinks sixteen times.
TEST(ParticleStack, NoiseStrengthDoesNotGrowAsTheWavelengthShrinks)
{
    for (const auto algorithm : {ParticleNoiseAlgorithm::Analytic, ParticleNoiseAlgorithm::Gradient})
    {
        const float wide = MeanNoiseStep(algorithm, 4.0f);
        const float fine = MeanNoiseStep(algorithm, 0.25f);
        ASSERT_GT(wide, 0.0f) << static_cast<int>(algorithm);
        EXPECT_GT(fine / wide, 0.5f) << static_cast<int>(algorithm);
        EXPECT_LT(fine / wide, 2.0f) << static_cast<int>(algorithm);
    }
}

TEST(ParticleStack, DistanceEventsLandAtEverySpacingAcrossTicks)
{
    auto document = EmptyStack(10.0f);
    Append(document, Processor(ParticleEmitBurstProcessor(), ParticleEmitBurstParameters{.Count = 1}));
    Append(document, Processor(ParticlePropertyProcessor(), SetVector(ParticleAttribute::Velocity, {3, 0, 0}),
                               ParticleStage::Birth));
    ParticleEventParameters marks{.Trigger = ParticleEventTrigger::Distance, .Action = ParticleEventAction::Emit, .Spacing = 0.2f};
    Append(document, Processor(ParticleEventProcessor(), marks));
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 4);
    std::vector<float> landed;
    for (int frame = 0; frame < 120; ++frame)
    {
        runtime.Advance(Input(), kFrame);
        for (const auto& event : runtime.SpawnEvents())
            landed.push_back(event.Position.x);
    }
    // 119 ticks of 0.05 units.
    ASSERT_EQ(landed.size(), 29u);
    for (size_t index = 0; index < landed.size(); ++index)
        EXPECT_NEAR(landed[index], 0.2f * static_cast<float>(index + 1), 1e-4f);
}

TEST(ParticleStack, ParameterEditsRebindWithoutRestarting)
{
    const auto document = Fountain();
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 512);
    AdvanceFrames(runtime, Input(), 30);
    const uint32 live = runtime.Count();
    ASSERT_GT(live, 0u);

    auto edited = document;
    for (auto& processor : edited.Phases[0].Processors)
        if (processor.Descriptor == &ParticleDragProcessor())
            processor.Params<ParticleDragParameters>().Drag = 0.9f;
    ASSERT_TRUE(HaveSameStructure(document, edited));
    const auto editedStack = Compile(edited);
    EXPECT_TRUE(runtime.Rebind(editedStack));
    EXPECT_EQ(runtime.Stack(), editedStack.get());
    EXPECT_EQ(runtime.Count(), live);

    auto extended = document;
    Append(extended, Processor(ParticleTrailProcessor(), ParticleTrailParameters{}));
    EXPECT_FALSE(HaveSameStructure(document, extended));
    EXPECT_FALSE(runtime.Rebind(Compile(extended)));
    EXPECT_EQ(runtime.Stack(), editedStack.get());
    EXPECT_EQ(runtime.Count(), live);
}

TEST(ParticleProcessorRegistry, AProcessorRegisteredOutsideTheModuleRunsInAStack)
{
    const auto& spin = RegisterSpinProcessor();
    EXPECT_EQ(ParticleProcessorRegistry::Find("test.spin"), &spin);
    bool listed = false;
    ParticleProcessorRegistry::ForEach([&listed, &spin](const ParticleProcessorDescriptor& descriptor)
                                       { listed |= &descriptor == &spin; });
    EXPECT_TRUE(listed);

    const std::string_view source =
        R"({"version":1,"entryPhase":1,"lifetime":5,"phases":[{"id":1,"label":"Spawn","processors":[)"
        R"({"type":"emitBurst","id":2,"stage":"emission","parameters":{"count":4}},)"
        R"({"type":"test.spin","id":3,"stage":"update","parameters":{"rate":3.5}}]}]})";
    StackDocument document;
    std::vector<StackDiagnostic> diagnostics;
    ASSERT_TRUE(ParseParticleStack(source, document, diagnostics)) << Describe(diagnostics);
    EXPECT_NE(SerializeParticleStack(document, -1).find(R"("rate":3.5)"), std::string::npos);
    StackDocument cooked;
    ASSERT_TRUE(ReadParticleStackBinary(Cook(document), cooked, diagnostics)) << Describe(diagnostics);
    ParticleRuntime runtime;
    runtime.Bind(Compile(cooked), 16);
    AdvanceFrames(runtime, Input(), 3);
    ASSERT_EQ(runtime.Count(), 4u);
    for (uint32 index = 0; index < runtime.Count(); ++index)
    {
        EXPECT_FLOAT_EQ(runtime.Channels().Spins()[index], 3.5f);
        EXPECT_GT(runtime.Channels().Rotations()[index], 0.0f);
    }
}

TEST(ParticleStack, ScaleAxesStayIndependent)
{
    auto document = EmptyStack(10.0f);
    Append(document, Processor(ParticleEmitBurstProcessor(), ParticleEmitBurstParameters{.Count = 1}));
    Append(document, Processor(ParticlePropertyProcessor(), SetVector(ParticleAttribute::Scale, {2, 5, 1}),
                               ParticleStage::Birth));
    ParticlePropertyParameters stretch{.Target = ParticleAttribute::Scale, .Operation = ParticleOperation::Multiply, .Basis = ParticleBasis::Birth};
    stretch.Value[0] = 0.5f;
    stretch.Value[1] = 2.0f;
    stretch.Value[2] = 1.0f;
    Append(document, Processor(ParticlePropertyProcessor(), stretch));
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 4);
    runtime.Advance(Input(), kFrame);
    ASSERT_EQ(runtime.Count(), 1u);
    // The first frame already holds the stretched birth scale.
    EXPECT_FLOAT_EQ(runtime.Channels().Scales()[0].x, 1.0f);
    EXPECT_FLOAT_EQ(runtime.Channels().Scales()[0].y, 10.0f);
    AdvanceFrames(runtime, Input(), 2);
    EXPECT_FLOAT_EQ(runtime.Channels().Scales()[0].x, 1.0f);
    EXPECT_FLOAT_EQ(runtime.Channels().Scales()[0].y, 10.0f);
    EXPECT_FLOAT_EQ(runtime.Channels().Scales()[0].z, 1.0f);
}

TEST(ParticleStack, AChildKeepsWhatItInheritsOverItsBirthDefaults)
{
    auto document = EmptyStack(10.0f);
    Append(document, Processor(ParticlePropertyProcessor(),
                               Scalar(ParticleAttribute::Size, ParticleOperation::Set, ParticleBasis::Current, 1.75f),
                               ParticleStage::Birth));
    ParticlePropertyParameters white{.Target = ParticleAttribute::Color, .Operation = ParticleOperation::Set};
    Append(document, Processor(ParticlePropertyProcessor(), white, ParticleStage::Birth));
    // Scales from the birth size, which is what the child inherited.
    Append(document, Processor(ParticlePropertyProcessor(),
                               Scalar(ParticleAttribute::Size, ParticleOperation::Multiply, ParticleBasis::Birth, 2.0f)));
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 4);
    ParticleSpawnRequest parent;
    parent.Inherit = kParticleInheritSize | kParticleInheritColor;
    parent.Size = 0.35f;
    parent.Color = {0.2f, 0.7f, 0.1f, 0.8f};
    ASSERT_TRUE(runtime.QueueSpawn(parent));
    runtime.Advance(Input(), kFrame);
    ASSERT_EQ(runtime.Count(), 1u);
    // The first frame already holds the doubled inherited size, not the doubled default of 1.75.
    EXPECT_FLOAT_EQ(runtime.Channels().Sizes()[0], 0.7f);
    EXPECT_FLOAT_EQ(runtime.Channels().Colors()[0].y, 0.7f);
    EXPECT_FLOAT_EQ(runtime.Channels().Colors()[0].w, 0.8f);
    runtime.Advance(Input(), kFrame);
    EXPECT_FLOAT_EQ(runtime.Channels().Sizes()[0], 0.7f);
}

TEST(ParticleStack, AVelocityConeLaunchesWithinItsSpreadAtItsSpeed)
{
    auto document = EmptyStack(5.0f);
    Append(document, Processor(ParticleEmitBurstProcessor(), ParticleEmitBurstParameters{.Count = 2000}));
    Append(document, Processor(ParticleVelocityConeProcessor(),
                               ParticleVelocityConeParameters{.Spread = 30.0f, .Speed = {2.0f, 3.0f}}));
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 2048);
    runtime.Advance(Input(), kFrame);
    ASSERT_EQ(runtime.Count(), 2000u);
    const float minimumCosine = std::cos(30.0f * 3.14159265f / 180.0f);
    double cosineSum = 0.0;
    for (uint32 index = 0; index < runtime.Count(); ++index)
    {
        const Vector3 velocity = runtime.Channels().Velocities()[index];
        const float speed = velocity.Length();
        EXPECT_GE(speed, 2.0f - 1e-4f);
        EXPECT_LE(speed, 3.0f + 1e-4f);
        const float cosine = velocity.y / speed;
        EXPECT_GE(cosine, minimumCosine - 1e-4f);
        cosineSum += cosine;
    }
    // Uniform over the cap: the polar cosine is uniform in [cos(spread), 1].
    EXPECT_NEAR(cosineSum / runtime.Count(), (1.0 + minimumCosine) * 0.5, 0.005);
}

TEST(ParticleStack, AVelocityConeTurnsWithTheEmitterOnlyInLocalSpace)
{
    const auto launchAlong = [](ParticleSpace space)
    {
        auto document = EmptyStack(5.0f);
        Append(document, Processor(ParticleEmitBurstProcessor(), ParticleEmitBurstParameters{.Count = 1}));
        Append(document, Processor(ParticleVelocityConeProcessor(),
                                   ParticleVelocityConeParameters{.Space = space, .Spread = 0.0f, .Speed = 2.0f}));
        // Columns: the emitter's +Y axis points along world -X.
        const float transform[16] = {0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        auto input = Input();
        input.Transform = transform;
        ParticleRuntime runtime;
        runtime.Bind(Compile(document), 4);
        runtime.Advance(input, kFrame);
        EXPECT_EQ(runtime.Count(), 1u);
        return runtime.Count() > 0 ? runtime.Channels().Velocities()[0] : Vector3{};
    };
    const Vector3 local = launchAlong(ParticleSpace::Local);
    EXPECT_NEAR(local.x, -2.0f, 1e-5f);
    EXPECT_NEAR(local.y, 0.0f, 1e-5f);
    const Vector3 world = launchAlong(ParticleSpace::World);
    EXPECT_NEAR(world.x, 0.0f, 1e-5f);
    EXPECT_NEAR(world.y, 2.0f, 1e-5f);
}

TEST(ParticleStack, AVelocityConeAddsItsShareOfTheEmitterVelocity)
{
    auto document = EmptyStack(5.0f);
    Append(document, Processor(ParticleEmitRateProcessor(), ParticleEmitRateParameters{.Rate = 60.0f}));
    Append(document, Processor(ParticleVelocityConeProcessor(),
                               ParticleVelocityConeParameters{.Spread = 0.0f, .Speed = 0.0f, .InheritEmitterVelocity = 0.5f}));
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 256);
    float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    auto input = Input();
    input.Transform = transform;
    // The emitter moves along X at 6 units per second.
    for (int frame = 0; frame < 30; ++frame)
    {
        transform[12] = 0.1f * static_cast<float>(frame);
        runtime.Advance(input, kFrame);
    }
    ASSERT_GT(runtime.Count(), 10u);
    uint32 newest = 0;
    for (uint32 index = 1; index < runtime.Count(); ++index)
        if (runtime.Channels().SpawnIndices()[index] > runtime.Channels().SpawnIndices()[newest])
            newest = index;
    EXPECT_NEAR(runtime.Channels().Velocities()[newest].x, 3.0f, 1e-3f);
}

TEST(ParticleStack, AnOrbitAcceleratesAroundAndAwayFromItsAxis)
{
    auto document = EmptyStack(5.0f);
    Append(document, Processor(ParticleEmitBurstProcessor(), ParticleEmitBurstParameters{.Count = 1}));
    Append(document, Processor(ParticlePropertyProcessor(), SetVector(ParticleAttribute::Position, {2, 5, 0}),
                               ParticleStage::Birth));
    Append(document, Processor(ParticleOrbitProcessor(), ParticleOrbitParameters{.Radial = 1.0f, .Tangential = 3.0f}));
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 4);
    AdvanceFrames(runtime, Input(), 2);
    ASSERT_EQ(runtime.Count(), 1u);
    // One tick at (2, 5, 0): outward is +X, and Y cross X is -Z.
    const Vector3 velocity = runtime.Channels().Velocities()[0];
    EXPECT_NEAR(velocity.x, 1.0f / 60.0f, 1e-5f);
    EXPECT_NEAR(velocity.y, 0.0f, 1e-6f);
    EXPECT_NEAR(velocity.z, -3.0f / 60.0f, 1e-5f);
}

TEST(ParticleStack, LinearDragStopsAParticleWhereExponentialDragOnlySlowsIt)
{
    const auto speedAfterOneSecond = [](ParticleDragMode mode)
    {
        auto document = EmptyStack(5.0f);
        Append(document, Processor(ParticleEmitBurstProcessor(), ParticleEmitBurstParameters{.Count = 1}));
        Append(document, Processor(ParticlePropertyProcessor(), SetVector(ParticleAttribute::Velocity, {1, 0, 0}),
                                   ParticleStage::Birth));
        Append(document, Processor(ParticleDragProcessor(), ParticleDragParameters{.Mode = mode, .Drag = 2.0f}));
        ParticleRuntime runtime;
        runtime.Bind(Compile(document), 4);
        AdvanceFrames(runtime, Input(), 61);
        EXPECT_EQ(runtime.Count(), 1u);
        return runtime.Count() > 0 ? runtime.Channels().Velocities()[0].Length() : -1.0f;
    };
    EXPECT_FLOAT_EQ(speedAfterOneSecond(ParticleDragMode::Linear), 0.0f);
    EXPECT_NEAR(speedAfterOneSecond(ParticleDragMode::Exponential), std::exp(-2.0f), 1e-4f);
}

TEST(ParticleStack, ATwoDimensionalEmitterKeepsItsShapesOnItsPlane)
{
    auto document = EmptyStack(5.0f);
    Append(document, Processor(ParticleEmitBurstProcessor(), ParticleEmitBurstParameters{.Count = 200}));
    Append(document, Processor(ParticleShapeProcessor(), ParticleShapeParameters{.Radius = 2.0f}));
    auto input = Input();
    input.Planar = true;
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 256);
    runtime.Advance(input, kFrame);
    ASSERT_EQ(runtime.Count(), 200u);
    float spread = 0.0f;
    for (uint32 index = 0; index < runtime.Count(); ++index)
    {
        EXPECT_FLOAT_EQ(runtime.Channels().Positions()[index].z, 0.0f);
        spread = std::max(spread, std::fabs(runtime.Channels().Positions()[index].x));
    }
    EXPECT_GT(spread, 0.5f);
}

TEST(ParticleStack, TheDefaultStackLaunchesParticlesUpwards)
{
    ParticleRuntime runtime;
    runtime.Bind(Compile(MakeDefaultStack()), Components::ParticleEmitter3D{}.Amount);
    AdvanceFrames(runtime, Input(), 30);
    ASSERT_GT(runtime.Count(), 0u);
    // The newest particle has had no tick of gravity yet: it leaves within 25 degrees of +Y.
    uint32 newest = 0;
    for (uint32 index = 1; index < runtime.Count(); ++index)
        if (runtime.Channels().SpawnIndices()[index] > runtime.Channels().SpawnIndices()[newest])
            newest = index;
    const Vector3 velocity = runtime.Channels().Velocities()[newest];
    EXPECT_GE(velocity.y, std::cos(25.0f * 3.14159265f / 180.0f) * velocity.Length() - 1e-4f);
    EXPECT_GE(velocity.Length(), 1.0f - 1e-4f);
}

namespace
{
constexpr std::string_view kReloadModule = "particles.reload-test";

// Stamps the registrations made while it lives with a load of kReloadModule, as the module loader does.
struct ModuleLoadScope
{
    explicit ModuleLoadScope(uint64 generation) { ECS::SetActiveRegistrationModule(kReloadModule, generation); }
    ~ModuleLoadScope() { ECS::ClearActiveRegistrationModule(); }
    ModuleLoadScope(const ModuleLoadScope&) = delete;
    ModuleLoadScope& operator=(const ModuleLoadScope&) = delete;
};

// The code each load of the module ships for the same processor: it spins particles at its generation.
void SpinAtGenerationOne(ParticleProcessorContext& context)
{
    for (const uint32 index : context.Particles)
        context.Channels.Spins()[index] = 1.0f;
}

void SpinAtGenerationTwo(ParticleProcessorContext& context)
{
    for (const uint32 index : context.Particles)
        context.Channels.Spins()[index] = 2.0f;
}

void SpinAtGenerationThree(ParticleProcessorContext& context)
{
    for (const uint32 index : context.Particles)
        context.Channels.Spins()[index] = 3.0f;
}

ParticleProcessorDescriptor ReloadedSpin(std::string_view id, void (*execute)(ParticleProcessorContext&))
{
    ParticleProcessorDescriptor descriptor;
    descriptor.Id = id;
    descriptor.DisplayName = "Reloaded Spin";
    descriptor.Category = "Tests";
    descriptor.Stages = StageBit(ParticleStage::Update);
    descriptor.DefaultStage = ParticleStage::Update;
    descriptor.Writes = ChannelBits(ParticleChannel::Spin);
    descriptor.Execute = execute;
    BindParticleParameters<ParticleRuntimeTestTypes::SpinParameters>(descriptor, kSpinFields);
    return descriptor;
}

float SpinAfterATick(ParticleRuntime& runtime)
{
    runtime.Advance(Input(), kFrame);
    return runtime.Count() > 0 ? runtime.Channels().Spins()[0] : -1.0f;
}
} // namespace

// Stacks hold parameter bytes laid out for the struct an id was first registered with, so a second
// registration with another struct is refused and the first stays.
TEST(ParticleProcessorRegistry, ADifferentParameterStructUnderARegisteredIdIsRefused)
{
    const auto& spin = RegisterSpinProcessor();
    ParticleProcessorDescriptor gusty = spin;
    BindParticleParameters<ParticleRuntimeTestTypes::GustySpinParameters>(gusty, kGustySpinFields);
    EXPECT_FALSE(ParticleProcessorRegistry::Register(gusty));
    const auto* found = ParticleProcessorRegistry::Find("test.spin");
    ASSERT_EQ(found, &spin);
    EXPECT_EQ(found->ParameterSize, sizeof(ParticleRuntimeTestTypes::SpinParameters));
}

// A module reload re-registers its processor in place: stacks already holding it run the new code,
// an aborted load gives back what it replaced and drops what it added, and the images that served
// the processor stay mapped.
TEST(ParticleProcessorRegistry, AModuleReloadSwapsItsProcessorCodeInPlace)
{
    {
        ModuleLoadScope load(1);
        ASSERT_TRUE(ParticleProcessorRegistry::Register(ReloadedSpin("test.reload.spin", SpinAtGenerationOne)));
    }
    const auto* registered = ParticleProcessorRegistry::Find("test.reload.spin");
    ASSERT_NE(registered, nullptr);
    auto document = EmptyStack(10.0f);
    Append(document, Processor(ParticleEmitBurstProcessor(), ParticleEmitBurstParameters{.Count = 1}));
    Append(document, Processor(*registered, ParticleRuntimeTestTypes::SpinParameters{}));
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 4);
    runtime.Advance(Input(), kFrame); // the burst particle is born at the end of the first tick
    ASSERT_EQ(SpinAfterATick(runtime), 1.0f);

    {
        ModuleLoadScope reload(2);
        ASSERT_TRUE(ParticleProcessorRegistry::Register(ReloadedSpin("test.reload.spin", SpinAtGenerationTwo)));
    }
    EXPECT_EQ(ParticleProcessorRegistry::Find("test.reload.spin"), registered) << "stacks keep the pointer they hold";
    EXPECT_EQ(SpinAfterATick(runtime), 2.0f) << "the stack runs the reloaded code";
    EXPECT_EQ(ParticleProcessorRegistry::CountSupersededModuleProcessors(kReloadModule, 2), 1u)
        << "the stack was compiled while the first image served the processor";
    EXPECT_EQ(ParticleProcessorRegistry::CountSupersededModuleProcessors("particles.another-module", 2), 0u);

    {
        ModuleLoadScope aborted(3);
        ASSERT_TRUE(ParticleProcessorRegistry::Register(ReloadedSpin("test.reload.spin", SpinAtGenerationThree)));
        ASSERT_TRUE(ParticleProcessorRegistry::Register(ReloadedSpin("test.reload.added", SpinAtGenerationThree)));
    }
    EXPECT_EQ(ParticleProcessorRegistry::PurgeModuleProcessors(kReloadModule, 3), 2u);
    EXPECT_EQ(SpinAfterATick(runtime), 2.0f) << "the aborted load gives back the code it replaced";
    EXPECT_EQ(ParticleProcessorRegistry::Find("test.reload.added"), nullptr);
}

#if !defined(NDEBUG)
namespace
{
// Reads Custom0 without declaring it, so a stack holding only this processor allocates no storage for it.
void ReadUndeclaredCustom(ParticleProcessorContext& context)
{
    const auto custom = context.Channels.Get<float>(ParticleChannel::Custom0);
    for (const uint32 index : context.Particles)
        context.Channels.Spins()[index] = custom[index];
}

const ParticleProcessorDescriptor& RegisterUndeclaredReader()
{
    static const ParticleProcessorDescriptor* registered = []
    {
        ParticleProcessorDescriptor descriptor;
        descriptor.Id = "test.undeclared-reader";
        descriptor.DisplayName = "Undeclared Reader";
        descriptor.Category = "Tests";
        descriptor.Stages = StageBit(ParticleStage::Update);
        descriptor.DefaultStage = ParticleStage::Update;
        descriptor.Writes = ChannelBits(ParticleChannel::Spin);
        descriptor.Execute = ReadUndeclaredCustom;
        BindParticleParameters<ParticleRuntimeTestTypes::SpinParameters>(descriptor, kSpinFields);
        ParticleProcessorRegistry::Register(descriptor);
        return ParticleProcessorRegistry::Find("test.undeclared-reader");
    }();
    return *registered;
}

void RunTheUndeclaredReader()
{
    auto document = EmptyStack(10.0f);
    Append(document, Processor(ParticleEmitBurstProcessor(), ParticleEmitBurstParameters{.Count = 1}));
    Append(document, Processor(RegisterUndeclaredReader(), ParticleRuntimeTestTypes::SpinParameters{}));
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 4);
    AdvanceFrames(runtime, Input(), 2);
}
} // namespace

namespace
{
// Reads the Velocity channel, which holds Vector3 elements, as floats.
void ReadVelocityAsFloat(ParticleProcessorContext& context)
{
    const auto velocity = context.Channels.Get<float>(ParticleChannel::Velocity);
    for (const uint32 index : context.Particles)
        context.Channels.Spins()[index] = velocity[index];
}

const ParticleProcessorDescriptor& RegisterWrongTypeReader()
{
    static const ParticleProcessorDescriptor* registered = []
    {
        ParticleProcessorDescriptor descriptor;
        descriptor.Id = "test.wrong-type-reader";
        descriptor.DisplayName = "Wrong Type Reader";
        descriptor.Category = "Tests";
        descriptor.Stages = StageBit(ParticleStage::Update);
        descriptor.DefaultStage = ParticleStage::Update;
        descriptor.Reads = ChannelBits(ParticleChannel::Velocity);
        descriptor.Writes = ChannelBits(ParticleChannel::Spin);
        descriptor.Execute = ReadVelocityAsFloat;
        BindParticleParameters<ParticleRuntimeTestTypes::SpinParameters>(descriptor, kSpinFields);
        ParticleProcessorRegistry::Register(descriptor);
        return ParticleProcessorRegistry::Find("test.wrong-type-reader");
    }();
    return *registered;
}

void RunTheWrongTypeReader()
{
    auto document = EmptyStack(10.0f);
    Append(document, Processor(ParticleEmitBurstProcessor(), ParticleEmitBurstParameters{.Count = 1}));
    Append(document, Processor(RegisterWrongTypeReader(), ParticleRuntimeTestTypes::SpinParameters{}));
    ParticleRuntime runtime;
    runtime.Bind(Compile(document), 4);
    AdvanceFrames(runtime, Input(), 2);
}
} // namespace

// A declared channel read as another element type indexes past its elements; debug builds stop at
// the read and name the processor, the channel and both sizes.
TEST(ParticleChannelsDeathTest, ReadingAChannelAsTheWrongTypeStopsAtTheRead)
{
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    ASSERT_DEATH(RunTheWrongTypeReader(), "read a channel as the wrong element type");
}

// Without the check the read returns a span over no storage and indexes it; debug builds stop at
// the read instead and say which processor read which channel.
TEST(ParticleChannelsDeathTest, ReadingAChannelNoProcessorDeclaredStopsAtTheRead)
{
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    ASSERT_DEATH(RunTheUndeclaredReader(), "read a channel no processor of its stack declared");
}
#endif
