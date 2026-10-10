#include <gtest/gtest.h>

#include "DebugServer/TerrainStrokeTarget.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>
#include <string>

namespace
{
using GameEngine::Editor::ResolveStrokeTarget;
using GameEngine::Editor::SlerpDir;
using GameEngine::Mathematics::Vector3;
using json = nlohmann::json;

constexpr bool kSpherical = true;
constexpr bool kPlanar = false;

// The defect these lock: `terrain_sculpt_dab` reported a successful stroke that wrote nothing
// and left no undo entry, with `dabPoints` full of nulls. A stroke endpoint at the planet
// centre normalizes to NaN, and every guard between resolution and the sculpt is a
// `length < epsilon` test — all of which NaN passes, because NaN compares false against
// everything. The endpoint has to be rejected where it is read.

TEST(TerrainStrokeTargetTests, PlanetCentrePositionIsRejected)
{
    const json params = {{"pos", {0.0f, 0.0f, 0.0f}}};
    Vector3 out{1.0f, 1.0f, 1.0f};
    std::string err;

    EXPECT_FALSE(ResolveStrokeTarget(params, "pos", "dir", kSpherical, out, err));
    EXPECT_NE(err.find("planet centre"), std::string::npos)
        << "the error must name what is wrong with the position, got: " << err;
}

// The same resolver call resolves the stroke's END point, so the guard has to cover it too —
// a zero `end_pos` is what reaches the slerp and produces the NaN target.
TEST(TerrainStrokeTargetTests, PlanetCentreEndPositionIsRejected)
{
    const json params = {{"end_pos", {0.0f, 0.0f, 0.0f}}};
    Vector3 out{1.0f, 1.0f, 1.0f};
    std::string err;

    EXPECT_FALSE(ResolveStrokeTarget(params, "end_pos", "end_dir", kSpherical, out, err));
    EXPECT_NE(err.find("end_pos"), std::string::npos)
        << "the error must name the key the caller supplied, got: " << err;
}

// The discriminating case. A planar terrain has no centre singularity: the world origin is an
// ordinary position over it, so an ungated restore of the planet-only guard would break
// sculpting there.
TEST(TerrainStrokeTargetTests, WorldOriginIsAValidPlanarPosition)
{
    const json params = {{"pos", {0.0f, 0.0f, 0.0f}}};
    Vector3 out{7.0f, 7.0f, 7.0f};
    std::string err;

    ASSERT_TRUE(ResolveStrokeTarget(params, "pos", "dir", kPlanar, out, err)) << err;
    EXPECT_TRUE(err.empty());
    EXPECT_FLOAT_EQ(out.x, 0.0f);
    EXPECT_FLOAT_EQ(out.y, 0.0f);
    EXPECT_FLOAT_EQ(out.z, 0.0f);
}

// Zero is not the only vector a `length < epsilon` test waves through: `inf < epsilon` is
// false for the same reason `0 < epsilon` is, so a non-finite endpoint reaches the sculpt as
// a NaN target exactly as the planet centre did. A JSON number outside float range is how one
// arrives -- the parse keeps it a number and the narrowing to float makes it inf.
//
// Unlike the planet centre, this is not domain-specific: neither terrain shape has a
// non-finite point, so the rejection is ungated.
TEST(TerrainStrokeTargetTests, NonFinitePositionIsRejectedOnBothDomains)
{
    const json params = json::parse(R"({"pos":[1e39,0,0]})");
    ASSERT_TRUE(params["pos"][0].is_number())
        << "a number too large for float must still parse as a number, or this test is not "
           "reaching the guard at all";
    ASSERT_FALSE(std::isfinite(params["pos"][0].get<float>()))
        << "1e39 must overflow float for this to be the non-finite case";

    for (const bool spherical : {kPlanar, kSpherical})
    {
        Vector3 out{7.0f, 7.0f, 7.0f};
        std::string err;

        EXPECT_FALSE(ResolveStrokeTarget(params, "pos", "dir", spherical, out, err))
            << "spherical=" << spherical;
        EXPECT_NE(err.find("not finite"), std::string::npos)
            << "the error must name what is wrong with the position, spherical=" << spherical
            << ", got: " << err;
    }
}

// The far end of a two-point stroke goes through the same resolver, and it is the end that
// reaches SlerpDir -- so a non-finite `end_pos` is the one that propagates.
TEST(TerrainStrokeTargetTests, NonFiniteEndPositionIsRejectedOnBothDomains)
{
    const json params = json::parse(R"({"end_pos":[0,-1e39,0]})");

    for (const bool spherical : {kPlanar, kSpherical})
    {
        Vector3 out{7.0f, 7.0f, 7.0f};
        std::string err;

        EXPECT_FALSE(ResolveStrokeTarget(params, "end_pos", "end_dir", spherical, out, err))
            << "spherical=" << spherical;
        EXPECT_NE(err.find("end_pos"), std::string::npos)
            << "the error must name the key the caller supplied, spherical=" << spherical
            << ", got: " << err;
        EXPECT_NE(err.find("not finite"), std::string::npos)
            << "spherical=" << spherical << ", got: " << err;
    }
}

// isfinite, not isinf: a NaN component is equally unusable and equally invisible to a length
// test, so an implementation that only rejected infinities would still let one through.
TEST(TerrainStrokeTargetTests, NaNPositionIsRejected)
{
    json params = json::object();
    params["pos"] = {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0};
    ASSERT_TRUE(params["pos"][0].is_number());
    ASSERT_TRUE(std::isnan(params["pos"][0].get<float>()));

    Vector3 out{7.0f, 7.0f, 7.0f};
    std::string err;

    EXPECT_FALSE(ResolveStrokeTarget(params, "pos", "dir", kSpherical, out, err));
    EXPECT_NE(err.find("not finite"), std::string::npos) << err;
}

// The direction branch reads through the same parse and defends itself with the same length
// test, so it has the same hole: the zero vector fails that test and a non-finite one passes.
TEST(TerrainStrokeTargetTests, NonFiniteDirectionIsRejected)
{
    const json params = json::parse(R"({"dir":[0,1e39,0]})");
    Vector3 out{7.0f, 7.0f, 7.0f};
    std::string err;

    EXPECT_FALSE(ResolveStrokeTarget(params, "pos", "dir", kSpherical, out, err));
    EXPECT_NE(err.find("not finite"), std::string::npos) << err;
}

TEST(TerrainStrokeTargetTests, PositionPassesThroughOnBothDomains)
{
    const json params = {{"pos", {1.0f, 2.0f, 3.0f}}};
    for (const bool spherical : {kPlanar, kSpherical})
    {
        Vector3 out{};
        std::string err;
        ASSERT_TRUE(ResolveStrokeTarget(params, "pos", "dir", spherical, out, err)) << err;
        EXPECT_FLOAT_EQ(out.x, 1.0f);
        EXPECT_FLOAT_EQ(out.y, 2.0f);
        EXPECT_FLOAT_EQ(out.z, 3.0f);
    }
}

TEST(TerrainStrokeTargetTests, ZeroDirectionIsRejected)
{
    const json params = {{"dir", {0.0f, 0.0f, 0.0f}}};
    Vector3 out{1.0f, 1.0f, 1.0f};
    std::string err;

    EXPECT_FALSE(ResolveStrokeTarget(params, "pos", "dir", kSpherical, out, err));
    EXPECT_NE(err.find("zero vector"), std::string::npos)
        << "a zero direction must be rejected on its own branch, got: " << err;
}

TEST(TerrainStrokeTargetTests, SurfaceDirectionMeansNothingOnAPlanarTerrain)
{
    const json params = {{"dir", {0.0f, 1.0f, 0.0f}}};
    Vector3 out{};
    std::string err;

    EXPECT_FALSE(ResolveStrokeTarget(params, "pos", "dir", kPlanar, out, err));
    EXPECT_NE(err.find("pos"), std::string::npos)
        << "the error must say which key to pass instead, got: " << err;
}

TEST(TerrainStrokeTargetTests, NeitherKeyPresentLeavesTheErrorEmpty)
{
    const json params = json::object();
    Vector3 out{};
    std::string err;

    EXPECT_FALSE(ResolveStrokeTarget(params, "pos", "dir", kSpherical, out, err));
    EXPECT_TRUE(err.empty()) << "an absent key is the caller's default, not a malformed request";
}

TEST(TerrainStrokeTargetTests, MalformedVectorIsReported)
{
    const json params = {{"pos", {1.0f, 2.0f}}};
    Vector3 out{};
    std::string err;

    EXPECT_FALSE(ResolveStrokeTarget(params, "pos", "dir", kSpherical, out, err));
    EXPECT_NE(err.find("[x,y,z]"), std::string::npos) << err;
}

// Why the guard sits at resolution rather than downstream: the interpolator cannot defend
// itself. Its own coincident/antipodal early-out is `s < epsilon`, which a NaN passes, so a
// zero endpoint comes out the far side non-finite and every later length check passes too.
TEST(TerrainStrokeTargetTests, SlerpOfAZeroEndpointIsNonFinite)
{
    const Vector3 zero{0.0f, 0.0f, 0.0f};
    const Vector3 good{0.0f, 1.0f, 0.0f};

    const Vector3 normalized = zero.Normalize();
    EXPECT_FALSE(std::isfinite(normalized.x))
        << "normalizing a zero vector must not silently yield a usable direction";

    const Vector3 mid = SlerpDir(normalized, good, 0.5f);
    EXPECT_FALSE(std::isfinite(mid.x) && std::isfinite(mid.y) && std::isfinite(mid.z))
        << "SlerpDir cannot rescue a non-finite input — the rejection belongs upstream";
    EXPECT_FALSE(mid.Length() < 1e-6f)
        << "and a non-finite result passes the downstream length guards, which is the whole trap";
}
} // namespace
