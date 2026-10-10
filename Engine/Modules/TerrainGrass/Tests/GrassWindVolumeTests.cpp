#include "Components/Rendering/WindVolume.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "Engine/Rendering/WindVolumeResolver.h"
#include "GlslShim.h"
#include <gtest/gtest.h>
#include <cstring>

namespace Shader
{
using namespace GameEngine::GlslShim;
using GameEngine::GlslShim::sqrt;
using GameEngine::GlslShim::abs;
#include "GrassWindVolumesExtracted.h"
}
using namespace GameEngine;
using namespace Engine::Renderer;

namespace
{
ECS::EntityHandle AddWind(ECS::World& world, Components::WindVolume v,
                          Components::WorldTransform xf = {})
{
    const auto e = world.CreateEntity();
    world.AddComponentImmediate(e, v);
    world.AddComponentImmediate(e, xf);
    return e;
}
std::vector<Rendering::WindVolumeGPU> ExtractGPU(ECS::World& world, uint32 mask = ~0u)
{
    std::vector<Rendering::WindVolumeGPU> out;
    WindVolumeResolver::ExtractGPU(world, out, mask);
    return out;
}
void CheckParity(ECS::World& world, float x, float y, float z, uint32 mask = ~0u)
{
    const ResolvedWind base{0.8f, 0.1f, -0.6f, 0.16f, 0.9f, 20.0f, 0.0f};
    auto gpu = Shader::GrassResolvedWind{Shader::vec3(base.VelocityX, base.VelocityY, base.VelocityZ),
        base.Turbulence, base.GustFrequency, base.GustScale, base.Weight};
    for (const auto& packed : ExtractGPU(world, mask))
    {
        Shader::GrassWindVolume volume;
        static_assert(sizeof(volume) == sizeof(packed));
        std::memcpy(&volume, &packed, sizeof(volume));
        gpu = Shader::grassBlendVolume(gpu, volume, Shader::vec3(x, y, z));
    }
    const auto cpu = WindVolumeResolver::ResolveAt(world, x, y, z, base, mask);
    EXPECT_NEAR(cpu.VelocityX, gpu.velocity.x, 1e-5f);
    EXPECT_NEAR(cpu.VelocityY, gpu.velocity.y, 1e-5f);
    EXPECT_NEAR(cpu.VelocityZ, gpu.velocity.z, 1e-5f);
    EXPECT_NEAR(cpu.Turbulence, gpu.turbulence, 1e-5f);
    EXPECT_NEAR(cpu.GustFrequency, gpu.frequency, 1e-5f);
    EXPECT_NEAR(cpu.GustScale, gpu.scale, 1e-4f);
    EXPECT_NEAR(cpu.Weight, gpu.weight, 1e-5f);
}
}

TEST(GrassWindVolumes, ShaderMatchesCpuForEveryShapeAndBlendAtRotatedScaledSites)
{
    for (int shape = 0; shape < 4; ++shape)
    for (int mode = 0; mode < 2; ++mode)
    {
        ECS::World world;
        Components::WindVolume v{};
        v.Shape = static_cast<Components::WindVolumeShape>(shape);
        v.BlendMode = static_cast<Components::WindVolumeBlendMode>(mode);
        v.Weight = 0.65f;
        v.Speed = 3.0f;
        Components::WorldTransform xf{};
        xf.matrix[0] = 0; xf.matrix[2] = -4;
        xf.matrix[5] = 8;
        xf.matrix[8] = 2; xf.matrix[10] = 0;
        xf.matrix[12] = 10; xf.matrix[13] = 2; xf.matrix[14] = -4;
        AddWind(world, v, xf);
        for (int x = 5; x <= 15; ++x)
        for (int y = -4; y <= 8; ++y)
        for (int z = -9; z <= 1; ++z)
            CheckParity(world, float(x), float(y), float(z));
    }
}

TEST(GrassWindVolumes, PriorityTiesGlobalLocalAndCalmBlendMatchCpu)
{
    for (int mode = 0; mode < 2; ++mode)
    {
        ECS::World world;
        Components::WindVolume v{};
        v.IsGlobal = true; v.Priority = 5; v.Speed = 2;
        AddWind(world, v);
        v.Priority = -1; v.Speed = 8; v.BlendMode = Components::WindVolumeBlendMode::Additive;
        AddWind(world, v);
        v.IsGlobal = false; v.Priority = 5; v.Speed = 0;
        v.Weight = 0.5f; v.BlendMode = static_cast<Components::WindVolumeBlendMode>(mode);
        AddWind(world, v);
        CheckParity(world, 0, 0, 0);
        CheckParity(world, 1.5f, 0, 0);
        CheckParity(world, 100, 0, 0);
    }
}

TEST(GrassWindVolumes, DisabledFilteredAndRemovedVolumesLeaveNoSnapshot)
{
    ECS::World world;
    Components::WindVolume v{};
    ECS::Entity(&world, AddWind(world, v)).SetEnabled<Components::WindVolume>(false);
    auto disabled = AddWind(world, v);
    world.AddComponentImmediate(disabled, ECS::Disabled{});
    v.LayerMask = 2;
    auto filtered = AddWind(world, v);
    EXPECT_TRUE(ExtractGPU(world, 1).empty());
    EXPECT_EQ(ExtractGPU(world, 2).size(), 1u);
    CheckParity(world, 0, 0, 0, 1);
    world.RemoveComponentImmediate<Components::WindVolume>(filtered);
    EXPECT_TRUE(ExtractGPU(world).empty());
}

TEST(GrassWindVolumes, DegenerateTransformOnlyAffectsGlobalVolumes)
{
    ECS::World world;
    Components::WorldTransform xf{};
    xf.matrix[0] = 0;
    Components::WindVolume v{};
    AddWind(world, v, xf);
    CheckParity(world, 0, 0, 0);
    v.IsGlobal = true;
    v.DirectionX = v.DirectionY = v.DirectionZ = 0;
    AddWind(world, v, xf);
    CheckParity(world, 500, -20, 10);
}
