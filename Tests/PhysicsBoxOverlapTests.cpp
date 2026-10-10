#include <gtest/gtest.h>
#include "Physics/PhysicsWorld.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>

using namespace GameEngine;
using namespace GameEngine::Physics;

namespace
{
class PhysicsBoxOverlapTests : public ::testing::Test
{
protected:
    std::unique_ptr<PhysicsWorld> world;
    std::vector<OverlapResult> hits;

    void SetUp() override
    {
        PhysicsWorldSettings settings;
        settings.numThreads = 1;
        settings.maxBodies = settings.maxBodyPairs = settings.maxContactConstraints = 1024;
        settings.tempAllocatorBytes = 16u * 1024u * 1024u;
        world = std::make_unique<PhysicsWorld>(settings);
    }

    BodyHandle Body(const ShapeDefinition& definition, const Vector3& position = {}, uint64 data = 101,
                    CollisionLayer layer = Layers::Static, bool sensor = false,
                    const Quaternion& rotation = Quaternion::Identity())
    {
        BodySettings settings;
        settings.shape = world->CreateShape(definition);
        if (!settings.shape.IsValid()) throw std::runtime_error("actual shape creation failed");
        settings.position = position;
        settings.rotation = rotation;
        settings.motionType = MotionType::Static;
        settings.layer = layer;
        settings.isSensor = sensor;
        settings.userData = data;
        auto body = world->CreateBody(settings);
        if (!body.IsValid()) throw std::runtime_error("actual body creation failed");
        return body;
    }

    BodyHandle Box(Vector3 position, Vector3 half = {.5f, .5f, .5f}, uint64 data = 101,
                   CollisionLayer layer = Layers::Static, bool sensor = false,
                   const Quaternion& rotation = Quaternion::Identity())
    {
        BoxShapeDef box;
        box.halfExtents = half;
        return Body(box, position, data, layer, sensor, rotation);
    }

    BodyHandle Terrain(float low)
    {
        HeightFieldShapeDef terrain;
        terrain.SampleCount = 32;
        terrain.Samples.assign(32 * 32, low);
        terrain.Offset = {-16, 0, -16};
        terrain.Scale = {1, 1, 1};
        for (unsigned z = 24; z < 32; ++z)
            for (unsigned x = 24; x < 32; ++x)
                terrain.Samples[z * 32 + x] = 20;
        return Body(terrain, {}, 900);
    }

    BoxOverlapQuery Query(Vector3 center = {}, Vector3 half = {.3f, .3f, .3f})
    {
        BoxOverlapQuery q;
        q.center = center;
        q.halfExtents = half;
        return q;
    }

    void Only(BodyHandle body, uint64 data)
    {
        ASSERT_EQ(hits.size(), 1u);
        EXPECT_EQ(hits[0].body, body);
        EXPECT_EQ(hits[0].userData, data);
    }

    void GroundAt(Vector3 point, BodyHandle expected, float height)
    {
        RayCastQuery ray;
        ray.ray.origin = {point.x, 30, point.z};
        ray.ray.direction = {0, -1, 0};
        ray.maxDistance = 40;
        RayCastResult result;
        ASSERT_TRUE(world->RayCast(ray, result));
        EXPECT_EQ(result.body, expected);
        EXPECT_NEAR(result.hitPoint.y, height, .01f);
    }
};

TEST_F(PhysicsBoxOverlapTests, LowPadBelowRemotePeakOnSameHeightfieldIsClear)
{
    const auto terrain = Terrain(4);
    ASSERT_NO_FATAL_FAILURE(GroundAt({-10, 0, -10}, terrain, 4));
    ASSERT_NO_FATAL_FAILURE(GroundAt({12, 0, 12}, terrain, 20));
    EXPECT_FALSE(world->BoxOverlap(Query({-10, 5.1f, -10}, {.3f, 1, .3f}), hits));
    EXPECT_TRUE(hits.empty());
}

TEST_F(PhysicsBoxOverlapTests, ActualTerrainPenetrationStillHits)
{
    const auto terrain = Terrain(4);
    EXPECT_TRUE(world->BoxOverlap(Query({-10, 4.1f, -10}), hits));
    ASSERT_NO_FATAL_FAILURE(Only(terrain, 900));
    EXPECT_TRUE(world->BoxOverlap(Query({12, 20.1f, 12}), hits));
    ASSERT_NO_FATAL_FAILURE(Only(terrain, 900));
}

TEST_F(PhysicsBoxOverlapTests, RealObstacleOverLowPadRetainsOnlyObstacleMetadata)
{
    Terrain(4);
    const auto obstacle = Box({-10, 5, -10}, {.4f, .5f, .4f}, 0xFEDCBA9876543210ull);
    EXPECT_TRUE(world->BoxOverlap(Query({-10, 5.1f, -10}, {.3f, 1, .3f}), hits));
    ASSERT_NO_FATAL_FAILURE(Only(obstacle, 0xFEDCBA9876543210ull));
}

TEST_F(PhysicsBoxOverlapTests, ElevatedBridgeHasClearanceAboveDeckAndDetectsCeiling)
{
    Terrain(0);
    const auto bridge = Box({0, 3.75f, 0}, {3, .25f, 1}, 200);
    ASSERT_NO_FATAL_FAILURE(GroundAt({0, 0, 0}, bridge, 4));
    const auto clearance = Query({0, 5.1f, 0}, {.3f, 1, .3f});
    EXPECT_FALSE(world->BoxOverlap(clearance, hits));
    EXPECT_TRUE(hits.empty());
    EXPECT_TRUE(world->BoxOverlap(Query({0, 3.9f, 0}), hits));
    ASSERT_NO_FATAL_FAILURE(Only(bridge, 200));
    const auto ceiling = Box({0, 6, 0}, {1, .15f, 1}, 201);
    EXPECT_TRUE(world->BoxOverlap(clearance, hits));
    ASSERT_NO_FATAL_FAILURE(Only(ceiling, 201));
}

TEST_F(PhysicsBoxOverlapTests, RotatedThinQueryFindsItsActualLongAxis)
{
    const auto along = Box({1.8f, 0, -1.8f}, {.15f, .15f, .15f}, 301);
    Box({1.8f, 0, 1.8f}, {.15f, .15f, .15f}, 302);
    auto query = Query({}, {3, .2f, .1f});
    query.rotation = Quaternion::FromAxisAngle({0, 1, 0}, .78539816339f);
    EXPECT_TRUE(world->BoxOverlap(query, hits));
    ASSERT_NO_FATAL_FAILURE(Only(along, 301));
}

TEST_F(PhysicsBoxOverlapTests, RotatedThinTargetRejectsEmptyPartOfItsBounds)
{
    const auto thin = Box({}, {3, .2f, .1f}, 401, Layers::Static, false,
                          Quaternion::FromAxisAngle({0, 1, 0}, .78539816339f));
    EXPECT_FALSE(world->BoxOverlap(Query({1.8f, 0, 1.8f}, {.1f, .1f, .1f}), hits));
    EXPECT_TRUE(hits.empty());
    EXPECT_TRUE(world->BoxOverlap(Query({1.8f, 0, -1.8f}, {.1f, .1f, .1f}), hits));
    ASSERT_NO_FATAL_FAILURE(Only(thin, 401));
}

TEST_F(PhysicsBoxOverlapTests, QueryLayerMaskAndSensorPolicyApplyBeforeLimit)
{
    Box({}, {.5f, .5f, .5f}, 501, Layers::Static);
    const auto sensor = Box({}, {.5f, .5f, .5f}, 502, Layers::Trigger, true);
    const auto custom = Box({}, {.5f, .5f, .5f}, 503, 31);
    auto query = Query();
    query.filter.layerMask = 1u << 31;
    ASSERT_TRUE(world->BoxOverlap(query, hits, 1));
    ASSERT_NO_FATAL_FAILURE(Only(custom, 503));
    query.filter.layerMask = 1u << Layers::Trigger;
    ASSERT_TRUE(world->BoxOverlap(query, hits));
    ASSERT_NO_FATAL_FAILURE(Only(sensor, 502));
    query.filter.ignoreSensors = true;
    EXPECT_FALSE(world->BoxOverlap(query, hits));
    EXPECT_TRUE(hits.empty());
    query.filter.layerMask = 0;
    EXPECT_FALSE(world->BoxOverlap(query, hits));
    EXPECT_TRUE(hits.empty());
}

TEST_F(PhysicsBoxOverlapTests, QueryDoesNotUseSimulationLayerPairMatrix)
{
    world.reset();
    PhysicsWorldSettings settings;
    settings.numThreads = 1;
    settings.layerCollisionMatrix.SetAll(false);
    world = std::make_unique<PhysicsWorld>(settings);
    const auto body = Box({}, {.5f, .5f, .5f}, 601, 8);
    auto query = Query();
    query.filter.layerMask = 1u << 8;
    EXPECT_TRUE(world->BoxOverlap(query, hits));
    ASSERT_NO_FATAL_FAILURE(Only(body, 601));
}

TEST_F(PhysicsBoxOverlapTests, CompoundContactsAreUniqueBodiesAndLimitsClearOutput)
{
    BoxShapeDef child;
    child.halfExtents = {.5f, .5f, .5f};
    const auto childShape = world->CreateShape(child);
    ASSERT_TRUE(childShape.IsValid());
    CompoundShapeDef compound;
    compound.children.push_back({childShape, {-.4f, 0, 0}});
    compound.children.push_back({childShape, {.4f, 0, 0}});
    const auto compoundBody = Body(compound, {}, 701);
    const auto other = Box({0, 0, .5f}, {.5f, .5f, .5f}, 702);
    const auto query = Query({}, {2, 2, 2});
    EXPECT_TRUE(world->BoxOverlap(query, hits));
    ASSERT_EQ(hits.size(), 2u);
    std::set<uint64> ids;
    for (const auto& hit : hits)
    {
        ids.insert(hit.body.Value());
        EXPECT_EQ(hit.userData, hit.body == compoundBody ? 701u : 702u);
    }
    EXPECT_EQ(ids, (std::set<uint64>{compoundBody.Value(), other.Value()}));
    EXPECT_TRUE(world->BoxOverlap(query, hits, 1));
    EXPECT_EQ(hits.size(), 1u);
    EXPECT_FALSE(world->BoxOverlap(query, hits, 0));
    EXPECT_TRUE(hits.empty());
    EXPECT_TRUE(world->BoxOverlap(query, hits, 2));
    EXPECT_EQ(hits.size(), 2u);
}

TEST_F(PhysicsBoxOverlapTests, MeshCeilingIntersectsFromEitherSideAndDeduplicatesTriangles)
{
    MeshShapeDef mesh;
    mesh.vertices = {{-2, 4, -2}, {2, 4, -2}, {2, 4, 2}, {-2, 4, 2}};
    mesh.indices = {0, 2, 1, 0, 3, 2};
    const auto ceiling = Body(mesh, {}, 801);
    for (float y : {3.9f, 4.1f})
    {
        EXPECT_TRUE(world->BoxOverlap(Query({0, y, 0}, {.5f, .2f, .5f}), hits));
        ASSERT_NO_FATAL_FAILURE(Only(ceiling, 801));
    }
    EXPECT_FALSE(world->BoxOverlap(Query({0, 3, 0}), hits));
}

TEST_F(PhysicsBoxOverlapTests, UnmappedCharacterBodiesRetainDistinctUserData)
{
    for (uint64 data : {901u, 902u})
    {
        CharacterSettings character;
        character.motor = CharacterMotor::Dynamic;
        character.userData = data;
        character.position = {data == 901 ? -.5f : .5f, 0, 0};
        ASSERT_TRUE(world->CreateCharacter(character).IsValid());
    }
    EXPECT_TRUE(world->BoxOverlap(Query({0, 1, 0}, {2, 2, 2}), hits));
    ASSERT_EQ(hits.size(), 2u);
    std::set<uint64> data;
    for (const auto& hit : hits)
    {
        EXPECT_FALSE(hit.body.IsValid());
        data.insert(hit.userData);
    }
    EXPECT_EQ(data, (std::set<uint64>{901, 902}));
}

TEST_F(PhysicsBoxOverlapTests, BodyMovementAndRemovalDoNotReturnStaleMetadata)
{
    const auto body = Box({}, {.5f, .5f, .5f}, 1001);
    EXPECT_TRUE(world->BoxOverlap(Query(), hits));
    ASSERT_NO_FATAL_FAILURE(Only(body, 1001));
    auto transform = world->GetBodyTransform(body);
    transform.position = {10, 0, 0};
    world->SetBodyTransform(body, transform);
    EXPECT_FALSE(world->BoxOverlap(Query(), hits));
    EXPECT_TRUE(hits.empty());
    EXPECT_TRUE(world->BoxOverlap(Query({10, 0, 0}), hits));
    ASSERT_NO_FATAL_FAILURE(Only(body, 1001));
    world->DestroyBody(body);
    EXPECT_FALSE(world->BoxOverlap(Query({10, 0, 0}), hits));
    EXPECT_TRUE(hits.empty());
}

TEST_F(PhysicsBoxOverlapTests, FiniteScaledQuaternionsRepresentSameRotation)
{
    const auto body = Box({}, {.5f, .5f, .5f}, 1101);
    for (float w : {1.0f, 1.0e30f, 1.0e-30f, -1.0e30f})
    {
        auto query = Query();
        query.rotation = Quaternion(w, 0, 0, 0);
        EXPECT_TRUE(world->BoxOverlap(query, hits));
        ASSERT_NO_FATAL_FAILURE(Only(body, 1101));
    }
}

TEST_F(PhysicsBoxOverlapTests, InvalidQueryClearsPriorResults)
{
    Box({});
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (int i = 0; i < 7; ++i)
    {
        ASSERT_TRUE(world->BoxOverlap(Query(), hits));
        auto query = Query();
        switch (i)
        {
        case 0: query.center.x = nan; break;
        case 1: query.center.z = inf; break;
        case 2: query.halfExtents.y = 0; break;
        case 3: query.halfExtents.x = -1; break;
        case 4: query.halfExtents.z = inf; break;
        case 5: query.rotation = Quaternion(0, 0, 0, 0); break;
        case 6: query.rotation = Quaternion(nan, 0, 0, 0); break;
        }
        EXPECT_FALSE(world->BoxOverlap(query, hits));
        EXPECT_TRUE(hits.empty());
    }
}
} // namespace

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
