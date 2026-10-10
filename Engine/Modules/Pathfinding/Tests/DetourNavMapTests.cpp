#include <gtest/gtest.h>
#include "Pathfinding/DetourNavMap.h"
#include "Pathfinding/ObstacleAvoidance.h"
#include "Pathfinding/PathBuffer.h"

#include <cmath>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Pathfinding;

// ---------------------------------------------------------------------------
// Test geometry: a subdivided 20x20 flat plane at Y=0, centered at origin.
// Grid of triangles to give Recast enough spans to work with.
// ---------------------------------------------------------------------------
namespace DetourTestHelpers
{
namespace
{

constexpr uint32 kGridRes = 10; // 10x10 quads = 200 triangles
static constexpr float32 kPlaneSize = 20.0f;
static constexpr float32 kPlaneHalf = kPlaneSize * 0.5f;

struct PlaneGeometry
{
    std::vector<float32> Vertices;
    std::vector<uint32> Indices;
    uint32 VertexCount = 0;
    uint32 TriangleCount = 0;
};

PlaneGeometry BuildPlane()
{
    PlaneGeometry g;
    const uint32 vertsPerSide = kGridRes + 1;
    g.VertexCount = vertsPerSide * vertsPerSide;
    g.Vertices.resize(g.VertexCount * 3);

    for (uint32 z = 0; z <= kGridRes; ++z)
    {
        for (uint32 x = 0; x <= kGridRes; ++x)
        {
            uint32 idx = (z * vertsPerSide + x) * 3;
            g.Vertices[idx + 0] = -kPlaneHalf + static_cast<float32>(x) * (kPlaneSize / kGridRes);
            g.Vertices[idx + 1] = 0.0f;
            g.Vertices[idx + 2] = -kPlaneHalf + static_cast<float32>(z) * (kPlaneSize / kGridRes);
        }
    }

    g.TriangleCount = kGridRes * kGridRes * 2;
    g.Indices.resize(g.TriangleCount * 3);
    uint32 ti = 0;
    for (uint32 z = 0; z < kGridRes; ++z)
    {
        for (uint32 x = 0; x < kGridRes; ++x)
        {
            uint32 topLeft = z * vertsPerSide + x;
            uint32 topRight = topLeft + 1;
            uint32 botLeft = topLeft + vertsPerSide;
            uint32 botRight = botLeft + 1;

            g.Indices[ti++] = topLeft;
            g.Indices[ti++] = botLeft;
            g.Indices[ti++] = topRight;

            g.Indices[ti++] = topRight;
            g.Indices[ti++] = botLeft;
            g.Indices[ti++] = botRight;
        }
    }
    return g;
}

NavMeshSettings MakeDetourSettings()
{
    NavMeshSettings s;
    s.CellSize = 0.3f;
    s.CellHeight = 0.2f;
    s.AgentRadius = 0.6f;
    s.AgentHeight = 2.0f;
    s.AgentMaxClimb = 0.9f;
    s.AgentMaxSlope = 45.0f;
    s.RegionMinSize = 1.0f;
    s.RegionMergeSize = 20.0f;
    s.EdgeMaxLen = 12.0f;
    s.EdgeMaxError = 1.3f;
    s.DetailSampleDist = 6.0f;
    s.DetailSampleMaxError = 1.0f;
    s.VertsPerPoly = 6;
    return s;
}

DetourNavMap::InputGeometry MakePlaneInput(const PlaneGeometry& plane)
{
    DetourNavMap::InputGeometry geo;
    geo.Vertices = plane.Vertices.data();
    geo.VertexCount = plane.VertexCount;
    geo.Indices = plane.Indices.data();
    geo.TriangleCount = plane.TriangleCount;
    return geo;
}

} // anonymous namespace
} // namespace DetourTestHelpers

// ---------------------------------------------------------------------------
// Fixture: builds a navmesh once for tests that need it
// ---------------------------------------------------------------------------
class DetourNavMapBuiltTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Plane = DetourTestHelpers::BuildPlane();
        auto settings = DetourTestHelpers::MakeDetourSettings();
        auto geometry = DetourTestHelpers::MakePlaneInput(m_Plane);
        ASSERT_TRUE(m_NavMap.Build(settings, geometry))
            << "Failed to build navmesh from test plane geometry ("
            << m_Plane.VertexCount << " verts, " << m_Plane.TriangleCount << " tris)";
    }

    DetourTestHelpers::PlaneGeometry m_Plane;
    DetourNavMap m_NavMap;
    PathBuffer m_PathBuffer;
};

// ===========================================================================
// Unbuilt / stub tests (existing)
// ===========================================================================

TEST(DetourNavMapTests, ConstructAndDestroy)
{
    DetourNavMap navMap;
    // Should not crash on destruction
}

TEST(DetourNavMapTests, IsBuiltReturnsFalseInitially)
{
    DetourNavMap navMap;
    EXPECT_FALSE(navMap.IsBuilt());
}

TEST(DetourNavMapTests, FindPathReturnsFailedWhenNotBuilt)
{
    DetourNavMap navMap;
    PathBuffer pathBuffer;
    PathHandle outPath;

    PathRequest request;
    request.StartX = 0.0f; request.StartZ = 0.0f;
    request.EndX = 5.0f; request.EndZ = 5.0f;

    PathStatus status = navMap.FindPath(request, pathBuffer, outPath);
    EXPECT_EQ(status, PathStatus::Failed);
}

TEST(DetourNavMapTests, IsPointNavigableReturnsFalseWhenNotBuilt)
{
    DetourNavMap navMap;
    EXPECT_FALSE(navMap.IsPointNavigable(1.0f, 0.0f, 1.0f, 0.5f));
}

TEST(DetourNavMapTests, GetClosestNavigablePointReturnsFalseWhenNotBuilt)
{
    DetourNavMap navMap;
    float32 outX = 0.0f, outY = 0.0f, outZ = 0.0f;
    EXPECT_FALSE(navMap.GetClosestNavigablePoint(1.0f, 0.0f, 1.0f, 5.0f, outX, outY, outZ));
}

TEST(DetourNavMapTests, RaycastReturnsFalseWhenNotBuilt)
{
    DetourNavMap navMap;
    float32 hitX = 0.0f, hitY = 0.0f, hitZ = 0.0f;
    EXPECT_FALSE(navMap.Raycast(0.0f, 0.0f, 0.0f, 5.0f, 0.0f, 5.0f, hitX, hitY, hitZ));
}

TEST(DetourNavMapTests, SerializeReturnsEmptyWhenNotBuilt)
{
    DetourNavMap navMap;
    std::vector<uint8> data;
    EXPECT_FALSE(navMap.Serialize(data));
    EXPECT_TRUE(data.empty());
}

TEST(DetourNavMapTests, GetDebugCountsReturnZeroWhenNotBuilt)
{
    DetourNavMap navMap;
    EXPECT_EQ(navMap.GetDebugVertexCount(), 0u);
    EXPECT_EQ(navMap.GetDebugTriangleCount(), 0u);
}

// ===========================================================================
// Build tests
// ===========================================================================

TEST(DetourNavMapTests, BuildWithValidGeometrySucceeds)
{
    DetourNavMap navMap;
    auto plane = DetourTestHelpers::BuildPlane();
    auto settings = DetourTestHelpers::MakeDetourSettings();
    auto geometry = DetourTestHelpers::MakePlaneInput(plane);
    EXPECT_TRUE(navMap.Build(settings, geometry));
    EXPECT_TRUE(navMap.IsBuilt());
}

TEST(DetourNavMapTests, BuildWithNullGeometryFails)
{
    DetourNavMap navMap;
    auto settings = DetourTestHelpers::MakeDetourSettings();
    DetourNavMap::InputGeometry empty;
    EXPECT_FALSE(navMap.Build(settings, empty));
    EXPECT_FALSE(navMap.IsBuilt());
}

TEST(DetourNavMapTests, BuildProducesDebugMesh)
{
    DetourNavMap navMap;
    auto plane = DetourTestHelpers::BuildPlane();
    auto settings = DetourTestHelpers::MakeDetourSettings();
    auto geometry = DetourTestHelpers::MakePlaneInput(plane);
    ASSERT_TRUE(navMap.Build(settings, geometry));

    EXPECT_GT(navMap.GetDebugVertexCount(), 0u);
    EXPECT_GT(navMap.GetDebugTriangleCount(), 0u);
    EXPECT_NE(navMap.GetDebugVertices(), nullptr);
    EXPECT_NE(navMap.GetDebugIndices(), nullptr);
}

TEST(DetourNavMapTests, RebuildReplacesNavMesh)
{
    DetourNavMap navMap;
    auto plane = DetourTestHelpers::BuildPlane();
    auto settings = DetourTestHelpers::MakeDetourSettings();
    auto geometry = DetourTestHelpers::MakePlaneInput(plane);

    ASSERT_TRUE(navMap.Build(settings, geometry));
    EXPECT_GT(navMap.GetDebugVertexCount(), 0u);

    // Build again with different cell size — should produce different mesh
    settings.CellSize = 0.5f;
    ASSERT_TRUE(navMap.Build(settings, geometry));
    EXPECT_TRUE(navMap.IsBuilt());
    EXPECT_GT(navMap.GetDebugVertexCount(), 0u);
}

// ===========================================================================
// Pathfinding tests (on built navmesh)
// ===========================================================================

TEST_F(DetourNavMapBuiltTest, FindPathOnFlatPlaneReturnsComplete)
{
    PathHandle outPath;
    PathRequest request;
    request.StartX = -5.0f; request.StartY = 0.0f; request.StartZ = -5.0f;
    request.EndX = 5.0f; request.EndY = 0.0f; request.EndZ = 5.0f;
    request.AgentRadius = 0.6f;
    request.AgentHeight = 2.0f;

    PathStatus status = m_NavMap.FindPath(request, m_PathBuffer, outPath);
    ASSERT_EQ(status, PathStatus::Complete);
    ASSERT_TRUE(outPath.IsValid());

    uint32 pointCount = m_PathBuffer.GetPointCount(outPath);
    ASSERT_GE(pointCount, 2u) << "Path should have at least start and end";

    // First waypoint should be near start
    PathPoint first = m_PathBuffer.GetPoint(outPath, 0);
    EXPECT_NEAR(first.X, -5.0f, 2.0f);
    EXPECT_NEAR(first.Z, -5.0f, 2.0f);

    // Last waypoint should be near end
    PathPoint last = m_PathBuffer.GetPoint(outPath, pointCount - 1);
    EXPECT_NEAR(last.X, 5.0f, 2.0f);
    EXPECT_NEAR(last.Z, 5.0f, 2.0f);
}

TEST_F(DetourNavMapBuiltTest, FindPathSameStartAndEndProducesPath)
{
    PathHandle outPath;
    PathRequest request;
    request.StartX = 0.0f; request.StartY = 0.0f; request.StartZ = 0.0f;
    request.EndX = 0.0f; request.EndY = 0.0f; request.EndZ = 0.0f;
    request.AgentRadius = 0.6f;
    request.AgentHeight = 2.0f;

    PathStatus status = m_NavMap.FindPath(request, m_PathBuffer, outPath);
    EXPECT_NE(status, PathStatus::Failed);
}

TEST_F(DetourNavMapBuiltTest, FindPathToUnreachablePointFails)
{
    PathHandle outPath;
    PathRequest request;
    // Point far outside the navmesh bounds
    request.StartX = 0.0f; request.StartY = 0.0f; request.StartZ = 0.0f;
    request.EndX = 500.0f; request.EndY = 0.0f; request.EndZ = 500.0f;
    request.AgentRadius = 0.6f;
    request.AgentHeight = 2.0f;

    PathStatus status = m_NavMap.FindPath(request, m_PathBuffer, outPath);
    // Should fail or return partial — the end point is off the mesh
    EXPECT_NE(status, PathStatus::Complete);
}

// ===========================================================================
// Point query tests
// ===========================================================================

TEST_F(DetourNavMapBuiltTest, IsPointNavigableOnMeshReturnsTrue)
{
    // Center of the plane should be navigable
    EXPECT_TRUE(m_NavMap.IsPointNavigable(0.0f, 0.0f, 0.0f, 0.5f));
}

TEST_F(DetourNavMapBuiltTest, IsPointNavigableOffMeshReturnsFalse)
{
    // Way outside the mesh
    EXPECT_FALSE(m_NavMap.IsPointNavigable(100.0f, 0.0f, 100.0f, 0.5f));
}

TEST_F(DetourNavMapBuiltTest, GetClosestNavigablePointOnMesh)
{
    float32 outX = 0.0f, outY = 0.0f, outZ = 0.0f;
    bool found = m_NavMap.GetClosestNavigablePoint(3.0f, 0.0f, 3.0f, 5.0f, outX, outY, outZ);
    EXPECT_TRUE(found);
    // Should snap to a point on the navmesh near the query
    EXPECT_NEAR(outX, 3.0f, 1.5f);
    EXPECT_NEAR(outZ, 3.0f, 1.5f);
}

TEST_F(DetourNavMapBuiltTest, GetClosestNavigablePointFarOffMesh)
{
    float32 outX = 0.0f, outY = 0.0f, outZ = 0.0f;
    // Query point very far away with small search radius
    bool found = m_NavMap.GetClosestNavigablePoint(500.0f, 0.0f, 500.0f, 1.0f, outX, outY, outZ);
    EXPECT_FALSE(found);
}

// ===========================================================================
// Raycast tests
// ===========================================================================

TEST_F(DetourNavMapBuiltTest, RaycastOnOpenPlaneReturnsNoHit)
{
    float32 hitX = 0.0f, hitY = 0.0f, hitZ = 0.0f;
    // Ray across the open plane — no obstacle, should reach end
    bool hit = m_NavMap.Raycast(-3.0f, 0.0f, 0.0f, 3.0f, 0.0f, 0.0f, hitX, hitY, hitZ);
    EXPECT_FALSE(hit) << "Open plane raycast should not hit anything";
}

TEST_F(DetourNavMapBuiltTest, RaycastToEdgeOfMeshHits)
{
    float32 hitX = 0.0f, hitY = 0.0f, hitZ = 0.0f;
    // Ray from center to well beyond the mesh boundary
    bool hit = m_NavMap.Raycast(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 50.0f, hitX, hitY, hitZ);
    // Should hit the mesh boundary (ray leaves navigable area)
    EXPECT_TRUE(hit);
    // Hit point Z should be near the mesh edge (~9.4 after agent radius erosion)
    EXPECT_GT(hitZ, 5.0f);
    EXPECT_LT(hitZ, 11.0f);
}

// ===========================================================================
// Serialization round-trip
// ===========================================================================

TEST_F(DetourNavMapBuiltTest, SerializeProducesNonEmptyData)
{
    std::vector<uint8> data;
    EXPECT_TRUE(m_NavMap.Serialize(data));
    EXPECT_GT(data.size(), 40u); // minimum: header(40) + at least one tile

    // Verify v2 multi-tile format header
    ASSERT_GE(data.size(), 4u);
    EXPECT_EQ(data[0], 'D');
    EXPECT_EQ(data[1], 'T');
    EXPECT_EQ(data[2], 'N');
    EXPECT_EQ(data[3], 'M');
}

TEST_F(DetourNavMapBuiltTest, SerializeDeserializeRoundTrip)
{
    // Serialize the built navmesh
    std::vector<uint8> data;
    ASSERT_TRUE(m_NavMap.Serialize(data));
    ASSERT_GT(data.size(), 0u);

    // Deserialize into a fresh navmap
    DetourNavMap restored;
    ASSERT_TRUE(restored.Deserialize(data.data(), static_cast<uint32>(data.size())));
    EXPECT_TRUE(restored.IsBuilt());

    // Verify pathfinding works on the restored navmesh
    PathBuffer pathBuffer;
    PathHandle outPath;
    PathRequest request;
    request.StartX = -4.0f; request.StartY = 0.0f; request.StartZ = 0.0f;
    request.EndX = 4.0f; request.EndY = 0.0f; request.EndZ = 0.0f;
    request.AgentRadius = 0.6f;
    request.AgentHeight = 2.0f;

    PathStatus status = restored.FindPath(request, pathBuffer, outPath);
    EXPECT_EQ(status, PathStatus::Complete);
    EXPECT_GE(pathBuffer.GetPointCount(outPath), 2u);
}

TEST(DetourNavMapTests, DeserializeWithInvalidDataFails)
{
    DetourNavMap navMap;
    uint8 garbage[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x00, 0x00, 0x00};
    EXPECT_FALSE(navMap.Deserialize(garbage, sizeof(garbage)));
    EXPECT_FALSE(navMap.IsBuilt());
}

TEST(DetourNavMapTests, DeserializeWithNullFails)
{
    DetourNavMap navMap;
    EXPECT_FALSE(navMap.Deserialize(nullptr, 0));
}

// ===========================================================================
// CrowdManager tests
// ===========================================================================

TEST_F(DetourNavMapBuiltTest, CrowdManagerInitializesWithBuiltNavMesh)
{
    CrowdManager crowd;
    EXPECT_TRUE(crowd.Initialize(m_NavMap, 32));
    EXPECT_TRUE(crowd.IsInitialized());
    crowd.Shutdown();
    EXPECT_FALSE(crowd.IsInitialized());
}

TEST_F(DetourNavMapBuiltTest, CrowdManagerFailsWithUnbuiltNavMesh)
{
    DetourNavMap unbuilt;
    CrowdManager crowd;
    EXPECT_FALSE(crowd.Initialize(unbuilt, 32));
    EXPECT_FALSE(crowd.IsInitialized());
}

TEST_F(DetourNavMapBuiltTest, CrowdAgentAddRemoveLifecycle)
{
    CrowdManager crowd;
    ASSERT_TRUE(crowd.Initialize(m_NavMap, 32));

    CrowdAgentParams params;
    params.Radius = 0.5f;
    params.Height = 2.0f;
    params.MaxSpeed = 3.5f;
    params.MaxAcceleration = 8.0f;

    CrowdAgentHandle agent = crowd.AddAgent(0.0f, 0.0f, 0.0f, params);
    EXPECT_NE(agent, kInvalidCrowdAgent);
    EXPECT_TRUE(crowd.IsAgentActive(agent));

    crowd.RemoveAgent(agent);
    EXPECT_FALSE(crowd.IsAgentActive(agent));

    crowd.Shutdown();
}

TEST_F(DetourNavMapBuiltTest, CrowdAgentMovesTowardTarget)
{
    CrowdManager crowd;
    ASSERT_TRUE(crowd.Initialize(m_NavMap, 32));

    CrowdAgentParams params;
    params.Radius = 0.5f;
    params.Height = 2.0f;
    params.MaxSpeed = 5.0f;
    params.MaxAcceleration = 10.0f;

    CrowdAgentHandle agent = crowd.AddAgent(-3.0f, 0.0f, 0.0f, params);
    ASSERT_NE(agent, kInvalidCrowdAgent);

    float32 initX = 0.0f, initY = 0.0f, initZ = 0.0f;
    crowd.GetAgentPosition(agent, initX, initY, initZ);

    crowd.SetAgentTarget(agent, 5.0f, 0.0f, 0.0f);

    // 3 seconds at 60fps
    for (int i = 0; i < 180; ++i)
        crowd.Update(1.0f / 60.0f);

    float32 posX = 0.0f, posY = 0.0f, posZ = 0.0f;
    crowd.GetAgentPosition(agent, posX, posY, posZ);
    EXPECT_TRUE(crowd.IsAgentActive(agent));

    // Agent should have moved significantly toward target at X=5
    EXPECT_GT(posX, 3.0f)
        << "Agent should have reached near target. Start=" << initX << " End=" << posX;

    crowd.Shutdown();
}

TEST_F(DetourNavMapBuiltTest, CrowdManagerSetAgentParamsUpdatesAgent)
{
    CrowdManager crowd;
    ASSERT_TRUE(crowd.Initialize(m_NavMap, 32));

    CrowdAgentParams params;
    params.Radius = 0.5f;
    params.Height = 2.0f;
    params.MaxSpeed = 3.5f;
    params.MaxAcceleration = 8.0f;

    CrowdAgentHandle agent = crowd.AddAgent(0.0f, 0.0f, 0.0f, params);
    ASSERT_NE(agent, kInvalidCrowdAgent);

    CrowdAgentParams newParams = params;
    newParams.Radius = 1.0f;
    newParams.MaxSpeed = 5.0f;
    newParams.AvoidanceQuality = ObstacleAvoidanceQuality::Low;

    crowd.SetAgentParams(agent, newParams);
    EXPECT_TRUE(crowd.IsAgentActive(agent));

    // Verify agent still functions after param update
    crowd.SetAgentTarget(agent, 3.0f, 0.0f, 0.0f);
    for (int i = 0; i < 60; ++i)
        crowd.Update(1.0f / 60.0f);

    float32 posX = 0.0f, posY = 0.0f, posZ = 0.0f;
    crowd.GetAgentPosition(agent, posX, posY, posZ);
    EXPECT_GT(posX, 0.5f) << "Agent should move after SetAgentParams";

    crowd.Shutdown();
}

TEST_F(DetourNavMapBuiltTest, CrowdManagerDoubleInitialize)
{
    CrowdManager crowd;
    ASSERT_TRUE(crowd.Initialize(m_NavMap, 32));

    CrowdAgentParams params;
    params.Radius = 0.5f; params.Height = 2.0f;
    params.MaxSpeed = 5.0f; params.MaxAcceleration = 10.0f;

    CrowdAgentHandle firstAgent = crowd.AddAgent(0.0f, 0.0f, 0.0f, params);
    EXPECT_NE(firstAgent, kInvalidCrowdAgent);

    // Re-initialize (should not leak, should reset)
    ASSERT_TRUE(crowd.Initialize(m_NavMap, 16));
    EXPECT_TRUE(crowd.IsInitialized());

    // Old agent should be gone
    EXPECT_FALSE(crowd.IsAgentActive(firstAgent));

    // New agent on re-initialized crowd should work
    CrowdAgentHandle newAgent = crowd.AddAgent(-2.0f, 0.0f, 0.0f, params);
    ASSERT_NE(newAgent, kInvalidCrowdAgent);

    crowd.SetAgentTarget(newAgent, 4.0f, 0.0f, 0.0f);
    for (int i = 0; i < 120; ++i)
        crowd.Update(1.0f / 60.0f);

    float32 posX = 0.0f, posY = 0.0f, posZ = 0.0f;
    crowd.GetAgentPosition(newAgent, posX, posY, posZ);
    EXPECT_GT(posX, 0.0f) << "Agent should move after double-init";

    crowd.Shutdown();
}

TEST_F(DetourNavMapBuiltTest, SerializeFormatHeaderAndTruncation)
{
    std::vector<uint8> data;
    ASSERT_TRUE(m_NavMap.Serialize(data));
    ASSERT_GE(data.size(), 40u);

    // Verify magic
    EXPECT_EQ(data[0], 'D');
    EXPECT_EQ(data[1], 'T');
    EXPECT_EQ(data[2], 'N');
    EXPECT_EQ(data[3], 'M');

    // Verify version == 1
    uint32 version = 0;
    std::memcpy(&version, data.data() + 4, sizeof(uint32));
    EXPECT_EQ(version, 1u);

    // Verify tileCount > 0 (at offset 36 = 4+4+28)
    uint32 tileCount = 0;
    std::memcpy(&tileCount, data.data() + 36, sizeof(uint32));
    EXPECT_GT(tileCount, 0u);

    // Truncated data should fail to deserialize
    DetourNavMap truncated;
    uint32 truncSize = std::min(static_cast<uint32>(data.size() - 1), 50u);
    EXPECT_FALSE(truncated.Deserialize(data.data(), truncSize));
}

TEST_F(DetourNavMapBuiltTest, CrowdAgentVelocityAndTargetReadback)
{
    CrowdManager crowd;
    ASSERT_TRUE(crowd.Initialize(m_NavMap, 32));

    CrowdAgentParams params;
    params.Radius = 0.5f; params.Height = 2.0f;
    params.MaxSpeed = 5.0f; params.MaxAcceleration = 10.0f;

    CrowdAgentHandle agent = crowd.AddAgent(-3.0f, 0.0f, 0.0f, params);
    ASSERT_NE(agent, kInvalidCrowdAgent);

    crowd.SetAgentTarget(agent, 5.0f, 0.0f, 0.0f);

    // Read back target
    float32 tgtX = 0.0f, tgtY = 0.0f, tgtZ = 0.0f;
    crowd.GetAgentTarget(agent, tgtX, tgtY, tgtZ);
    EXPECT_NEAR(tgtX, 5.0f, 1.0f);

    // Simulate
    for (int i = 0; i < 60; ++i)
        crowd.Update(1.0f / 60.0f);

    // Check velocity is non-zero and in +X direction
    float32 vx = 0.0f, vy = 0.0f, vz = 0.0f;
    crowd.GetAgentVelocity(agent, vx, vy, vz);
    float32 speed = std::sqrt(vx * vx + vy * vy + vz * vz);
    EXPECT_GT(speed, 0.1f) << "Agent should have non-zero velocity";
    EXPECT_GT(vx, 0.0f) << "Velocity should be in +X direction";

    crowd.Shutdown();
}

TEST_F(DetourNavMapBuiltTest, GetClosestNavigablePointRejectsAABBCorner)
{
    float32 outX = 0.0f, outY = 0.0f, outZ = 0.0f;

    // Query point far off navmesh corner -- XZ distance to nearest poly > searchRadius
    // even though each axis individually is within the AABB extent
    bool foundTight = m_NavMap.GetClosestNavigablePoint(14.0f, 0.0f, 14.0f, 5.0f,
                                                         outX, outY, outZ);
    EXPECT_FALSE(foundTight) << "Sphere check should reject AABB corner result";

    // Larger radius should succeed
    bool foundLarge = m_NavMap.GetClosestNavigablePoint(14.0f, 0.0f, 14.0f, 10.0f,
                                                         outX, outY, outZ);
    EXPECT_TRUE(foundLarge);
    EXPECT_GT(outX, 7.0f);
    EXPECT_GT(outZ, 7.0f);
}
