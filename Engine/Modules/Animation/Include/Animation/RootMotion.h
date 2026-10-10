#pragma once

#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"

namespace GameEngine
{
namespace Animation
{

// Delta transform extracted from the root bone each frame.
// Applied to the entity's world transform for physically-grounded locomotion.
struct RootMotionDelta
{
    Mathematics::Vector3 Translation;
    Mathematics::Quaternion Rotation; // Identity by default

    static RootMotionDelta Blend(const RootMotionDelta& a, const RootMotionDelta& b, float weight);
};

} // namespace Animation
} // namespace GameEngine
