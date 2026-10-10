#pragma once

#include "Mathematics/Curve.h"
#include "Types/Types.h"

#include <initializer_list>
#include <type_traits>
#include <utility>

namespace GameEngine::Particles
{

/// How a processor parameter varies across particles and over their lives.
enum class ParticleValueMode : uint8
{
    /// One value for every particle.
    Constant,
    /// A value picked once per particle between Minimum and Maximum.
    Range,
    /// MinimumCurve evaluated at the processor's input.
    Curve,
    /// A value picked once per particle between MinimumCurve and MaximumCurve at the input.
    CurveRange
};

/// The quantity a curve is evaluated at. Every driver but Random changes during a particle's life.
enum class ParticleDriver : uint8
{
    /// Age divided by lifetime: 0 at birth, 1 at death.
    NormalizedAge,
    /// Seconds since birth.
    Age,
    /// Seconds since the particle entered its current phase.
    PhaseAge,
    /// A value in [0, 1) fixed per particle; every channel of one processor sees the same value.
    Random,
    /// Particle speed in units per second.
    Speed,
    /// Particle size.
    Size,
    /// Emitter speed in world units per second.
    EmitterSpeed,
    Custom0,
    Custom1,
    Custom2,
    Custom3
};

/// How an input outside [Minimum, Maximum] maps into the curve domain [0, 1].
enum class ParticleWrap : uint8
{
    Clamp,
    Repeat,
    Mirror
};

/// A scalar processor parameter: a constant, a per-particle random range, or a curve (or a random
/// blend between two curves) over the processor's input. Curves use the engine curve keys, so the
/// inspector edits them with the shared curve field. Trivially copyable: parameter blocks are PODs.
struct ParticleValue
{
    ParticleValueMode Mode = ParticleValueMode::Constant;
    float Minimum = 0.0f;
    float Maximum = 0.0f;
    Math::Curve MinimumCurve{};
    Math::Curve MaximumCurve{};

    constexpr ParticleValue() = default;
    constexpr ParticleValue(float constant) : Minimum(constant), Maximum(constant) {}
    constexpr ParticleValue(float minimum, float maximum)
        : Mode(ParticleValueMode::Range), Minimum(minimum), Maximum(maximum)
    {
    }

    /// A curve through the given (time, value) keys, linear between them.
    static ParticleValue MakeCurve(std::initializer_list<std::pair<float, float>> keys);

    bool UsesCurve() const { return Mode == ParticleValueMode::Curve || Mode == ParticleValueMode::CurveRange; }
    bool UsesRandom() const { return Mode == ParticleValueMode::Range || Mode == ParticleValueMode::CurveRange; }
};

static_assert(std::is_trivially_copyable_v<ParticleValue>);
static_assert(std::is_standard_layout_v<ParticleValue>);

/// The input a processor's curves are evaluated at, shared by all of its values.
struct ParticleValueInput
{
    ParticleDriver Driver = ParticleDriver::NormalizedAge;
    /// Driver values mapped to the curve domain's 0 and 1.
    float Minimum = 0.0f;
    float Maximum = 1.0f;
    ParticleWrap Wrap = ParticleWrap::Clamp;
};

static_assert(std::is_trivially_copyable_v<ParticleValueInput>);

/// Stable hash-based random number in [0, 1): the same (seed, particle, processor, stream) always
/// gives the same value, independent of iteration order or frame count.
float ParticleRandom(uint32 seed, uint32 particle, uint32 processor, uint32 stream);

/// Maps a raw driver value into the curve domain [0, 1] through `input`'s range and wrap.
float RemapParticleInput(const ParticleValueInput& input, float driver);

/// Evaluates `value` at curve input `input` with the particle's random pick `random` in [0, 1).
float EvaluateParticleValue(const ParticleValue& value, float input, float random);

} // namespace GameEngine::Particles
