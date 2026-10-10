#pragma once

#include "Mathematics/Vector3.h"
#include "Types/Types.h"

namespace GameEngine::Particles
{
struct ParticleProcessorDescriptor;

enum class ParticleShapeKind : uint8
{
    Sphere,
    Hemisphere,
    Box,
    Circle,
    Cone,
    Cylinder,
    Line,
    MeshVertices,
    MeshSurface
};

/// Places newborn particles on or inside a shape around the emitter, uniformly by volume or area.
struct ParticleShapeParameters
{
    ParticleShapeKind Shape = ParticleShapeKind::Sphere;
    float Radius = 1.0f;
    /// Inner radius of a circle or cylinder, for rings.
    float InnerRadius = 0.0f;
    float Length = 1.0f;
    /// Cone half angle in degrees.
    float Angle = 30.0f;
    /// Box half extents.
    Mathematics::Vector3 Extents{1.0f, 1.0f, 1.0f};
    Mathematics::Vector3 Offset{0.0f, 0.0f, 0.0f};
    bool Surface = false;
    /// Turn the birth velocity to the shape's surface normal, keeping its speed.
    bool AlignToNormal = false;
};

/// The registered "shape" processor.
const ParticleProcessorDescriptor& ParticleShapeProcessor();
} // namespace GameEngine::Particles
