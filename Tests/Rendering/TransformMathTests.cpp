#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "Components/Transform.h"
#include "Components/Rendering/Light.h"
#include "Components/Hierarchy.h"
#include "ECS/World.h"
#include "ECS/Query.h"
#include "ECS/ChangeFilter.h"
#include "ECS/ECSTemplates.h"
#include "ECSModules/Rendering/Systems/TransformHierarchySystem.h"
#include "JobSystem/WorkStealingThreadPool.h"

using namespace GameEngine;
using namespace GameEngine::Components;
using GameEngine::Mathematics::Quaternion;
using GameEngine::Mathematics::Vector3;

namespace
{
    constexpr float kDegEpsilon = 0.1f; // generous for composed conversions

    void ExpectEulerNear(float ex, float ey, float ez,
                         float ax, float ay, float az,
                         float eps = kDegEpsilon)
    {
        EXPECT_NEAR(ax, ex, eps);
        EXPECT_NEAR(ay, ey, eps);
        EXPECT_NEAR(az, ez, eps);
    }

	    constexpr float kMatrixEpsilon   = 1e-4f;
	    constexpr float kPositionEpsilon = 1e-4f;
	    constexpr float kScaleEpsilon    = 1e-4f;

	    // Local copy of the column-major 4x4 multiply used by TransformHierarchySystem.
	    inline void MultiplyColumnMajor4x4Test(const float32* a,
	                                           const float32* b,
	                                           float32* out)
	    {
	        for (int col = 0; col < 4; ++col)
	        {
	            for (int row = 0; row < 4; ++row)
	            {
	                out[col * 4 + row] =
	                    a[0 * 4 + row] * b[col * 4 + 0] +
	                    a[1 * 4 + row] * b[col * 4 + 1] +
	                    a[2 * 4 + row] * b[col * 4 + 2] +
	                    a[3 * 4 + row] * b[col * 4 + 3];
	            }
	        }
	    }
}

namespace
{
    Vector3 MulPoint(const Transform& t, const Vector3& p)
    {
        const float* m = t.matrix;
        return Vector3(
            m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
            m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
            m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]);
    }
}

TEST(TransformMathTests, Identity_PlusZIsEntityForward)
{
    const Transform t = Transform::FromTRS(Vector3(0.0f, 0.0f, 0.0f), Quaternion{}, Vector3(1.0f, 1.0f, 1.0f));
    const Vector3 forward = MulPoint(t, Vector3(0.0f, 0.0f, 1.0f));
    EXPECT_NEAR(forward.x, 0.0f, 1e-4f);
    EXPECT_NEAR(forward.y, 0.0f, 1e-4f);
    EXPECT_NEAR(forward.z, 1.0f, 1e-4f);
}

TEST(TransformMathTests, DefaultSunEuler_PlusZShinesDownward)
{
    const Quaternion q = QuaternionFromEulerXYZDegrees(50.0f, -30.0f, 0.0f);
    const Transform t = Transform::FromTRS(Vector3(0.0f, 0.0f, 0.0f), q, Vector3(1.0f, 1.0f, 1.0f));
    const Vector3 shine = MulPoint(t, Vector3(0.0f, 0.0f, 1.0f));
    const Vector3 viaQuat = q.Rotate(Vector3(0.0f, 0.0f, 1.0f));
    EXPECT_LT(shine.y, 0.0f);
    EXPECT_NEAR(shine.x, viaQuat.x, 1e-4f);
    EXPECT_NEAR(shine.y, viaQuat.y, 1e-4f);
    EXPECT_NEAR(shine.z, viaQuat.z, 1e-4f);
}

TEST(TransformMathTests, FromTRS_YawPositiveSendsZTowardX)
{
    const float kHalfPi = 1.57079632679f;
    const Quaternion q = Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), kHalfPi);
    const Transform t = Transform::FromTRS(Vector3(0.0f, 0.0f, 0.0f), q, Vector3(1.0f, 1.0f, 1.0f));
    const Vector3 rotated = MulPoint(t, Vector3(0.0f, 0.0f, 1.0f));
    const Vector3 viaQuat = q.Rotate(Vector3(0.0f, 0.0f, 1.0f));
    EXPECT_NEAR(rotated.x, 1.0f, 1e-4f);
    EXPECT_NEAR(rotated.y, 0.0f, 1e-4f);
    EXPECT_NEAR(rotated.z, 0.0f, 1e-4f);
    EXPECT_NEAR(rotated.x, viaQuat.x, 1e-4f);
    EXPECT_NEAR(rotated.y, viaQuat.y, 1e-4f);
    EXPECT_NEAR(rotated.z, viaQuat.z, 1e-4f);
}

TEST(TransformMathTests, FromTRSRH_YawPositiveSendsZTowardMinusX)
{
    const float kHalfPi = 1.57079632679f;
    const Quaternion q = Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), kHalfPi);
    const Transform t = Transform::FromTRSRH(Vector3(0.0f, 0.0f, 0.0f), q, Vector3(1.0f, 1.0f, 1.0f));
    const Vector3 rotated = MulPoint(t, Vector3(0.0f, 0.0f, 1.0f));
    EXPECT_NEAR(rotated.x, -1.0f, 1e-4f);
    EXPECT_NEAR(rotated.y, 0.0f, 1e-4f);
    EXPECT_NEAR(rotated.z, 0.0f, 1e-4f);
}

TEST(TransformMathTests, FromTRS_GetRotationMatchesInput)
{
    const float kHalfPi = 1.57079632679f;
    const Quaternion q = Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), kHalfPi);
    const Transform t = Transform::FromTRS(Vector3(1.0f, 2.0f, 3.0f), q, Vector3(1.0f, 1.0f, 1.0f));
    const Quaternion back = t.GetRotation();
    const Vector3 a = q.Rotate(Vector3(0.0f, 0.0f, 1.0f));
    const Vector3 b = back.Rotate(Vector3(0.0f, 0.0f, 1.0f));
    EXPECT_NEAR(a.x, b.x, 1e-4f);
    EXPECT_NEAR(a.y, b.y, 1e-4f);
    EXPECT_NEAR(a.z, b.z, 1e-4f);
}

// Round-trip on compound rotations: a single-axis case cannot catch a
// component swizzle or a dropped term, so pin GetRotation(FromTRS(q)) on
// quats with all components non-zero, checked against two basis vectors.
TEST(TransformMathTests, FromTRS_GetRotationRoundTripsCompoundRotations)
{
    const Quaternion cases[] = {
        QuaternionFromEulerXYZDegrees(50.0f, -30.0f, 0.0f), // the default sun pose
        QuaternionFromEulerXYZDegrees(50.0f, -30.0f, 20.0f),
        Quaternion(0.8923991f, 0.2391176f, 0.3696438f, -0.0990458f), // yaw45 then pitch30
    };
    const Vector3 probes[] = {Vector3(0.0f, 0.0f, 1.0f), Vector3(1.0f, 0.0f, 0.0f)};
    for (const Quaternion& q : cases)
    {
        const Transform t = Transform::FromTRS(Vector3(0.0f, 0.0f, 0.0f), q, Vector3(1.0f, 1.0f, 1.0f));
        const Quaternion back = t.GetRotation();
        for (const Vector3& v : probes)
        {
            const Vector3 a = q.Rotate(v);
            const Vector3 b = back.Rotate(v);
            EXPECT_NEAR(a.x, b.x, 1e-4f);
            EXPECT_NEAR(a.y, b.y, 1e-4f);
            EXPECT_NEAR(a.z, b.z, 1e-4f);
        }
    }
}

// The Basic3D template ships this literal; it must be the engine's own
// Euler-to-quaternion output, unit-norm. A hand-derived value with a dropped
// component denormalizes the rotation basis FromTRS builds from it.
TEST(TransformMathTests, DefaultSunSceneLiteralMatchesEulerComposition)
{
    const Quaternion q = QuaternionFromEulerXYZDegrees(
        kDefaultDirectionalLightEulerXDeg,
        kDefaultDirectionalLightEulerYDeg,
        kDefaultDirectionalLightEulerZDeg);
    const auto& g = q.GetGLM();
    EXPECT_NEAR(g.x, 0.408218f, 1e-5f);
    EXPECT_NEAR(g.y, -0.23457f, 1e-5f);
    EXPECT_NEAR(g.z, 0.109382f, 1e-5f);
    EXPECT_NEAR(g.w, 0.875426f, 1e-5f);
    EXPECT_NEAR(g.x * g.x + g.y * g.y + g.z * g.z + g.w * g.w, 1.0f, 1e-5f);
}

// Sanity check: direct Euler -> quaternion -> Euler round-trip
TEST(TransformMathTests, EulerQuaternionRoundTrip_Direct)
{
    struct Sample { float x, y, z; };
    const Sample kSamples[] = {
        { 0.0f, 0.0f, 0.0f },
        { 5.0f, 6.0f, 0.0f },
        { 10.0f, -20.0f, 30.0f },
        { -45.0f, 30.0f, -10.0f },
        { 0.0f, 89.0f, 0.0f },
        { 0.0f, -89.0f, 0.0f },
        { 179.0f, 0.0f, 0.0f },
    };

    for (const auto& s : kSamples)
    {
        Quaternion q = QuaternionFromEulerXYZDegrees(s.x, s.y, s.z);

        float rx = 0.0f, ry = 0.0f, rz = 0.0f;
        EulerXYZDegreesFromQuaternion(q, rx, ry, rz);

        ExpectEulerNear(s.x, s.y, s.z, rx, ry, rz, 1e-3f);
    }
}

// Larger and negative angles through TRS, identity scale
TEST(TransformMathTests, TransformTRSRoundTrip_LargeAndNegative_IdentityScale)
{
    struct Sample { float x, y, z; };
    const Sample kSamples[] = {
        { 0.0f, 0.0f, 0.0f },
        { -45.0f, 30.0f, -10.0f },
        { 90.0f, 0.0f, 0.0f },
        { 0.0f, 90.0f, 0.0f },
        { 0.0f, 0.0f, 90.0f },
        { 120.0f, -60.0f, 45.0f },
    };

    Vector3 pos(0.0f, 0.0f, 0.0f);
    Vector3  scale(1.0f, 1.0f, 1.0f);

    for (const auto& s : kSamples)
    {
        Quaternion q = QuaternionFromEulerXYZDegrees(s.x, s.y, s.z);
        Transform t = Transform::FromTRS(pos, q, scale);

        Quaternion q2 = t.GetRotation();
        float rx = 0.0f, ry = 0.0f, rz = 0.0f;
        EulerXYZDegreesFromQuaternion(q2, rx, ry, rz);
        ExpectEulerNear(s.x, s.y, s.z, rx, ry, rz);
    }
}

// TRS should not affect translation when extracting rotation/scale
TEST(TransformMathTests, TransformTRS_PreservesTranslation)
{
    Vector3 pos(3.0f, -4.0f, 5.0f);
    Vector3  scale(2.0f, 3.0f, 4.0f);
    Quaternion q = QuaternionFromEulerXYZDegrees(15.0f, -25.0f, 35.0f);

    Transform t = Transform::FromTRS(pos, q, scale);

    Vector3 outPos = t.GetPosition();
    EXPECT_NEAR(outPos.x, pos.x, 1e-4f);
    EXPECT_NEAR(outPos.y, pos.y, 1e-4f);
    EXPECT_NEAR(outPos.z, pos.z, 1e-4f);
}

// Full pipeline: Euler -> quaternion -> Transform matrix -> quaternion -> Euler
TEST(TransformMathTests, TransformTRSRoundTrip_SmallAngles_IdentityScale)
{
    const float ex = 5.0f;
    const float ey = 6.0f;
    const float ez = 0.0f;

    Vector3 pos(0.0f, 0.0f, 0.0f);
    Vector3 scale(1.0f, 1.0f, 1.0f);

    Quaternion q = QuaternionFromEulerXYZDegrees(ex, ey, ez);
    Transform t = Transform::FromTRS(pos, q, scale);

    Quaternion q2 = t.GetRotation();
    EXPECT_NEAR(q2.GetGLM().x, q.GetGLM().x, 1e-4f);
    EXPECT_NEAR(q2.GetGLM().y, q.GetGLM().y, 1e-4f);
    EXPECT_NEAR(q2.GetGLM().z, q.GetGLM().z, 1e-4f);
    EXPECT_NEAR(q2.GetGLM().w, q.GetGLM().w, 1e-4f);
    float rx = 0.0f, ry = 0.0f, rz = 0.0f;
    EulerXYZDegreesFromQuaternion(q2, rx, ry, rz);

    ExpectEulerNear(ex, ey, ez, rx, ry, rz);
}

TEST(TransformMathTests, TransformTRSRoundTrip_SmallAngles_NonUniformScale)
{
    const float ex = 5.0f;
    const float ey = 6.0f;
    const float ez = 0.0f;

    Vector3 pos(1.0f, -2.0f, 3.0f);
    Vector3 scale(2.0f, 3.0f, 4.0f);

    Quaternion q = QuaternionFromEulerXYZDegrees(ex, ey, ez);
    Transform t = Transform::FromTRS(pos, q, scale);

    Quaternion q2 = t.GetRotation();
    EXPECT_NEAR(q2.GetGLM().x, q.GetGLM().x, 1e-4f);
    EXPECT_NEAR(q2.GetGLM().y, q.GetGLM().y, 1e-4f);
    EXPECT_NEAR(q2.GetGLM().z, q.GetGLM().z, 1e-4f);
    EXPECT_NEAR(q2.GetGLM().w, q.GetGLM().w, 1e-4f);
    float rx = 0.0f, ry = 0.0f, rz = 0.0f;
    EulerXYZDegreesFromQuaternion(q2, rx, ry, rz);

    ExpectEulerNear(ex, ey, ez, rx, ry, rz);
}

	// Full TRS round-trip: build a Transform from TRS, decompose back to
	// components, then rebuild and require the matrices to match.
	TEST(TransformMathTests, TransformTRSFullRoundTrip_MatrixAndComponents)
	{
	    struct TrsCase
	    {
	        const char* name;
	        Vector3     position;
	        Quaternion  rotation;
	        Vector3     scale;
	    };

	    const TrsCase kCases[] = {
	        { "Identity",
	          Vector3(0.0f, 0.0f, 0.0f),
	          Quaternion{},
	          Vector3(1.0f, 1.0f, 1.0f) },
	        { "TranslateOnly",
	          Vector3(1.0f, -2.0f, 3.5f),
	          Quaternion{},
	          Vector3(1.0f, 1.0f, 1.0f) },
	        { "RotateOnly_Y90",
	          Vector3(0.0f, 0.0f, 0.0f),
	          QuaternionFromEulerXYZDegrees(0.0f, 90.0f, 0.0f),
	          Vector3(1.0f, 1.0f, 1.0f) },
	        { "TranslateRotate",
	          Vector3(2.0f, 0.0f, -1.0f),
	          QuaternionFromEulerXYZDegrees(15.0f, -25.0f, 35.0f),
	          Vector3(1.0f, 1.0f, 1.0f) },
	        { "UniformScale",
	          Vector3(0.0f, 0.0f, 0.0f),
	          Quaternion{},
	          Vector3(2.0f, 2.0f, 2.0f) },
	        { "TranslateRotateScale",
	          Vector3(1.0f, -3.0f, 4.0f),
	          QuaternionFromEulerXYZDegrees(10.0f, 20.0f, -15.0f),
	          Vector3(2.0f, 3.0f, 4.0f) },
	    };

	    for (const auto& c : kCases)
	    {
	        SCOPED_TRACE(c.name);

	        Transform t0 = Transform::FromTRS(c.position, c.rotation, c.scale);

	        Vector3 p1 = t0.GetPosition();
	        Quaternion r1 = t0.GetRotation();
	        Vector3 s1 = t0.GetScale();

	        // Components should round-trip.
	        EXPECT_NEAR(p1.x, c.position.x, kPositionEpsilon);
	        EXPECT_NEAR(p1.y, c.position.y, kPositionEpsilon);
	        EXPECT_NEAR(p1.z, c.position.z, kPositionEpsilon);

	        EXPECT_NEAR(s1.x, c.scale.x, kScaleEpsilon);
	        EXPECT_NEAR(s1.y, c.scale.y, kScaleEpsilon);
	        EXPECT_NEAR(s1.z, c.scale.z, kScaleEpsilon);

	        Transform t1 = Transform::FromTRS(p1, r1, s1);

	        for (int i = 0; i < 16; ++i)
	        {
	            EXPECT_NEAR(t0.matrix[i], t1.matrix[i], kMatrixEpsilon);
	        }
	    }
	}

	// Simple hierarchy sanity: a child under a translated root should match
	// manual TRS composition using the same column-major multiply as the
	// TransformHierarchySystem.
	TEST(TransformMathTests, Hierarchy_CompositionMatchesTransformHierarchySystem_TranslateOnly)
	{
	    ECS::World world;

	    ECS::Entity parent = world.Create();
	    ECS::Entity child  = world.Create();

	    Vector3 parentPos(1.0f, 2.0f, 3.0f);
	    Quaternion parentRot{};
	    Vector3 parentScale(1.0f, 1.0f, 1.0f);

	    Vector3 childLocalPos(4.0f, 5.0f, -2.0f);
	    Quaternion childRot{};
	    Vector3 childScale(1.0f, 1.0f, 1.0f);

	    Transform parentLocal = Transform::FromTRS(parentPos, parentRot, parentScale);
	    Transform childLocal  = Transform::FromTRS(childLocalPos, childRot, childScale);

	    parent.Set(parentLocal);
	    Parent parentComponent{};
	    parentComponent.parent = parent.GetHandle();
	    child.Set(childLocal);
	    child.Set(parentComponent);

	    world.ProcessCommands();

	    Engine::Renderer::TransformHierarchySystem system;
	    system.Update(world, 0.0f);

	    auto* parentWorld = world.GetComponent<WorldTransform>(parent.GetHandle());
	    auto* childWorld  = world.GetComponent<WorldTransform>(child.GetHandle());

	    ASSERT_NE(parentWorld, nullptr);
	    ASSERT_NE(childWorld, nullptr);

	    // Root world transform should match its local transform.
	    for (int i = 0; i < 16; ++i)
	    {
	        EXPECT_NEAR(parentWorld->matrix[i], parentLocal.matrix[i], kMatrixEpsilon);
	    }

	    // Child world transform should match manual matrix composition.
	    float32 expectedChildWorld[16] = {};
	    MultiplyColumnMajor4x4Test(parentWorld->matrix, childLocal.matrix, expectedChildWorld);

	    for (int i = 0; i < 16; ++i)
	    {
	        EXPECT_NEAR(childWorld->matrix[i], expectedChildWorld[i], kMatrixEpsilon);
	    }
	}

		// More complex hierarchy case: parent and child both have rotation and
		// non-uniform scale. World transforms from TransformHierarchySystem
		// should still match manual column-major matrix composition.
		TEST(TransformMathTests, Hierarchy_CompositionMatchesTransformHierarchySystem_RotateScale)
		{
		    ECS::World world;

		    ECS::Entity parent = world.Create();
		    ECS::Entity child  = world.Create();

		    Vector3   parentPos(1.0f, -2.0f, 3.0f);
		    Quaternion parentRot = QuaternionFromEulerXYZDegrees(20.0f, -30.0f, 15.0f);
		    Vector3   parentScale(2.0f, 1.5f, 0.5f);

		    Vector3   childLocalPos(-1.0f, 0.5f, 2.0f);
		    Quaternion childRot = QuaternionFromEulerXYZDegrees(-10.0f, 45.0f, 5.0f);
		    Vector3   childScale(0.75f, 1.25f, 1.5f);

		    Transform parentLocal = Transform::FromTRS(parentPos, parentRot, parentScale);
		    Transform childLocal  = Transform::FromTRS(childLocalPos, childRot, childScale);

		    parent.Set(parentLocal);
		    Parent parentComponent{};
		    parentComponent.parent = parent.GetHandle();
		    child.Set(childLocal);
		    child.Set(parentComponent);

		    world.ProcessCommands();

		    Engine::Renderer::TransformHierarchySystem system;
		    system.Update(world, 0.0f);

		    auto* parentWorld = world.GetComponent<WorldTransform>(parent.GetHandle());
		    auto* childWorld  = world.GetComponent<WorldTransform>(child.GetHandle());

		    ASSERT_NE(parentWorld, nullptr);
		    ASSERT_NE(childWorld, nullptr);

		    // Root world transform should still match its local transform.
		    for (int i = 0; i < 16; ++i)
		    {
		        EXPECT_NEAR(parentWorld->matrix[i], parentLocal.matrix[i], kMatrixEpsilon);
		    }

		    // Child world transform should match manual matrix composition.
		    float32 expectedChildWorld[16] = {};
		    MultiplyColumnMajor4x4Test(parentWorld->matrix, childLocal.matrix, expectedChildWorld);

		    for (int i = 0; i < 16; ++i)
		    {
		        EXPECT_NEAR(childWorld->matrix[i], expectedChildWorld[i], kMatrixEpsilon);
		    }
		}

// Cached topology must make ordinary transform edits proportional to the
// affected subtrees, including multi-selection-style batches. A parent and
// child edited in the same frame overlap and therefore traverse their branch
// only once; independent branches remain independent dirty roots.
TEST(TransformMathTests, Hierarchy_CachedTopologyPropagatesCollapsedDirtySubtrees)
{
    ECS::World world(nullptr);
    ECS::Entity root = world.Create(Transform::FromTRS(
        {10.0f, 0.0f, 0.0f}, Quaternion{}, {1, 1, 1}));
    ECS::Entity branchA = world.Create(Transform::FromTRS(
        {0.0f, 2.0f, 0.0f}, Quaternion{}, {1, 1, 1}));
    ECS::Entity leafA = world.Create(Transform::FromTRS(
        {0.0f, 0.0f, 3.0f}, Quaternion{}, {1, 1, 1}));
    ECS::Entity branchB = world.Create(Transform::FromTRS(
        {0.0f, -2.0f, 0.0f}, Quaternion{}, {1, 1, 1}));
    ECS::Entity leafB = world.Create(Transform::FromTRS(
        {0.0f, 0.0f, -3.0f}, Quaternion{}, {1, 1, 1}));

    Parent toRoot{}; toRoot.parent = root.GetHandle();
    Parent toA{}; toA.parent = branchA.GetHandle();
    Parent toB{}; toB.parent = branchB.GetHandle();
    branchA.Set(toRoot);
    branchB.Set(toRoot);
    leafA.Set(toA);
    leafB.Set(toB);
    world.ProcessCommands();

    Engine::Renderer::TransformHierarchySystem hierarchy;
    hierarchy.Update(world, 0.0f);
    EXPECT_EQ(hierarchy.GetTopologyBuildCount(), 1u);
    EXPECT_EQ(hierarchy.GetLastPropagatedNodeCount(), 5u);

    // A clean hierarchy frame neither rebuilds nor walks any node.
    hierarchy.Update(world, 0.0f);
    EXPECT_EQ(hierarchy.GetTopologyBuildCount(), 1u);
    EXPECT_EQ(hierarchy.GetLastPropagatedNodeCount(), 0u);

    // One leaf edit is one propagated node; the other branch stays untouched.
    const uint32 leafBBefore =
        world.GetComponent<WorldTransform>(leafB.GetHandle())->Version;
    world.GetComponentForWrite<Transform>(leafA.GetHandle())->Translate(1.0f, 0.0f, 0.0f);
    hierarchy.Update(world, 0.0f);
    EXPECT_EQ(hierarchy.GetTopologyBuildCount(), 1u);
    EXPECT_EQ(hierarchy.GetLastPropagatedNodeCount(), 1u);
    EXPECT_EQ(world.GetComponent<WorldTransform>(leafB.GetHandle())->Version,
              leafBBefore);

    // Two independent selected leaves produce two dirty roots.
    world.GetComponentForWrite<Transform>(leafA.GetHandle())->Translate(1.0f, 0.0f, 0.0f);
    world.GetComponentForWrite<Transform>(leafB.GetHandle())->Translate(-1.0f, 0.0f, 0.0f);
    hierarchy.Update(world, 0.0f);
    EXPECT_EQ(hierarchy.GetLastPropagatedNodeCount(), 2u);

    // A selected parent plus its selected child collapses to the parent's
    // two-node subtree instead of applying the leaf twice.
    world.GetComponentForWrite<Transform>(branchA.GetHandle())->Translate(0.0f, 1.0f, 0.0f);
    world.GetComponentForWrite<Transform>(leafA.GetHandle())->Translate(0.0f, 0.0f, 1.0f);
    hierarchy.Update(world, 0.0f);
    EXPECT_EQ(hierarchy.GetTopologyBuildCount(), 1u);
    EXPECT_EQ(hierarchy.GetLastPropagatedNodeCount(), 2u);

    // A root edit necessarily visits the entire five-node hierarchy.
    world.GetComponentForWrite<Transform>(root.GetHandle())->Translate(1.0f, 0.0f, 0.0f);
    hierarchy.Update(world, 0.0f);
    EXPECT_EQ(hierarchy.GetTopologyBuildCount(), 1u);
    EXPECT_EQ(hierarchy.GetLastPropagatedNodeCount(), 5u);

    // Parent data edits and unrelated structural moves both invalidate cached
    // pointers/topology and force exactly one rebuild.
    world.GetComponentForWrite<Parent>(leafA.GetHandle())->parent = branchB.GetHandle();
    hierarchy.Update(world, 0.0f);
    EXPECT_EQ(hierarchy.GetTopologyBuildCount(), 2u);
    const WorldTransform* reparented =
        world.GetComponent<WorldTransform>(leafA.GetHandle());
    ASSERT_NE(reparented, nullptr);
    EXPECT_FLOAT_EQ(reparented->matrix[12], 13.0f);
    EXPECT_FLOAT_EQ(reparented->matrix[13], -2.0f);
    EXPECT_FLOAT_EQ(reparented->matrix[14], 4.0f);

    root.Set(HierarchyOrder{7});
    world.ProcessCommands();
    hierarchy.Update(world, 0.0f);
    EXPECT_EQ(hierarchy.GetTopologyBuildCount(), 3u);
}

// Entities without a Parent resolve on the flat pass even when a hierarchy
// exists elsewhere in the world: one parented pair must not send a thousand
// unrelated roots down the hierarchy walk.
TEST(TransformMathTests, Hierarchy_UnparentedRootsTakeTheFlatPassBesideAHierarchy)
{
    constexpr int kRoots = 1000;
    ECS::World world(nullptr);
    ECS::Entity parent = world.Create(Transform::FromTRS({0.0f, 0.0f, 0.0f}, Quaternion{}, {1, 1, 1}));
    ECS::Entity child = world.Create(Transform::FromTRS({1.0f, 0.0f, 0.0f}, Quaternion{}, {1, 1, 1}));
    child.Set(Parent{parent.GetHandle()});
    std::vector<ECS::Entity> roots;
    for (int i = 0; i < kRoots; ++i)
        roots.push_back(world.Create(Transform::FromTRS(
            {static_cast<float>(i), 0.0f, 0.0f}, Quaternion{}, {1, 1, 1})));
    world.ProcessCommands();

    Engine::Renderer::TransformHierarchySystem hierarchy;
    hierarchy.Update(world, 0.0f);
    hierarchy.Update(world, 0.0f);

    for (ECS::Entity& root : roots)
        world.GetComponentForWrite<Transform>(root.GetHandle())->Translate(0.0f, 1.0f, 0.0f);
    hierarchy.Update(world, 0.0f);
    EXPECT_EQ(hierarchy.GetLastPropagatedNodeCount(), 0u)
        << "unparented roots walked the hierarchy instead of the flat pass";
    for (ECS::Entity& root : roots)
    {
        const auto* local = world.GetComponent<Transform>(root.GetHandle());
        const auto* worldTransform = world.GetComponent<WorldTransform>(root.GetHandle());
        ASSERT_NE(worldTransform, nullptr);
        EXPECT_EQ(0, std::memcmp(worldTransform->matrix, local->matrix, sizeof(local->matrix)));
    }
}

// Every unit a small hierarchy (a root with a child, the shape an imported
// model has): a still frame walks nothing, and moving every root walks each
// root's two-node subtree once, on the job system.
TEST(TransformMathTests, Hierarchy_EveryUnitAHierarchyStillWalksNothingAndMovesWalkOnce)
{
    constexpr int kUnits = 2000; // past the parallel-propagation threshold
    JobSystem::WorkStealingThreadPool pool(4);
    ECS::World world(&pool);
    std::vector<ECS::Entity> roots, children;
    for (int i = 0; i < kUnits; ++i)
    {
        roots.push_back(world.Create(Transform::FromTRS(
            {static_cast<float>(i), 0.0f, 0.0f}, Quaternion{}, {1, 1, 1})));
        ECS::Entity child = world.Create(Transform::FromTRS({0.0f, 1.0f, 0.0f}, Quaternion{}, {1, 1, 1}));
        child.Set(Parent{roots.back().GetHandle()});
        children.push_back(child);
    }
    world.ProcessCommands();

    Engine::Renderer::TransformHierarchySystem hierarchy;
    hierarchy.Update(world, 0.0f);
    hierarchy.Update(world, 0.0f);
    EXPECT_EQ(hierarchy.GetLastPropagatedNodeCount(), 0u) << "a still frame walked the hierarchy";

    for (ECS::Entity& root : roots)
        world.GetComponentForWrite<Transform>(root.GetHandle())->Translate(0.0f, 0.0f, 2.0f);
    hierarchy.Update(world, 0.0f);
    EXPECT_EQ(hierarchy.GetLastPropagatedNodeCount(), static_cast<size_t>(2 * kUnits));
    for (int i = 0; i < kUnits; ++i)
    {
        const auto* childWorld = world.GetComponent<WorldTransform>(children[i].GetHandle());
        ASSERT_NE(childWorld, nullptr);
        EXPECT_FLOAT_EQ(childWorld->matrix[12], static_cast<float>(i));
        EXPECT_FLOAT_EQ(childWorld->matrix[13], 1.0f);
        EXPECT_FLOAT_EQ(childWorld->matrix[14], 2.0f);
    }
}

namespace
{
// One random forest, built identically into any number of worlds: roots with
// and without children, chains up to six deep, and Parent components whose
// handle is unset (roots that the hierarchy pass, not the flat pass, owns).
struct ForestSpec
{
    std::vector<int> parentOf; // -1: no Parent component, -2: a Parent with an unset handle
    std::vector<Transform> locals;
};

ForestSpec MakeForest(std::mt19937& rng, int count)
{
    std::uniform_real_distribution<float> offset(-5.0f, 5.0f);
    std::uniform_real_distribution<float> angle(-180.0f, 180.0f);
    std::uniform_real_distribution<float> scale(0.5f, 2.0f);
    std::uniform_int_distribution<int> kind(0, 9);
    ForestSpec spec;
    std::vector<int> depth;
    for (int i = 0; i < count; ++i)
    {
        int parent = -1;
        const int k = kind(rng);
        if (k == 0)
            parent = -2;
        else if (k >= 4 && i > 0)
        {
            const int candidate = std::uniform_int_distribution<int>(0, i - 1)(rng);
            if (depth[candidate] < 5)
                parent = candidate;
        }
        spec.parentOf.push_back(parent);
        depth.push_back(parent >= 0 ? depth[parent] + 1 : 0);
        spec.locals.push_back(Transform::FromTRS(
            {offset(rng), offset(rng), offset(rng)},
            QuaternionFromEulerXYZDegrees(angle(rng), angle(rng), angle(rng)),
            {scale(rng), scale(rng), scale(rng)}));
    }
    return spec;
}

std::vector<ECS::EntityHandle> BuildForest(ECS::World& world, const ForestSpec& spec)
{
    std::vector<ECS::EntityHandle> handles;
    for (const Transform& local : spec.locals)
        handles.push_back(world.Create(local).GetHandle());
    for (size_t i = 0; i < spec.parentOf.size(); ++i)
    {
        if (spec.parentOf[i] >= 0)
            world.AddComponentImmediate<Parent>(handles[i], Parent{handles[spec.parentOf[i]]});
        else if (spec.parentOf[i] == -2)
            world.AddComponentImmediate<Parent>(handles[i], Parent{});
    }
    world.ProcessCommands();
    return handles;
}

// The forest's world matrices by direct recursion over the ECS data.
void ReferenceWorld(const ECS::World& world, const std::vector<ECS::EntityHandle>& handles,
                    const ForestSpec& spec, std::vector<std::array<float32, 16>>& out)
{
    out.assign(handles.size(), {});
    std::vector<uint8_t> done(handles.size(), 0u);
    std::vector<size_t> stack;
    for (size_t start = 0; start < handles.size(); ++start)
    {
        for (size_t at = start; done[at] == 0u;)
        {
            const int parent = spec.parentOf[at];
            if (parent >= 0 && done[static_cast<size_t>(parent)] == 0u)
            {
                stack.push_back(at);
                at = static_cast<size_t>(parent);
                continue;
            }
            const float32* local = world.GetComponent<Transform>(handles[at])->matrix;
            if (parent >= 0)
                MultiplyColumnMajor4x4Test(out[static_cast<size_t>(parent)].data(), local, out[at].data());
            else
                std::memcpy(out[at].data(), local, sizeof(float32) * 16);
            done[at] = 1u;
            if (stack.empty())
                break;
            at = stack.back();
            stack.pop_back();
        }
    }
}
} // namespace

// I4: the split paths (flat pass for unparented entities, parallel topmost-
// subtree propagation for the rest) produce world matrices bitwise equal to
// the all-serial cached path, and equal to a direct recursion, across random
// moves, a reparenting and frames where hundreds of roots move at once. The
// forest carries one 200-link chain (skeletons and ropes), so the deep walk
// and the topmost filter's climb run on a long spine.
//
// The two legs guard different things: the bitwise leg compares the split
// paths against the same code's serial seam, so it cannot see a math error
// they share; the NEAR leg against a direct recursion is the one that catches
// a wrong multiply order. Neither is redundant.
TEST(TransformMathTests, Hierarchy_SplitPathsMatchTheSerialPathOnRandomForests)
{
    constexpr int kRandomEntities = 3000;
    constexpr int kChainLength = 200;
    constexpr int kFrames = 8;
    std::mt19937 rng(3355u);
    ForestSpec spec = MakeForest(rng, kRandomEntities);
    for (int link = 0; link < kChainLength; ++link)
    {
        // The chain's first link has no Parent (a root the flat pass owns).
        spec.parentOf.push_back(link == 0 ? -1 : static_cast<int>(spec.parentOf.size()) - 1);
        spec.locals.push_back(Transform::FromTRS({0.0f, 0.5f, 0.1f},
                                                 Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), 0.05f),
                                                 {1.0f, 1.0f, 1.0f}));
    }
    const int entityCount = static_cast<int>(spec.parentOf.size());

    JobSystem::WorkStealingThreadPool pool(4);
    ECS::World splitWorld(&pool);
    ECS::World serialWorld(nullptr);
    const auto splitHandles = BuildForest(splitWorld, spec);
    const auto serialHandles = BuildForest(serialWorld, spec);
    Engine::Renderer::TransformHierarchySystem split;
    Engine::Renderer::TransformHierarchySystem serial(true);

    std::uniform_int_distribution<int> pick(0, entityCount - 1);
    std::uniform_real_distribution<float> step(-1.0f, 1.0f);
    std::vector<std::array<float32, 16>> reference;
    for (int frame = 0; frame < kFrames; ++frame)
    {
        // Frame 3 moves a third of everything (past the parallel threshold);
        // the others move a handful. Frame 5 reparents one entity.
        const int moves = frame == 3 ? entityCount / 3 : 20;
        for (int m = 0; m < moves; ++m)
        {
            const int i = pick(rng);
            const float dx = step(rng), dy = step(rng), dz = step(rng);
            splitWorld.GetComponentForWrite<Transform>(splitHandles[i])->Translate(dx, dy, dz);
            serialWorld.GetComponentForWrite<Transform>(serialHandles[i])->Translate(dx, dy, dz);
        }
        if (frame == 5)
        {
            for (int i = entityCount - 1; i > 0; --i)
            {
                if (spec.parentOf[i] >= 0)
                {
                    splitWorld.GetComponentForWrite<Parent>(splitHandles[i])->parent = splitHandles[0];
                    serialWorld.GetComponentForWrite<Parent>(serialHandles[i])->parent = serialHandles[0];
                    break;
                }
            }
        }
        split.Update(splitWorld, 0.0f);
        serial.Update(serialWorld, 0.0f);

        for (int i = 0; i < entityCount; ++i)
        {
            const auto* a = splitWorld.GetComponent<WorldTransform>(splitHandles[i]);
            const auto* b = serialWorld.GetComponent<WorldTransform>(serialHandles[i]);
            ASSERT_NE(a, nullptr);
            ASSERT_NE(b, nullptr);
            ASSERT_EQ(0, std::memcmp(a->matrix, b->matrix, sizeof(a->matrix)))
                << "frame " << frame << " entity " << i << " differs from the serial path";
        }
    }

    ForestSpec settled = spec;
    for (int i = entityCount - 1; i > 0; --i)
        if (settled.parentOf[i] >= 0) { settled.parentOf[i] = 0; break; }
    ReferenceWorld(splitWorld, splitHandles, settled, reference);
    for (int i = 0; i < entityCount; ++i)
    {
        const float32* actual = splitWorld.GetComponent<WorldTransform>(splitHandles[i])->matrix;
        for (int e = 0; e < 16; ++e)
            ASSERT_NEAR(actual[e], reference[i][e], 1e-3f * (1.0f + std::fabs(reference[i][e])))
                << "entity " << i << " element " << e;
    }
}

TEST(TransformMathTests, DISABLED_Hierarchy_CachedTopologyPerformanceProbe)
{
    constexpr size_t kRoots = 1000;
    constexpr size_t kEntities = 1'000'000;
    ECS::WorldConfig config{};
    config.ExpectedEntityCount = kEntities;
    ECS::World world(config, nullptr);
    std::vector<ECS::EntityHandle> roots;
    std::vector<ECS::EntityHandle> leaves;
    roots.reserve(kRoots);
    leaves.reserve(kEntities - kRoots);

    for (size_t index = 0; index < kRoots; ++index) {
        roots.push_back(world.Create(Transform::FromTRS(
            {static_cast<float>(index), 0.0f, 0.0f}, Quaternion{}, {1, 1, 1})).GetHandle());
    }
    for (size_t index = kRoots; index < kEntities; ++index) {
        ECS::Entity entity = world.Create(Transform::FromTRS(
            {0.0f, static_cast<float>(index % 17), 0.0f}, Quaternion{}, {1, 1, 1}));
        entity.Set(Parent{roots[index % kRoots]});
        leaves.push_back(entity.GetHandle());
    }
    world.ProcessCommands();

    Engine::Renderer::TransformHierarchySystem hierarchy;
    auto milliseconds = [](auto&& operation) {
        const auto start = std::chrono::steady_clock::now();
        operation();
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
    };
    const double firstBuildMs = milliseconds([&] { hierarchy.Update(world, 0.0f); });

    auto median = [&](auto&& operation, int runs) {
        std::vector<double> samples;
        samples.reserve(static_cast<size_t>(runs));
        for (int run = 0; run < runs; ++run)
            samples.push_back(milliseconds(operation));
        std::sort(samples.begin(), samples.end());
        return samples[samples.size() / 2];
    };

    const double cleanMs = median([&] { hierarchy.Update(world, 0.0f); }, 21);
    size_t leafStep = 0;
    const double leafMs = median([&] {
        world.GetComponentForWrite<Transform>(leaves[leafStep++ % leaves.size()])
            ->Translate(0.001f, 0.0f, 0.0f);
        hierarchy.Update(world, 0.0f);
        EXPECT_EQ(hierarchy.GetLastPropagatedNodeCount(), 1u);
    }, 21);
    size_t rootStep = 0;
    const double branchMs = median([&] {
        world.GetComponentForWrite<Transform>(roots[rootStep++ % roots.size()])
            ->Translate(0.001f, 0.0f, 0.0f);
        hierarchy.Update(world, 0.0f);
        EXPECT_EQ(hierarchy.GetLastPropagatedNodeCount(), 1000u);
    }, 11);
    bool toggle = false;
    const double rebuildMs = median([&] {
        world.GetComponentForWrite<Parent>(leaves.front())->parent =
            roots[toggle ? 0 : 1];
        toggle = !toggle;
        hierarchy.Update(world, 0.0f);
    }, 7);

    std::printf(
        "\n[TransformHierarchyPerf] entities=%zu firstBuild=%.3fms clean=%.3fms "
        "leaf=%.3fms branch1000=%.3fms topologyRebuild=%.3fms\n",
        kEntities, firstBuildMs, cleanMs, leafMs, branchMs, rebuildMs);
}

// Change-signaling contract for hierarchy-propagated moves. The hierarchy's
// incremental path writes WorldTransform through pointers cached at topology
// rebuild, so nothing in that write is a query visit — the per-(chunk, column)
// write-grant stamp that Changed<WorldTransform> consumers filter on has to be
// issued explicitly. Consumers that gate on it (the terrain modifier system's
// dirty scan) go blind to every hierarchy-driven move without it, which is the
// difference between moving an entity from the inspector (Transform only) and
// from the viewport gizmo (which writes WorldTransform itself).
//
// Both propagation branches are covered: a child recomposed against its parent
// and a root copied straight from its local matrix.
TEST(TransformMathTests, HierarchyPropagationStampsWorldTransformForChangedGate)
{
    ECS::World world;

    ECS::Entity root  = world.Create(Transform::FromTRS(
        {0.0f, 0.0f, 0.0f}, Quaternion{}, {1, 1, 1}));
    ECS::Entity child = world.Create(Transform::FromTRS(
        {1.0f, 0.0f, 0.0f}, Quaternion{}, {1, 1, 1}));
    Parent toRoot{}; toRoot.parent = root.GetHandle();
    child.Set(toRoot);
    world.ProcessCommands();

    Engine::Renderer::TransformHierarchySystem hierarchy;
    hierarchy.Update(world, 0.0f); // topology build + first propagation
    hierarchy.Update(world, 0.0f); // quiesce: nothing dirty, nothing walked
    ASSERT_EQ(hierarchy.GetTopologyBuildCount(), 1u);
    ASSERT_EQ(hierarchy.GetLastPropagatedNodeCount(), 0u);

    // Entities a Changed<WorldTransform>-gated scan visits since `gate`. The
    // callback binds WorldTransform as const, so the probe classifies the slot
    // as a read and never stamps what it is measuring.
    auto movedSince = [&world](uint64 gate) {
        ECS::ChangeGate g;
        g.LastRunVersion = gate;
        std::vector<ECS::EntityHandle> visited;
        auto q = world.Query<ECS::Read<WorldTransform>>();
        q.Changed<WorldTransform>(g);
        q.Each([&](ECS::EntityHandle e, const WorldTransform&) { visited.push_back(e); });
        return visited;
    };

    auto contains = [](const std::vector<ECS::EntityHandle>& handles,
                       ECS::EntityHandle wanted) {
        return std::find(handles.begin(), handles.end(), wanted) != handles.end();
    };

    // Positive control: the probe must be capable of seeing a stamped write.
    // A write grant on an untouched entity is the known event injected into
    // the measurement channel — without it a zero below would be unreadable.
    {
        const uint64 gate = world.GetGlobalSystemVersion();
        ASSERT_NE(world.GetComponentForWrite<WorldTransform>(root.GetHandle()), nullptr);
        EXPECT_TRUE(contains(movedSince(gate), root.GetHandle()))
            << "probe cannot see a write-granted WorldTransform; the gate below proves nothing";
    }

    // Child branch: a local Transform edit (the inspector's write shape — a
    // data-only component set, no archetype move, so no topology rebuild).
    {
        const uint64 gate = world.GetGlobalSystemVersion();
        const uint32 versionBefore =
            world.GetComponent<WorldTransform>(child.GetHandle())->Version;

        Transform moved = Transform::FromTRS({7.0f, 0.0f, 0.0f}, Quaternion{}, {1, 1, 1});
        world.AddComponentImmediate(child.GetHandle(), moved);
        hierarchy.Update(world, 0.0f);

        ASSERT_EQ(hierarchy.GetTopologyBuildCount(), 1u) << "test drifted: a rebuild stamps everything";
        ASSERT_EQ(hierarchy.GetLastPropagatedNodeCount(), 1u);
        ASSERT_GT(world.GetComponent<WorldTransform>(child.GetHandle())->Version, versionBefore)
            << "propagation did not run; the stamp assertion below would be vacuous";

        EXPECT_TRUE(contains(movedSince(gate), child.GetHandle()))
            << "hierarchy recomposed the child's WorldTransform without stamping its column";
    }

    // Root branch: the same edit applied to the parent, which propagates
    // through the root path (local matrix copied straight into WorldTransform).
    {
        const uint64 gate = world.GetGlobalSystemVersion();
        const uint32 versionBefore =
            world.GetComponent<WorldTransform>(root.GetHandle())->Version;

        Transform moved = Transform::FromTRS({0.0f, 5.0f, 0.0f}, Quaternion{}, {1, 1, 1});
        world.AddComponentImmediate(root.GetHandle(), moved);
        hierarchy.Update(world, 0.0f);

        ASSERT_EQ(hierarchy.GetTopologyBuildCount(), 1u);
        ASSERT_GT(world.GetComponent<WorldTransform>(root.GetHandle())->Version, versionBefore);

        const std::vector<ECS::EntityHandle> moved2 = movedSince(gate);
        EXPECT_TRUE(contains(moved2, root.GetHandle()))
            << "hierarchy rewrote the root's WorldTransform without stamping its column";
        EXPECT_TRUE(contains(moved2, child.GetHandle()))
            << "a root move must signal the subtree it recomputed";
    }
}
