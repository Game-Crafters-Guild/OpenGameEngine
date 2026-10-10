// A region mark-up's display (MarkupRegionMesh): the walls on its outline, the lid that follows
// the ground at the walls' height, the label point, and the cache that rebuilds them
// (MarkupRegionDisplayCache).

#include "MarkupECS/MarkupRegionDisplayCache.h"
#include "MarkupECS/MarkupRegionMesh.h"
#include "MarkupECS/MarkupRegionOutline.h"

#include "Mathematics/Geometry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace GameEngine;
using MarkupECS::BuildMarkupRegionGround;
using MarkupECS::BuildMarkupRegionMesh;
using MarkupECS::MarkupRegionDisplayCache;
using MarkupECS::MarkupRegionGround;
using MarkupECS::MarkupRegionGroundKey;
using MarkupECS::MarkupRegionMesh;
using MarkupECS::MarkupRegionVertex;
using Mathematics::Vector2;
using Mathematics::Vector3;

namespace
{

constexpr float kHeight = 8.0f;

// The polygon's edges sampled every meter at the ground height `ground` gives, as the drape
// samples a linear region's outline.
std::vector<Vector3> SampleOutline(const std::vector<Vector2>& corners, float (*ground)(float, float))
{
    std::vector<Vector3> samples;
    for (std::size_t i = 0; i < corners.size(); ++i)
    {
        const Vector2 from = corners[i];
        const Vector2 to = corners[(i + 1) % corners.size()];
        const int steps = std::max(1, static_cast<int>(std::ceil((to - from).Length())));
        for (int s = 0; s < steps; ++s)
        {
            const Vector2 p = from + (to - from) * (static_cast<float>(s) / static_cast<float>(steps));
            samples.emplace_back(p.x, ground(p.x, p.y), p.y);
        }
    }
    samples.push_back(samples.front()); // the drape repeats a closed ring's first sample
    return samples;
}

float Flat(float, float) { return 0.0f; }
float Slope(float x, float) { return 0.2f * x; }

MarkupECS::MarkupGroundSampler SamplerOf(float (*ground)(float, float))
{
    return [ground](const Vector2& xz) { return ground(xz.x, xz.y); };
}

std::vector<Vector2> RingOf(const MarkupRegionGround& ground)
{
    std::vector<Vector2> ring;
    for (const Vector3& p : ground.Outline)
        ring.emplace_back(p.x, p.z);
    return ring;
}

Vector3 TriangleCross(const MarkupRegionVertex* v)
{
    return Vector3::Cross(v[1].Position - v[0].Position, v[2].Position - v[0].Position);
}

// The walls stand on every outline sample and face out (front-facing from outside, as
// markup_glow.vert winds a box: the geometric cross points inward), ExtrudeHeight tall; the lid's
// triangles face up, lie inside the ring, cover its area and keep their edges within the split
// limit.
void ExpectWallsAndLid(const MarkupRegionGround& ground, const MarkupRegionMesh& mesh)
{
    const std::vector<Vector2> ring = RingOf(ground);
    ASSERT_EQ(mesh.WallVertexCount, ground.Outline.size() * 6u);
    for (std::size_t v = 0; v < mesh.WallVertexCount; v += 3)
    {
        const MarkupRegionVertex* triangle = &mesh.Vertices[v];
        const Vector3 centroid = (triangle[0].Position + triangle[1].Position + triangle[2].Position) / 3.0f;
        const Vector3 cross = TriangleCross(triangle);
        // Outward: from the nearest point of the wall's own base edge toward outside the ring.
        const Vector2 probe(centroid.x - cross.x * 0.01f, centroid.z - cross.z * 0.01f);
        EXPECT_FALSE(Mathematics::PointInPolygon(probe, ring)) << "wall triangle " << v / 3 << " faces in";
    }
    for (std::size_t s = 0; s < ground.Outline.size(); ++s)
    {
        const MarkupRegionVertex* quad = &mesh.Vertices[s * 6];
        EXPECT_NEAR(quad[1].Position.y - ground.Outline[s].y, kHeight, 1.0e-4f) << "sample " << s;
        EXPECT_EQ(quad[1].Rim, 0.0f) << "the top edge is the rim";
    }
    float lidArea = 0.0f;
    for (std::size_t v = mesh.WallVertexCount; v < mesh.Vertices.size(); v += 3)
    {
        const MarkupRegionVertex* triangle = &mesh.Vertices[v];
        const Vector3 cross = TriangleCross(triangle);
        EXPECT_LE(cross.y, 1.0e-3f) << "lid triangle " << (v - mesh.WallVertexCount) / 3 << " faces down";
        lidArea += -cross.y * 0.5f;
        const Vector3 centroid = (triangle[0].Position + triangle[1].Position + triangle[2].Position) / 3.0f;
        EXPECT_TRUE(Mathematics::PointInPolygon(Vector2(centroid.x, centroid.z), ring))
            << "lid triangle " << (v - mesh.WallVertexCount) / 3 << " lies outside the ring";
        for (int k = 0; k < 3; ++k)
        {
            const Vector3 edge = triangle[(k + 1) % 3].Position - triangle[k].Position;
            EXPECT_LE(std::sqrt(edge.x * edge.x + edge.z * edge.z), MarkupECS::kLidMinEdgeMeters + 1.0e-3f);
        }
    }
    EXPECT_NEAR(lidArea, MarkupECS::RegionOutlineArea(ring), 0.5f);
}

} // namespace

TEST(MarkupRegionMesh, AConvexOutlineStandsAWallOnEverySampleUnderItsLid)
{
    const std::vector<Vector2> square{{0, 0}, {20, 0}, {20, 20}, {0, 20}};
    const MarkupRegionGround ground = BuildMarkupRegionGround(SampleOutline(square, Flat), true, SamplerOf(Flat));
    EXPECT_TRUE(ground.Closed);
    EXPECT_FALSE(ground.Crosses);
    EXPECT_EQ(ground.Outline.size(), 80u) << "the repeated closing sample is merged";
    ExpectWallsAndLid(ground, BuildMarkupRegionMesh(ground, kHeight));
}

TEST(MarkupRegionMesh, AConcaveOutlineKeepsItsLidOutOfTheNotch)
{
    // A C opening to +x: its lid must not cover the notch, and it is wound clockwise here, which
    // the builder turns.
    const std::vector<Vector2> c{{0, 0}, {0, 30}, {30, 30}, {30, 20}, {10, 20}, {10, 10}, {30, 10}, {30, 0}};
    const MarkupRegionGround ground = BuildMarkupRegionGround(SampleOutline(c, Flat), true, SamplerOf(Flat));
    ASSERT_FALSE(ground.Crosses);
    EXPECT_GT(Mathematics::PolygonDoubledSignedArea(RingOf(ground)), 0.0f);
    ExpectWallsAndLid(ground, BuildMarkupRegionMesh(ground, kHeight));
}

TEST(MarkupRegionMesh, TheLidFollowsTheGroundAtTheWallsHeight)
{
    const std::vector<Vector2> square{{0, 0}, {40, 0}, {40, 40}, {0, 40}};
    const MarkupRegionGround ground = BuildMarkupRegionGround(SampleOutline(square, Slope), true, SamplerOf(Slope));
    const MarkupRegionMesh mesh = BuildMarkupRegionMesh(ground, kHeight);
    ASSERT_GT(mesh.Vertices.size(), mesh.WallVertexCount);
    for (std::size_t v = mesh.WallVertexCount; v < mesh.Vertices.size(); ++v)
    {
        const Vector3& p = mesh.Vertices[v].Position;
        EXPECT_NEAR(p.y, Slope(p.x, p.z) + kHeight, 1.0e-3f) << "lid vertex at x = " << p.x;
    }
    ExpectWallsAndLid(ground, mesh);
}

TEST(MarkupRegionMesh, ACrossingOutlineKeepsItsWallsAndDropsItsLid)
{
    const std::vector<Vector2> eight{{0, 0}, {20, 20}, {20, 0}, {0, 20}};
    const MarkupRegionGround ground = BuildMarkupRegionGround(SampleOutline(eight, Flat), true, SamplerOf(Flat));
    EXPECT_TRUE(ground.Crosses);
    const MarkupRegionMesh mesh = BuildMarkupRegionMesh(ground, kHeight);
    EXPECT_EQ(mesh.WallVertexCount, ground.Outline.size() * 6u);
    EXPECT_EQ(mesh.Vertices.size(), mesh.WallVertexCount);
}

TEST(MarkupRegionMesh, TheLabelStandsInsideACShapeAtTheWallsHeight)
{
    // The C's area centroid falls in its notch; the label must stand on the C itself.
    const std::vector<Vector2> c{{0, 0}, {30, 0}, {30, 10}, {10, 10}, {10, 20}, {30, 20}, {30, 30}, {0, 30}};
    const MarkupRegionGround ground = BuildMarkupRegionGround(SampleOutline(c, Slope), true, SamplerOf(Slope));
    const MarkupRegionMesh mesh = BuildMarkupRegionMesh(ground, kHeight);
    EXPECT_TRUE(Mathematics::PointInPolygon(Vector2(mesh.Label.x, mesh.Label.z), c));
    EXPECT_NEAR(mesh.Label.y, Slope(mesh.Label.x, mesh.Label.z) + kHeight, 1.0e-3f);
}

TEST(MarkupRegionMesh, TheCacheRebuildsOnlyWhatAChangedKeyNeedsAndEvictsUnseenRegions)
{
    const std::vector<Vector2> square{{0, 0}, {20, 0}, {20, 20}, {0, 20}};
    const std::vector<Vector3> outline = SampleOutline(square, Flat);
    int groundBuilds = 0;
    const MarkupRegionDisplayCache::GroundBuilder build = [&]() {
        ++groundBuilds;
        return BuildMarkupRegionGround(outline, true, SamplerOf(Flat));
    };
    const MarkupRegionDisplayCache::MemberKeyReader noMember = [](ECS::EntityHandle) {
        return MarkupECS::MarkupFootprintKey{};
    };
    MarkupRegionDisplayCache cache;
    const ECS::EntityHandle region(7u);
    MarkupRegionGroundKey key;
    key.SplineVersion = 1;

    const uint64 first = cache.Resolve(region, key, kHeight, 1, noMember, build).MeshRevision;
    cache.Resolve(region, key, kHeight, 2, noMember, build);
    EXPECT_EQ(groundBuilds, 1) << "an unchanged key rebuilt the ground";
    EXPECT_EQ(cache.Resolve(region, key, kHeight, 3, noMember, build).MeshRevision, first) << "an unchanged key rebuilt the mesh";

    const uint64 taller = cache.Resolve(region, key, kHeight * 2.0f, 4, noMember, build).MeshRevision;
    EXPECT_EQ(groundBuilds, 1) << "a height change rebuilt the ground";
    EXPECT_NE(taller, first) << "a height change kept the old mesh";

    for (const auto change : {+[](MarkupRegionGroundKey& k) { ++k.SplineVersion; },
                              +[](MarkupRegionGroundKey& k) { k.Matrix[12] += 1.0f; },
                              +[](MarkupRegionGroundKey& k) { k.SeaLevel += 1.0f; },
                              +[](MarkupRegionGroundKey& k) { ++k.GroundRevision; }})
    {
        const int before = groundBuilds;
        change(key);
        cache.Resolve(region, key, kHeight, 5, noMember, build);
        EXPECT_EQ(groundBuilds, before + 1) << "a key change did not rebuild the ground";
    }

    cache.EvictUnseen(5 + MarkupRegionDisplayCache::kEvictAfterFrames);
    EXPECT_NE(cache.Find(region), nullptr) << "evicted a region seen kEvictAfterFrames frames ago";
    cache.EvictUnseen(6 + MarkupRegionDisplayCache::kEvictAfterFrames);
    EXPECT_EQ(cache.Find(region), nullptr) << "kept a region unseen for longer than kEvictAfterFrames";
}

// A straight run of drape samples (one a meter) must not clip into flat slivers that the split
// then multiplies: a 60 m square's lid stays near its area over the split limit squared, with no
// triangle of zero area, and its edge still passes through every wall sample.
TEST(MarkupRegionMesh, AStraightSampledEdgeKeepsTheLidLeanAndCrackFree)
{
    const std::vector<Vector2> square{{0, 0}, {60, 0}, {60, 60}, {0, 60}};
    const MarkupRegionGround ground = BuildMarkupRegionGround(SampleOutline(square, Slope), true, SamplerOf(Slope));
    const std::size_t triangles = ground.LidTriangles.size() / 3;
    // Every triangle at least an eighth of the limit squared on average: 3600 / (16 / 8) = 1800.
    EXPECT_LE(triangles, 1800u);
    float smallest = std::numeric_limits<float>::max();
    for (std::size_t t = 0; t < ground.LidTriangles.size(); t += 3)
    {
        const Vector3& a = ground.LidPoints[ground.LidTriangles[t]];
        const Vector3& b = ground.LidPoints[ground.LidTriangles[t + 1]];
        const Vector3& c = ground.LidPoints[ground.LidTriangles[t + 2]];
        smallest = std::min(smallest, std::abs(Vector3::Cross(b - a, c - a).y) * 0.5f);
    }
    EXPECT_GT(smallest, 0.01f) << "a flat sliver in the lid";
    // Every wall sample is a corner of a lid triangle, at its own height: no crack between the lid
    // and the walls.
    std::vector<Vector3> corners;
    for (const uint32 index : ground.LidTriangles)
        corners.push_back(ground.LidPoints[index]);
    for (std::size_t i = 0; i < ground.Outline.size(); ++i)
    {
        const bool corner = std::any_of(corners.begin(), corners.end(), [&](const Vector3& point) {
            return (point - ground.Outline[i]).Length() < 1.0e-3f;
        });
        EXPECT_TRUE(corner) << "wall sample " << i << " is not a lid corner";
    }
}

// The cache rebuilds a drawn region when its member list changes (a member's mode flips, a member
// is appended), and a region whose list dropped a member is no longer rebuilt when that member
// moves, though another region still listing it is.
TEST(MarkupRegionMesh, TheCacheFollowsTheMemberListAndForgetsADroppedMember)
{
    const std::vector<Vector3> outline = SampleOutline({{0, 0}, {20, 0}, {20, 20}, {0, 20}}, Flat);
    const MarkupRegionDisplayCache::GroundBuilder build = [&]() {
        return BuildMarkupRegionGround(outline, true, SamplerOf(Flat));
    };
    // The members' footprint keys, as the world would give them: moving a member changes its matrix.
    std::vector<MarkupECS::MarkupFootprintKey> footprints(2);
    for (MarkupECS::MarkupFootprintKey& footprint : footprints)
        footprint.Shape = MarkupECS::MarkupFootprintKey::Kind::Box;
    const MarkupRegionDisplayCache::MemberKeyReader readMember = [&](ECS::EntityHandle member) {
        return footprints[member.id - 1u];
    };
    const ECS::EntityHandle pond(1u);
    const ECS::EntityHandle yard(2u);
    MarkupRegionDisplayCache cache;
    const ECS::EntityHandle forest(7u);
    const ECS::EntityHandle village(8u);
    MarkupRegionGroundKey forestKey;
    forestKey.MemberCount = 1;
    forestKey.Members[0] = {pond, Components::MarkupMemberMode::Exclude};
    MarkupRegionGroundKey villageKey = forestKey;
    uint64 frame = 1;
    const auto revisionOf = [&](ECS::EntityHandle region, const MarkupRegionGroundKey& key) {
        return cache.Resolve(region, key, kHeight, frame, readMember, build).MeshRevision;
    };
    uint64 forestRevision = revisionOf(forest, forestKey);
    revisionOf(village, villageKey);

    ++frame;
    forestKey.Members[0].Mode = Components::MarkupMemberMode::Include;
    EXPECT_NE(revisionOf(forest, forestKey), forestRevision) << "a mode flip kept the old ground";
    forestRevision = revisionOf(forest, forestKey);

    ++frame;
    forestKey.Members[forestKey.MemberCount++] = {yard, Components::MarkupMemberMode::Exclude};
    EXPECT_NE(revisionOf(forest, forestKey), forestRevision) << "an appended member kept the old ground";

    ++frame;
    forestKey.MemberCount = 1;
    forestKey.Members[0] = {yard, Components::MarkupMemberMode::Exclude};
    forestRevision = revisionOf(forest, forestKey);
    const uint64 villageRevision = revisionOf(village, villageKey);

    ++frame;
    footprints[pond.id - 1u].Matrix[12] += 5.0f;
    EXPECT_NE(revisionOf(village, villageKey), villageRevision) << "the region still listing the pond kept its ground";
    EXPECT_EQ(revisionOf(forest, forestKey), forestRevision) << "the region that dropped the pond was rebuilt";
}

// A path's display is its open line: no walls, no lid, the line through every sample, and its
// label halfway along it, not at its start.
TEST(MarkupRegionMesh, AnOpenOutlineDrawsItsLineAndLabelsItsMiddle)
{
    // Samples crowded at the start (every 0.5 m to x = 20, then every 4 m to 100): the middle sample
    // by count is at x = 20, the middle by length at 50.
    std::vector<Vector3> line;
    for (float x = 0.0f; x < 20.0f; x += 0.5f)
        line.emplace_back(x, Slope(x, 0.0f), 0.0f);
    for (float x = 20.0f; x <= 100.0f; x += 4.0f)
        line.emplace_back(x, Slope(x, 0.0f), 0.0f);
    const MarkupRegionGround ground = BuildMarkupRegionGround(line, false, SamplerOf(Slope));
    EXPECT_FALSE(ground.Closed);
    EXPECT_TRUE(ground.LidTriangles.empty());
    const MarkupRegionMesh mesh = BuildMarkupRegionMesh(ground, 0.0f);
    EXPECT_TRUE(mesh.Vertices.empty());
    EXPECT_EQ(ground.Outline.size(), line.size());
    EXPECT_NEAR(mesh.Label.x, 50.0f, 1.0e-3f);
    EXPECT_NEAR(mesh.Label.y, Slope(50.0f, 0.0f), 1.0e-3f);
}
