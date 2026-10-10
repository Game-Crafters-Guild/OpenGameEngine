#pragma once

#include "Animation/RootMotion.h"
#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"

namespace GameEngine
{
namespace Animation
{

struct AnimationPose;

// Extracts root motion from an AnimationPose by capturing the root bone's
// (index 0) transform delta from the previous frame, then zeroing it out
// so the skeleton stays in place.
class RootMotionExtractor
{
public:
    // Call after pose evaluation. Returns the delta to apply to the entity.
    RootMotionDelta Extract(AnimationPose& pose);

    // Reset when animation changes (no delta produced on first frame after reset).
    void Reset();

    bool HasPrevious() const { return m_HasPrevious; }

private:
    bool m_HasPrevious = false;
    Mathematics::Vector3 m_PrevPosition;
    Mathematics::Quaternion m_PrevRotation;
};

} // namespace Animation
} // namespace GameEngine
