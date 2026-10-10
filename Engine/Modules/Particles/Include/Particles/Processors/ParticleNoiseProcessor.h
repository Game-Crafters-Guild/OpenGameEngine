#pragma once

#include "Mathematics/Vector3.h"
#include "Particles/ParticleValue.h"

namespace GameEngine::Particles
{
struct ParticleProcessorDescriptor;

/// Whether the curl field pushes velocity (a force) or displaces position directly.
enum class ParticleNoiseMode : uint8
{
    Force,
    Displacement
};

enum class ParticleNoiseAlgorithm : uint8
{
    /// A closed-form trigonometric curl sampled at a seeded offset and scrolled by the emitter's clock:
    /// cheap, and its pattern repeats every 2 pi wavelengths along each axis.
    Analytic,
    /// Curl of seeded 3D gradient noise (Noise::GradientCurl3D): no repeating pattern, at several
    /// times the analytic field's cost per particle.
    Gradient
};

/// Divergence-free turbulence from a curl field, optionally over several octaves. The field's
/// strength does not depend on its wavelength: a shorter wavelength makes it finer, not stronger.
struct ParticleNoiseParameters
{
    ParticleNoiseMode Mode = ParticleNoiseMode::Force;
    ParticleNoiseAlgorithm Algorithm = ParticleNoiseAlgorithm::Analytic;
    uint32 Octaves = 1;
    float OctaveMultiplier = 0.5f;
    float OctaveScale = 2.0f;
    Mathematics::Vector3 Axes{1.0f, 1.0f, 1.0f};
    /// Radians per second of spin the field adds, signed per particle.
    float Rotation = 0.0f;
    /// Relative size change the field applies around the birth size.
    float SizeModulation = 0.0f;
    ParticleValueInput Input{};
    ParticleValue Strength{1.0f};
    ParticleValue Wavelength{1.0f};
    ParticleValue Scroll{1.0f};
};

/// The registered "noise" processor.
const ParticleProcessorDescriptor& ParticleNoiseProcessor();
} // namespace GameEngine::Particles
