#include "Animation/RootMotion.h"

namespace GameEngine
{
namespace Animation
{

RootMotionDelta RootMotionDelta::Blend(const RootMotionDelta& a, const RootMotionDelta& b, float weight)
{
    RootMotionDelta result;
    result.Translation = a.Translation + (b.Translation - a.Translation) * weight;
    result.Rotation = Mathematics::Quaternion::Slerp(a.Rotation, b.Rotation, weight);
    return result;
}

} // namespace Animation
} // namespace GameEngine
