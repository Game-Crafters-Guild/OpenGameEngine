#include "SplineLayout/CenterlineSampling.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::SplineLayout
{

float32 LargestAxisScale(const Mathematics::Matrix4x4& worldMatrix)
{
    const auto axisScale = [&worldMatrix](int column)
    {
        const auto& axis = worldMatrix[column];
        return std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    };
    return std::max({axisScale(0), axisScale(1), axisScale(2)});
}

float32 WorldCenterlineLength(float32 localArcLength, const Mathematics::Matrix4x4& worldMatrix)
{
    return localArcLength * LargestAxisScale(worldMatrix);
}

uint32 CenterlineSampleCount(float32 worldLength)
{
    constexpr uint32 kMinCenterlineSamples = 2u;
    if (!(worldLength > 0.0f))
        return kMinCenterlineSamples;

    // Compare before casting: a length past the budget — or infinite — would
    // otherwise narrow to an out-of-range uint32.
    const float32 steps = worldLength / kCenterlineStepMetres;
    if (steps >= static_cast<float32>(kMaxCenterlineSamples))
        return kMaxCenterlineSamples;

    return std::max(static_cast<uint32>(steps) + 2u, kMinCenterlineSamples);
}

} // namespace GameEngine::SplineLayout
