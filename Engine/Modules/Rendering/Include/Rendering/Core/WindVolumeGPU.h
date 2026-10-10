#pragma once
#include "Types/Types.h"

namespace GameEngine::Rendering
{
// Shared std430 wind-volume record, sorted by priority/entity.
struct alignas(16) WindVolumeGPU
{
    float32 CenterShape[4]{}; // xyz center; w: -1 global, -2 invalid, otherwise WindVolumeShape
    float32 AxisXExtent[4]{};
    float32 AxisYExtent[4]{};
    float32 AxisZExtent[4]{};
    float32 VelocityWeight[4]{};
    float32 Gust[4]{}; // turbulence, frequency, wavelength, blend distance
    float32 Mode[4]{}; // x: 0 additive, 1 override
};
static_assert(sizeof(WindVolumeGPU) == 112);
}
