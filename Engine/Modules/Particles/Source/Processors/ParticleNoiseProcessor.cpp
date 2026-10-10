#include "Particles/Processors/ParticleNoiseProcessor.h"

#include "Noise/GradientCurl3D.h"
#include "Processors/ParticleBuiltInProcessors.h"
#include "Processors/ParticleProcessorOptions.h"
#include "Processors/ParticleValueSamples.h"

#include <algorithm>
#include <array>
#include <cmath>

GE_REFLECT(GameEngine::Particles::ParticleNoiseParameters, Mode, Algorithm, Octaves, OctaveMultiplier, OctaveScale,
           Axes, Rotation, SizeModulation, Input, Strength, Wavelength, Scroll);

namespace GameEngine::Particles
{
namespace
{
using Mathematics::Vector3;
using Parameters = ParticleNoiseParameters;

constexpr uint32 kMaximumOctaves = 4;
constexpr float kMaximumSize = 10000.0f;
constexpr uint32 kRotationStream = 3;
constexpr uint32 kFieldOffsetStream = 4;
// The analytic field repeats every 2 pi field units along each axis and in time.
constexpr double kAnalyticPeriod = 6.283185307179586;
// Shortest wavelength a sample uses, so the field's frequency stays finite.
constexpr float kMinimumWavelength = 0.001f;
// Where Execute's sampled values sit: Strength, Wavelength and Scroll, in that order.
constexpr uint32 kStrengthValue = 0;
constexpr uint32 kWavelengthValue = 1;
constexpr uint32 kScrollValue = 2;

constexpr std::array<ParticleEnumOption, 2> kModes = {{
    {"force", "Force", static_cast<uint32>(ParticleNoiseMode::Force), "particle-choice-move"},
    {"displacement", "Displacement", static_cast<uint32>(ParticleNoiseMode::Displacement), "particle-choice-move"},
}};

constexpr std::array<ParticleEnumOption, 2> kAlgorithms = {{
    {"analytic", "Analytic", static_cast<uint32>(ParticleNoiseAlgorithm::Analytic)},
    {"gradient", "Gradient", static_cast<uint32>(ParticleNoiseAlgorithm::Gradient)},
}};

bool HasOctaves(const void* parameters)
{
    return static_cast<const Parameters*>(parameters)->Octaves > 1;
}

bool UsesCurves(const void* parameters)
{
    const auto& settings = *static_cast<const Parameters*>(parameters);
    return settings.Strength.UsesCurve() || settings.Wavelength.UsesCurve() || settings.Scroll.UsesCurve();
}

constexpr ParticleParameterField kFields[] = {
    {.Name = "Mode", .Label = "Motion", .Tooltip = "Force bends velocity; displacement moves position directly", .Kind = ParticleParameterKind::Enum, .Options = kModes},
    {.Name = "Algorithm", .Label = "Field", .Tooltip = "Analytic is about a third of the cost; gradient noise shows no repeating pattern", .Kind = ParticleParameterKind::Enum, .Options = kAlgorithms},
    {.Name = "Octaves", .Label = "Octaves", .Tooltip = "Layers of the field, each at a shorter wavelength", .Kind = ParticleParameterKind::UInt, .Minimum = 1.0f, .Maximum = static_cast<float>(kMaximumOctaves)},
    {.Name = "OctaveMultiplier", .Label = "Octave Strength", .Tooltip = "Strength of each octave relative to the last", .Kind = ParticleParameterKind::Float, .Minimum = 0.0f, .Maximum = 1.0f, .Visible = HasOctaves},
    {.Name = "OctaveScale", .Label = "Octave Frequency", .Tooltip = "Frequency of each octave relative to the last", .Kind = ParticleParameterKind::Float, .Minimum = 1.0f, .Maximum = 8.0f, .Visible = HasOctaves},
    {.Name = "Axes", .Label = "Axis Weights", .Tooltip = "How much of the field applies along each axis", .Kind = ParticleParameterKind::Vector3, .ComponentLabels = kParticleVectorLabels},
    {.Name = "Rotation", .Label = "Rotation", .Tooltip = "Spin the field adds, in radians per second", .Kind = ParticleParameterKind::Float},
    {.Name = "SizeModulation", .Label = "Size Modulation", .Tooltip = "Relative size change around the birth size", .Kind = ParticleParameterKind::Float},
    {.Name = "Input", .Label = "Curve Input", .Tooltip = "What the curves are evaluated at", .Kind = ParticleParameterKind::ValueInput, .Visible = UsesCurves},
    {.Name = "Strength", .Label = "Strength", .Tooltip = "Force or displacement per second", .Kind = ParticleParameterKind::Value},
    {.Name = "Wavelength", .Label = "Wavelength", .Tooltip = "World distance over which the field varies", .Kind = ParticleParameterKind::Value},
    {.Name = "Scroll", .Label = "Scroll Speed", .Tooltip = "How fast the field changes over time", .Kind = ParticleParameterKind::Value},
};

// Closed-form curl of A = (sin(y+t)cos(z-t), sin(z+t)cos(x-t), sin(x+t)cos(y-t)) at a point in field
// units: spatially coherent and divergence-free.
Vector3 AnalyticCurl(const Vector3& point, float time)
{
    const float x = point.x;
    const float y = point.y;
    const float z = point.z;
    return {-std::sin(x + time) * std::sin(y - time) - std::cos(z + time) * std::cos(x - time),
            -std::sin(y + time) * std::sin(z - time) - std::cos(x + time) * std::cos(y - time),
            -std::sin(z + time) * std::sin(x - time) - std::cos(y + time) * std::cos(z - time)};
}

// Where one emitter's processor samples the analytic field, in field units: a seeded offset within one
// period, so emitters at one place, or two noise processors in one stack, do not push alike.
Vector3 AnalyticFieldOffset(uint32 seed, uint32 processorId)
{
    const auto component = [seed, processorId](uint32 axis)
    { return ParticleRandom(seed, axis, processorId, kFieldOffsetStream) * static_cast<float>(kAnalyticPeriod); };
    return {component(0), component(1), component(2)};
}

// The field's time argument: the emitter's clock times the scroll speed. The analytic field is periodic
// in time, so its argument is wrapped in double precision and stays exact however long the emitter runs.
// Used once per tick for a scroll every particle shares.
float FieldTime(bool gradient, double elapsed, float scroll)
{
    const double time = elapsed * static_cast<double>(scroll);
    return static_cast<float>(gradient ? time : std::fmod(time, kAnalyticPeriod));
}

// The emitter's clock split once per tick into whole analytic periods and the remainder, so a particle
// with a scroll of its own wraps its field time in float.
struct FieldClock
{
    double Elapsed = 0.0;
    float Periods = 0.0f;
    float Remainder = 0.0f;
};

FieldClock SplitFieldClock(double elapsed)
{
    const double periods = std::floor(elapsed / kAnalyticPeriod);
    return {elapsed, static_cast<float>(periods), static_cast<float>(elapsed - periods * kAnalyticPeriod)};
}

// FieldTime for one particle's scroll. (periods * P + remainder) * scroll equals
// P * fract(periods * scroll) + remainder * scroll up to whole periods, which the analytic field does not
// see. Periods stay exact in float for 2^24 of them (about three years of emitter time); the product
// rounds by at most half a float step of periods * scroll, 0.0015 radians after a day at a scroll of 0.3.
float ParticleFieldTime(bool gradient, const FieldClock& clock, float scroll)
{
    if (gradient)
        return static_cast<float>(clock.Elapsed * static_cast<double>(scroll));
    const float turns = clock.Periods * scroll;
    return static_cast<float>(kAnalyticPeriod) * (turns - std::floor(turns)) + clock.Remainder * scroll;
}

// One octave of the field at a simulation-space position; both fields are sampled in field units, so
// neither grows stronger as the wavelength shrinks.
Vector3 SampleCurl(bool gradient, const Vector3& position, float time, float wavelength, uint32 seed,
                   const Vector3& analyticOffset)
{
    const float frequency = 1.0f / std::max(std::fabs(wavelength), kMinimumWavelength);
    const Vector3 point = position * frequency;
    if (!gradient)
        return AnalyticCurl(point + analyticOffset, time);
    return Noise::GradientCurl3D(point + Vector3{time, time, time}, seed);
}

ParticleChannelMask Channels(const void* parameters)
{
    const auto& settings = *static_cast<const Parameters*>(parameters);
    ParticleChannelMask mask = ChannelBit(ParticleChannel::Position) |
                               ChannelBit(settings.Mode == ParticleNoiseMode::Displacement ? ParticleChannel::Position
                                                                                       : ParticleChannel::Velocity);
    if (settings.SizeModulation != 0.0f)
        mask |= ChannelBits(ParticleChannel::Size, ParticleChannel::BirthSize);
    if (settings.Rotation != 0.0f)
        mask |= ChannelBits(ParticleChannel::Rotation, ParticleChannel::SpawnIndex);
    return mask;
}

void Execute(ParticleProcessorContext& context)
{
    const auto& parameters = context.Params<Parameters>();
    const ParticleValue values[] = {parameters.Strength, parameters.Wavelength, parameters.Scroll};
    const auto samples = SampleParticleValues(context, parameters.Input, values);
    auto positions = context.Channels.Positions();
    auto velocities = context.Channels.Velocities();
    const auto spawns = context.Channels.SpawnIndices();
    const bool gradient = parameters.Algorithm == ParticleNoiseAlgorithm::Gradient;
    const uint32 octaves = std::clamp(parameters.Octaves, 1u, kMaximumOctaves);
    const uint32 fieldSeed = context.Emitter.Seed ^ context.ProcessorId;
    const Vector3 analyticOffset = AnalyticFieldOffset(context.Emitter.Seed, context.ProcessorId);
    const bool displace = parameters.Mode == ParticleNoiseMode::Displacement;
    const bool modulateSize = parameters.SizeModulation != 0.0f;
    const bool rotate = parameters.Rotation != 0.0f;
    auto sizes = modulateSize ? context.Channels.Sizes() : std::span<float>{};
    const auto birthSizes = modulateSize ? context.Channels.Get<float>(ParticleChannel::BirthSize) : std::span<float>{};
    auto rotations = rotate ? context.Channels.Rotations() : std::span<float>{};
    // Both fields scroll with the emitter's clock, so every particle sees one moving field and particles
    // born at one point at different times are pushed different ways.
    const bool sharedScroll = samples.IsUniform[kScrollValue];
    const float sharedTime = sharedScroll ? FieldTime(gradient, context.Emitter.Elapsed, samples.Uniform[kScrollValue]) : 0.0f;
    const FieldClock clock = sharedScroll ? FieldClock{} : SplitFieldClock(context.Emitter.Elapsed);
    for (size_t k = 0; k < context.Particles.size(); ++k)
    {
        const uint32 i = context.Particles[k];
        const float strength = samples.Get(kStrengthValue, k);
        const float wavelength = samples.Get(kWavelengthValue, k);
        Vector3 curl{};
        float weight = 1.0f;
        float total = 0.0f;
        float octaveWavelength = wavelength;
        const float time = sharedScroll ? sharedTime : ParticleFieldTime(gradient, clock, samples.Get(kScrollValue, k));
        for (uint32 octave = 0; octave < octaves; ++octave)
        {
            const Vector3 sample =
                SampleCurl(gradient, positions[i], time, octaveWavelength, fieldSeed, analyticOffset);
            curl = curl + sample * weight;
            total += weight;
            weight *= parameters.OctaveMultiplier;
            octaveWavelength /= parameters.OctaveScale;
        }
        const float amount = strength * context.DeltaTime / total;
        const Vector3 delta{curl.x * parameters.Axes.x * amount, curl.y * parameters.Axes.y * amount, curl.z * parameters.Axes.z * amount};
        if (displace)
            positions[i] = positions[i] + delta;
        else
            velocities[i] = velocities[i] + delta;
        const float normalized = curl.x / total * 0.5f;
        if (modulateSize)
            sizes[i] = std::clamp(birthSizes[i] * (1.0f + normalized * parameters.SizeModulation), 0.0f, kMaximumSize);
        if (rotate)
            rotations[i] += normalized * parameters.Rotation *
                            (ParticleRandom(context.Emitter.Seed, spawns[i], context.ProcessorId, kRotationStream) *
                                 2.0f -
                             1.0f) *
                            context.DeltaTime;
    }
}
} // namespace

ParticleProcessorDescriptor MakeNoiseProcessorDescriptor()
{
    ParticleProcessorDescriptor descriptor;
    descriptor.Id = "noise";
    descriptor.DisplayName = "Noise";
    descriptor.Description = "Turbulence from a divergence-free curl field";
    descriptor.IconClass = "particle-choice-noise";
    descriptor.Category = "Forces";
    descriptor.Stages = StageBit(ParticleStage::Update);
    descriptor.DefaultStage = ParticleStage::Update;
    descriptor.ParameterChannels = Channels;
    descriptor.Execute = Execute;
    BindParticleParameters<Parameters>(descriptor, kFields);
    return descriptor;
}

const ParticleProcessorDescriptor& ParticleNoiseProcessor()
{
    return FindBuiltInParticleProcessor("noise");
}

} // namespace GameEngine::Particles
