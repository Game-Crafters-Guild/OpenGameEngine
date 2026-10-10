#include "Particles/ParticleValue.h"

#include "Mathematics/Interpolation.h"
#include "Particles/ParticleProcessorRegistry.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace GameEngine::Particles
{
namespace
{
constexpr std::array<ParticleEnumOption, 4> kValueModes = {{
    {"constant", "Constant", static_cast<uint32>(ParticleValueMode::Constant)},
    {"range", "Random Range", static_cast<uint32>(ParticleValueMode::Range)},
    {"curve", "Curve", static_cast<uint32>(ParticleValueMode::Curve)},
    {"curveRange", "Random Curves", static_cast<uint32>(ParticleValueMode::CurveRange)},
}};

constexpr std::array<ParticleEnumOption, 11> kDrivers = {{
    {"normalizedAge", "Age / Lifetime", static_cast<uint32>(ParticleDriver::NormalizedAge), "particle-choice-time"},
    {"age", "Age (seconds)", static_cast<uint32>(ParticleDriver::Age), "particle-choice-time"},
    {"phaseAge", "Phase Age (seconds)", static_cast<uint32>(ParticleDriver::PhaseAge), "particle-choice-time"},
    {"random", "Random per Particle", static_cast<uint32>(ParticleDriver::Random), "particle-choice-curve"},
    {"speed", "Speed", static_cast<uint32>(ParticleDriver::Speed), "particle-choice-move"},
    {"size", "Size", static_cast<uint32>(ParticleDriver::Size), "particle-choice-size"},
    {"emitterSpeed", "Emitter Speed", static_cast<uint32>(ParticleDriver::EmitterSpeed), "particle-choice-move"},
    {"custom0", "Custom 1", static_cast<uint32>(ParticleDriver::Custom0), "particle-choice-custom"},
    {"custom1", "Custom 2", static_cast<uint32>(ParticleDriver::Custom1), "particle-choice-custom"},
    {"custom2", "Custom 3", static_cast<uint32>(ParticleDriver::Custom2), "particle-choice-custom"},
    {"custom3", "Custom 4", static_cast<uint32>(ParticleDriver::Custom3), "particle-choice-custom"},
}};

constexpr std::array<ParticleEnumOption, 3> kWraps = {{
    {"clamp", "Clamp", static_cast<uint32>(ParticleWrap::Clamp)},
    {"repeat", "Repeat", static_cast<uint32>(ParticleWrap::Repeat)},
    {"mirror", "Ping-Pong", static_cast<uint32>(ParticleWrap::Mirror)},
}};

float EvaluateCurve(const Math::Curve& curve, float input)
{
    return Math::EvaluateCurveKeys(curve.Keys, curve.KeyCount, input);
}
} // namespace

std::span<const ParticleEnumOption> ParticleValueModeOptions()
{
    return kValueModes;
}

std::span<const ParticleEnumOption> ParticleDriverOptions()
{
    return kDrivers;
}

std::span<const ParticleEnumOption> ParticleWrapOptions()
{
    return kWraps;
}

ParticleValue ParticleValue::MakeCurve(std::initializer_list<std::pair<float, float>> keys)
{
    ParticleValue value;
    value.Mode = ParticleValueMode::Curve;
    for (const auto& [time, keyValue] : keys)
    {
        Math::CurveKey key;
        key.Time = time;
        key.Value = keyValue;
        key.Interp = Math::CurveInterp::Linear;
        value.MinimumCurve.TryInsert(key);
    }
    value.MaximumCurve = value.MinimumCurve;
    return value;
}

float ParticleRandom(uint32 seed, uint32 particle, uint32 processor, uint32 stream)
{
    uint32 x = seed ^ (particle * 0x9e3779b9u) ^ (processor * 0x85ebca6bu) ^ (stream * 0xc2b2ae35u);
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return static_cast<float>(x & 0xffffffu) / 16777216.0f;
}

float RemapParticleInput(const ParticleValueInput& input, float driver)
{
    const float span = input.Maximum - input.Minimum;
    float normalized = span != 0.0f ? (driver - input.Minimum) / span : 0.0f;
    switch (input.Wrap)
    {
    case ParticleWrap::Repeat:
        return normalized - std::floor(normalized);
    case ParticleWrap::Mirror:
        normalized -= std::floor(normalized * 0.5f) * 2.0f;
        return normalized > 1.0f ? 2.0f - normalized : normalized;
    case ParticleWrap::Clamp:
    default:
        return std::clamp(normalized, 0.0f, 1.0f);
    }
}

float EvaluateParticleValue(const ParticleValue& value, float input, float random)
{
    switch (value.Mode)
    {
    case ParticleValueMode::Range:
        return Math::Lerp(value.Minimum, value.Maximum, random);
    case ParticleValueMode::Curve:
        return EvaluateCurve(value.MinimumCurve, input);
    case ParticleValueMode::CurveRange:
        return Math::Lerp(EvaluateCurve(value.MinimumCurve, input), EvaluateCurve(value.MaximumCurve, input), random);
    case ParticleValueMode::Constant:
    default:
        return value.Minimum;
    }
}

} // namespace GameEngine::Particles
