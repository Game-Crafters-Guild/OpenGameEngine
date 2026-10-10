// AudioListenerSystemTests.cpp — listener look is engine +Z (WorldTransform col2).

#include <gtest/gtest.h>

#include "Audio/AudioTypes.h"
#include "Audio/SpatializerHandedness.h"
#include "Components/Transform.h"
#include "ECSModules/Audio/Systems/AudioListenerSystem.h"
#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"

#include <cstring>

using GameEngine::Components::Transform;
using GameEngine::Components::WorldTransform;
using GameEngine::Mathematics::Quaternion;
using GameEngine::Mathematics::Vector3;
using GameEngine::Audio::ListenerState;
using GameEngine::Engine::Audio::FillListenerStateFromWorldTransform;

namespace
{

WorldTransform WorldFromLocal(const Transform& local)
{
    WorldTransform wt{};
    std::memcpy(wt.matrix, local.matrix, sizeof(wt.matrix));
    return wt;
}

} // namespace

TEST(AudioListener, DefaultForwardIsPlusZ)
{
    const ListenerState s{};
    EXPECT_FLOAT_EQ(s.forward[0], 0.0f);
    EXPECT_FLOAT_EQ(s.forward[1], 0.0f);
    EXPECT_FLOAT_EQ(s.forward[2], 1.0f);
    EXPECT_FLOAT_EQ(s.up[0], 0.0f);
    EXPECT_FLOAT_EQ(s.up[1], 1.0f);
    EXPECT_FLOAT_EQ(s.up[2], 0.0f);
}

TEST(AudioListener, IdentityWorldTransformLooksAlongPlusZ)
{
    const WorldTransform wt{};
    ListenerState s{};
    FillListenerStateFromWorldTransform(wt, s);

    EXPECT_NEAR(s.position[0], 0.0f, 1e-6f);
    EXPECT_NEAR(s.position[1], 0.0f, 1e-6f);
    EXPECT_NEAR(s.position[2], 0.0f, 1e-6f);
    EXPECT_NEAR(s.forward[0], 0.0f, 1e-5f);
    EXPECT_NEAR(s.forward[1], 0.0f, 1e-5f);
    EXPECT_NEAR(s.forward[2], 1.0f, 1e-5f);
    EXPECT_NEAR(s.up[0], 0.0f, 1e-5f);
    EXPECT_NEAR(s.up[1], 1.0f, 1e-5f);
    EXPECT_NEAR(s.up[2], 0.0f, 1e-5f);
}

TEST(AudioListener, TranslatedIdentityKeepsPlusZAndCopiesPosition)
{
    WorldTransform wt{};
    wt.matrix[12] = 3.0f;
    wt.matrix[13] = 4.0f;
    wt.matrix[14] = 5.0f;

    ListenerState s{};
    FillListenerStateFromWorldTransform(wt, s);

    EXPECT_NEAR(s.position[0], 3.0f, 1e-6f);
    EXPECT_NEAR(s.position[1], 4.0f, 1e-6f);
    EXPECT_NEAR(s.position[2], 5.0f, 1e-6f);
    EXPECT_NEAR(s.forward[0], 0.0f, 1e-5f);
    EXPECT_NEAR(s.forward[1], 0.0f, 1e-5f);
    EXPECT_NEAR(s.forward[2], 1.0f, 1e-5f);
}

// Same pin as TransformMathTests.FromTRS_YawPositiveSendsZTowardX: +Y yaw
// sends local +Z toward +X, and the listener must follow that look.
TEST(AudioListener, YawPositiveSendsForwardTowardPlusX)
{
    constexpr float kHalfPi = 1.57079632679f;
    const Quaternion q = Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), kHalfPi);
    const Transform local =
        Transform::FromTRS(Vector3(1.0f, 2.0f, 3.0f), q, Vector3(1.0f, 1.0f, 1.0f));
    const WorldTransform wt = WorldFromLocal(local);

    ListenerState s{};
    FillListenerStateFromWorldTransform(wt, s);

    EXPECT_NEAR(s.position[0], 1.0f, 1e-4f);
    EXPECT_NEAR(s.position[1], 2.0f, 1e-4f);
    EXPECT_NEAR(s.position[2], 3.0f, 1e-4f);
    EXPECT_NEAR(s.forward[0], 1.0f, 1e-4f);
    EXPECT_NEAR(s.forward[1], 0.0f, 1e-4f);
    EXPECT_NEAR(s.forward[2], 0.0f, 1e-4f);
    EXPECT_NEAR(s.up[0], 0.0f, 1e-4f);
    EXPECT_NEAR(s.up[1], 1.0f, 1e-4f);
    EXPECT_NEAR(s.up[2], 0.0f, 1e-4f);
}

// miniaudio look-at: axisX = cross(look, up), then negate if left-handed.
// Engine +Z look / +Y up / +X offset must pan to the RIGHT ear under LH.
TEST(AudioListener, LhLookAtPutsWorldPlusXOnTheRightEar)
{
    using GameEngine::Audio::ListenerSpaceX;
    using GameEngine::Audio::kMiniaudioListenerLeftHanded;

    const float lh = ListenerSpaceX(0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, true);
    const float rh = ListenerSpaceX(0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, false);
    EXPECT_GT(lh, 0.0f);
    EXPECT_LT(rh, 0.0f);
    EXPECT_NEAR(ListenerSpaceX(0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, -1.0f, 0.0f, 0.0f, true),
                -lh, 1e-5f);
    EXPECT_NEAR(ListenerSpaceX(0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, true),
                0.0f, 1e-5f);
    // After +Y yaw (look +X), engine right (+X) has gone to world -Z.
    EXPECT_GT(ListenerSpaceX(1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, -1.0f, true), 0.0f);
    EXPECT_TRUE(kMiniaudioListenerLeftHanded);
}
