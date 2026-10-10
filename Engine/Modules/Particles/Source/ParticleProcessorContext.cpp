#include "Particles/ParticleProcessorRegistry.h"

#include "Mathematics/Interpolation.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Particles
{
using Mathematics::Vector3;

Vector3 ParticleEmitterFrame::LocalToWorldVector(const Vector3& vector) const
{
    return Axis[0] * vector.x + Axis[1] * vector.y + Axis[2] * vector.z;
}

Vector3 ParticleEmitterFrame::WorldToLocalVector(const Vector3& vector) const
{
    return InverseAxis[0] * vector.x + InverseAxis[1] * vector.y + InverseAxis[2] * vector.z;
}

Vector3 ParticleEmitterFrame::LocalToWorldPoint(const Vector3& point) const
{
    return LocalToWorldVector(point) + Origin;
}

Vector3 ParticleEmitterFrame::WorldToLocalPoint(const Vector3& point) const
{
    return WorldToLocalVector(point - Origin);
}

Vector3 ParticleEmitterFrame::SimulationToWorldPoint(const Vector3& point) const
{
    return LocalSpace ? LocalToWorldPoint(point) : point;
}

Vector3 ParticleEmitterFrame::SimulationToWorldVector(const Vector3& vector) const
{
    return LocalSpace ? LocalToWorldVector(vector) : vector;
}

Vector3 ParticleEmitterFrame::WorldToSimulationPoint(const Vector3& point) const
{
    return LocalSpace ? WorldToLocalPoint(point) : point;
}

Vector3 ParticleEmitterFrame::WorldToSimulationVector(const Vector3& vector) const
{
    return LocalSpace ? WorldToLocalVector(vector) : vector;
}

Vector3 ParticleEmitterFrame::LocalToSimulationVector(const Vector3& vector) const
{
    return LocalSpace ? vector : LocalToWorldVector(vector);
}

Vector3 ParticleEmitterFrame::LocalToSimulationPoint(const Vector3& point) const
{
    return LocalSpace ? point : LocalToWorldPoint(point);
}

std::span<const float> ParticleProcessorContext::SampleInputs(const ParticleValueInput& input, bool needed,
                                                              uint32 scratchIndex)
{
    if (!needed)
        return {};
    auto out = Scratch.Floats(scratchIndex).first(Particles.size());
    const auto count = Particles.size();
    switch (input.Driver)
    {
    case ParticleDriver::Age:
    {
        const auto ages = Channels.Ages();
        for (size_t k = 0; k < count; ++k)
            out[k] = ages[Particles[k]];
        break;
    }
    case ParticleDriver::PhaseAge:
    {
        const auto ages = Channels.PhaseAges();
        for (size_t k = 0; k < count; ++k)
            out[k] = ages[Particles[k]];
        break;
    }
    case ParticleDriver::Random:
    {
        const auto spawns = Channels.SpawnIndices();
        for (size_t k = 0; k < count; ++k)
            out[k] = ParticleRandom(Emitter.Seed, spawns[Particles[k]], ProcessorId, 0);
        break;
    }
    case ParticleDriver::Speed:
    {
        const auto velocities = Channels.Velocities();
        for (size_t k = 0; k < count; ++k)
            out[k] = velocities[Particles[k]].Length();
        break;
    }
    case ParticleDriver::Size:
    {
        const auto sizes = Channels.Sizes();
        for (size_t k = 0; k < count; ++k)
            out[k] = sizes[Particles[k]];
        break;
    }
    case ParticleDriver::EmitterSpeed:
    {
        std::fill(out.begin(), out.end(), Emitter.Velocity.Length());
        break;
    }
    case ParticleDriver::Custom0:
    case ParticleDriver::Custom1:
    case ParticleDriver::Custom2:
    case ParticleDriver::Custom3:
    {
        const auto channel = static_cast<ParticleChannel>(
            static_cast<uint32>(ParticleChannel::Custom0) +
            (static_cast<uint32>(input.Driver) - static_cast<uint32>(ParticleDriver::Custom0)));
        const auto custom = Channels.Get<float>(channel);
        for (size_t k = 0; k < count; ++k)
            out[k] = custom[Particles[k]];
        break;
    }
    case ParticleDriver::NormalizedAge:
    default:
    {
        const auto ages = Channels.Ages();
        const auto lifetimes = Channels.Lifetimes();
        for (size_t k = 0; k < count; ++k)
        {
            const uint32 i = Particles[k];
            out[k] = ages[i] / std::max(0.001f, lifetimes[i]);
        }
        break;
    }
    }
    for (auto& value : out)
        value = RemapParticleInput(input, value);
    return out;
}

std::span<const float> ParticleProcessorContext::SampleValue(const ParticleValue& value, std::span<const float> inputs,
                                                             uint32 stream, uint32 scratchIndex)
{
    auto out = Scratch.Floats(scratchIndex).first(Particles.size());
    const auto count = Particles.size();
    const auto spawns = Channels.SpawnIndices();
    switch (value.Mode)
    {
    case ParticleValueMode::Range:
        for (size_t k = 0; k < count; ++k)
            out[k] = Math::Lerp(value.Minimum, value.Maximum,
                                ParticleRandom(Emitter.Seed, spawns[Particles[k]], ProcessorId, stream));
        break;
    case ParticleValueMode::Curve:
        for (size_t k = 0; k < count; ++k)
            out[k] = Math::EvaluateCurveKeys(value.MinimumCurve.Keys, value.MinimumCurve.KeyCount, inputs[k]);
        break;
    case ParticleValueMode::CurveRange:
        for (size_t k = 0; k < count; ++k)
        {
            const float low = Math::EvaluateCurveKeys(value.MinimumCurve.Keys, value.MinimumCurve.KeyCount, inputs[k]);
            const float high = Math::EvaluateCurveKeys(value.MaximumCurve.Keys, value.MaximumCurve.KeyCount, inputs[k]);
            out[k] = Math::Lerp(low, high, ParticleRandom(Emitter.Seed, spawns[Particles[k]], ProcessorId, stream));
        }
        break;
    case ParticleValueMode::Constant:
    default:
        std::fill(out.begin(), out.end(), value.Minimum);
        break;
    }
    return out;
}

} // namespace GameEngine::Particles
