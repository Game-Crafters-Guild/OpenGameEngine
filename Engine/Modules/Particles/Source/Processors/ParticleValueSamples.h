#pragma once

#include "Particles/ParticleProcessorRegistry.h"

#include <array>
#include <span>

namespace GameEngine::Particles
{
// The values of one processor for every particle of a batch, each hoisted to a single number when
// it is the same for every particle. Values share the processor's input, sampled once.
struct ParticleValueSamples
{
    std::array<std::span<const float>, 4> PerParticle;
    std::array<float, 4> Uniform{};
    std::array<bool, 4> IsUniform{};

    float Get(uint32 value, size_t particle) const
    {
        return IsUniform[value] ? Uniform[value] : PerParticle[value][particle];
    }
};

// Samples `values` (at most four) over the batch. Scratch array 0 holds the inputs and array
// 1 + n value n; random streams are 1 + n so the values of a processor pick independently.
inline ParticleValueSamples SampleParticleValues(ParticleProcessorContext& context, const ParticleValueInput& input,
                                                 std::span<const ParticleValue> values)
{
    ParticleValueSamples samples;
    bool needsInput = false;
    for (const auto& value : values)
        needsInput |= value.UsesCurve();
    const auto inputs = context.SampleInputs(input, needsInput, 0);
    for (uint32 index = 0; index < values.size() && index < 4; ++index)
    {
        const auto& value = values[index];
        samples.IsUniform[index] = value.Mode == ParticleValueMode::Constant;
        if (samples.IsUniform[index])
            samples.Uniform[index] = value.Minimum;
        else
            samples.PerParticle[index] = context.SampleValue(value, inputs, 1 + index, 1 + index);
    }
    return samples;
}
} // namespace GameEngine::Particles
