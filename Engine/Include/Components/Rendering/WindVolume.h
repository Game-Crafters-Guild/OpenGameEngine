#pragma once

#include "Types/Types.h"

namespace GameEngine::Components
{

enum class WindVolumeShape : int32
{
    Box = 0,
    Sphere = 1,
    Capsule = 2,
    Cylinder = 3,
};

enum class WindVolumeBlendMode : int32
{
    Additive = 0,
    Override = 1,
};

struct WindVolume
{
    bool IsGlobal = false;
    WindVolumeShape Shape = WindVolumeShape::Box;
    WindVolumeBlendMode BlendMode = WindVolumeBlendMode::Override;
    float32 BlendDistance = 2.0f;
    float32 Weight = 1.0f;
    int32 Priority = 0;
    uint32 LayerMask = 0xFFFFFFFFu;

    float32 DirectionX = 0.5f;
    float32 DirectionY = 0.0f;
    float32 DirectionZ = 0.5f;
    float32 Speed = 1.0f;
    float32 Turbulence = 0.35f;
    float32 GustFrequency = 0.5f;
    float32 GustScale = 70.0f;
};

} // namespace GameEngine::Components
