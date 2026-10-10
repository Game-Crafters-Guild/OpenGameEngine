#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include "Mathematics/Vector4.h"

namespace GameEngine::Particles
{
/// One particle as the particle shaders read it, 128 bytes in std430; mirrors
/// Includes/particle_instance_fields.glsl. Particle draws use their own upload arena; they do
/// not allocate, dirty, or cull ordinary GPUScene instances.
struct alignas(16) ParticleRenderData
{
    Mathematics::Vector4 PositionSize{};
    Mathematics::Vector4 VelocityAngle{};
    Mathematics::Vector4 Color{};
    Mathematics::Vector4 AxisX{1,0,0,0};
    Mathematics::Vector4 AxisY{0,1,0,0};
    Mathematics::Vector4 AxisZ{0,0,1,0};
    Mathematics::Vector4 Animation{}; // age, sprite frame, velocity stretch, trail length
    std::array<uint32_t, 4> Metadata{}; // material, alignment, instance flags, geometry (sprite/mesh/ribbon)
};
static_assert(std::is_trivially_copyable_v<ParticleRenderData>);
static_assert(std::is_standard_layout_v<ParticleRenderData>);
static_assert(sizeof(ParticleRenderData) == 128);
static_assert(offsetof(ParticleRenderData, PositionSize) == 0);
static_assert(offsetof(ParticleRenderData, VelocityAngle) == 16);
static_assert(offsetof(ParticleRenderData, Color) == 32);
static_assert(offsetof(ParticleRenderData, AxisX) == 48);
static_assert(offsetof(ParticleRenderData, AxisY) == 64);
static_assert(offsetof(ParticleRenderData, AxisZ) == 80);
static_assert(offsetof(ParticleRenderData, Animation) == 96);
static_assert(offsetof(ParticleRenderData, Metadata) == 112);
} // namespace GameEngine::Particles
