#include "Animation/SkeletonModifierStack.h"

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Animation;

TEST(SkeletonModifierStack, CopyTransformBlendsTargetBone)
{
    AnimationPose pose;
    pose.Resize(2);
    pose.Positions[0] = Mathematics::Vector3(10.0f, 0.0f, 0.0f);
    pose.Positions[1] = Mathematics::Vector3(0.0f, 0.0f, 0.0f);

    SkeletonModifierStack stack;
    SkeletonModifier modifier;
    modifier.Kind = SkeletonModifierKind::CopyTransform;
    modifier.SourceBone = 0;
    modifier.TargetBone = 1;
    modifier.Weight = 0.25f;
    stack.Add(modifier);

    stack.Apply(pose);
    EXPECT_FLOAT_EQ(pose.Positions[1].x, 2.5f);
    EXPECT_FLOAT_EQ(pose.Positions[1].y, 0.0f);
    EXPECT_FLOAT_EQ(pose.Positions[1].z, 0.0f);
}
