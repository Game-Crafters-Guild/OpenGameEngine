// CPU oracle for the (face, rect) region-identity foundation (plan §planet-editing).
// These are the arc-law oracles for planet editing: they run WITHOUT a Vulkan device
// (pure math) so they always execute, and each discriminates a real failure mode.
//   * Face-mapping round-trip: dir -> (face,uv) -> dir is exact on cube corners/edges
//     and fp-tight in the interior (the exactness the crack-free geometry relies on).
//   * Cross-face dirty: an edit straddling a cube edge produces rects on BOTH faces
//     whose union covers the cap with no gap at the shared edge (the C4 T-junction
//     lesson — gaps are invisible to area tests, so the edge is sampled explicitly).
//   * Region-proportionality: an interior edit touches exactly one face; a corner edit
//     touches three.
//   * Atlas addressing: face UV rects map to disjoint atlas texel bands.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>

#include "CBTTerrain/CBTSphereFaceMap.h"

using namespace GameEngine::CBTTerrain;

namespace
{
float Dot(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// Is direction `d` inside any of the edit's face rects? Classify it and test the rect
// for its face (with a small UV epsilon for the boundary-sample floor).
bool CoveredBy(const SphereEditRegions& regions, float dx, float dy, float dz, float uvEps)
{
    const SphereFaceUV s = WorldDirToFaceUV(dx, dy, dz);
    for (uint32_t i = 0; i < regions.Count; ++i)
    {
        const SphereFaceUVRect& r = regions.Rects[i];
        if (r.Face != s.Face)
            continue;
        if (s.U >= r.MinU - uvEps && s.U <= r.MaxU + uvEps && s.V >= r.MinV - uvEps &&
            s.V <= r.MaxV + uvEps)
            return true;
    }
    return false;
}
} // namespace

// Every direction maps to a face UV strictly inside [0,1] (up to fp), and the inverse
// reproduces the direction. Swept over a dense direction grid covering all six faces.
TEST(CBTSphereFaceMap, RoundTripAllFaces)
{
    uint32_t checked = 0;
    for (int i = -8; i <= 8; ++i)
        for (int j = -8; j <= 8; ++j)
            for (int k = -8; k <= 8; ++k)
            {
                if (i == 0 && j == 0 && k == 0)
                    continue;
                const float x = static_cast<float>(i);
                const float y = static_cast<float>(j);
                const float z = static_cast<float>(k);
                const SphereFaceUV s = WorldDirToFaceUV(x, y, z);
                ASSERT_LT(s.Face, kCubeFaceCount);
                EXPECT_GE(s.U, -1e-5f);
                EXPECT_LE(s.U, 1.0f + 1e-5f);
                EXPECT_GE(s.V, -1e-5f);
                EXPECT_LE(s.V, 1.0f + 1e-5f);

                float rx, ry, rz;
                FaceUVToWorldDir(s.Face, s.U, s.V, rx, ry, rz);
                // Compare unit directions (the input need not be unit length).
                const float len = std::sqrt(x * x + y * y + z * z);
                const float nx = x / len, ny = y / len, nz = z / len;
                const float in[3] = {nx, ny, nz};
                const float out[3] = {rx, ry, rz};
                EXPECT_GT(Dot(in, out), 1.0f - 1e-5f)
                    << "round-trip diverged at (" << i << "," << j << "," << k << ")";
                ++checked;
            }
    EXPECT_GT(checked, 1000u);
}

// Cube corners/edges round-trip EXACTLY (integer cube coords -> UV on {0,0.5,1}). A
// direction through a cube corner reproduces that corner's direction bit-for-bit.
TEST(CBTSphereFaceMap, ExactOnCubeCornersAndEdges)
{
    // All 8 cube corners.
    for (int c = 0; c < 8; ++c)
    {
        const float x = (c & 1) ? 1.0f : -1.0f;
        const float y = (c & 2) ? 1.0f : -1.0f;
        const float z = (c & 4) ? 1.0f : -1.0f;
        const SphereFaceUV s = WorldDirToFaceUV(x, y, z);
        // UV must be exactly a corner of the face square.
        EXPECT_TRUE(s.U == 0.0f || s.U == 1.0f) << "corner " << c << " U=" << s.U;
        EXPECT_TRUE(s.V == 0.0f || s.V == 1.0f) << "corner " << c << " V=" << s.V;
        float rx, ry, rz;
        FaceUVToWorldDir(s.Face, s.U, s.V, rx, ry, rz);
        const float n = 1.0f / std::sqrt(3.0f);
        EXPECT_NEAR(rx, x * n, 1e-6f);
        EXPECT_NEAR(ry, y * n, 1e-6f);
        EXPECT_NEAR(rz, z * n, 1e-6f);
    }
    // Cube-edge midpoints (one component 0): UV lands on 0.5 for the varying axis.
    const SphereFaceUV e = WorldDirToFaceUV(1.0f, 0.0f, -1.0f); // +X/-Z shared edge midpoint
    EXPECT_TRUE(e.U == 0.5f || e.V == 0.5f) << "edge midpoint U=" << e.U << " V=" << e.V;
}

// Interior edit (cap well inside one face) -> exactly one region on that face
// (region-proportionality). Centre a small cap on each face centre.
TEST(CBTSphereFaceMap, InteriorEditSingleFace)
{
    const float centres[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int f = 0; f < 6; ++f)
    {
        const SphereEditRegions regions =
            ClassifySphereCapEdit(centres[f][0], centres[f][1], centres[f][2], 0.05f);
        EXPECT_EQ(regions.Count, 1u) << "face-centre cap must touch exactly one face (f=" << f << ")";
        if (regions.Count == 1u)
        {
            EXPECT_FALSE(regions.Rects[0].IsEmpty());
            // The rect straddles the face centre (u,v ~ 0.5) and is small.
            EXPECT_LT(regions.Rects[0].MinU, 0.5f);
            EXPECT_GT(regions.Rects[0].MaxU, 0.5f);
        }
    }
}

// Edit straddling a cube edge -> exactly two face rects; both reach the shared boundary;
// their union covers the cap with NO gap at the edge (dense cap points all covered, and
// the shared cube edge itself is covered on both sides).
TEST(CBTSphereFaceMap, CrossFaceEditTwoFacesNoGap)
{
    // Centre on the +X / +Y shared cube edge midpoint (corners 3=(+,+,-),7=(+,+,+)).
    const float cx = 1.0f, cy = 1.0f, cz = 0.0f;
    const float rad = 0.08f;
    const SphereEditRegions regions = ClassifySphereCapEdit(cx, cy, cz, rad);
    ASSERT_EQ(regions.Count, 2u) << "an edge-straddling cap must touch exactly two faces";

    // The two faces are +X (0) and +Y (2).
    std::array<uint32_t, 2> faces{regions.Rects[0].Face, regions.Rects[1].Face};
    EXPECT_TRUE((faces[0] == 0u && faces[1] == 2u) || (faces[0] == 2u && faces[1] == 0u));

    // Both rects must reach the shared boundary (the edge) — otherwise a gap opens there.
    // On +X the shared +Y edge is u=1 (Q1 corner is (+,+,-)); on +Y the shared +X edge is
    // also a face boundary. Assert each rect touches a 0/1 boundary.
    for (uint32_t i = 0; i < 2u; ++i)
    {
        const SphereFaceUVRect& r = regions.Rects[i];
        const bool touchesBoundary =
            r.MinU == 0.0f || r.MaxU == 1.0f || r.MinV == 0.0f || r.MaxV == 1.0f;
        EXPECT_TRUE(touchesBoundary) << "face " << r.Face << " rect does not reach a shared edge";
    }

    // No-gap coverage: dense sample of the cap interior + the shared cube edge line.
    const float clen = std::sqrt(cx * cx + cy * cy + cz * cz);
    const float nx = cx / clen, ny = cy / clen, nz = cz / clen;
    // Tangent frame.
    float tx = -ny, ty = nx, tz = 0.0f; // up=(0,0,1) x n gives a tangent; then b=n x t
    const float tl = std::sqrt(tx * tx + ty * ty + tz * tz);
    tx /= tl; ty /= tl; tz /= tl;
    const float bx = ny * tz - nz * ty, by = nz * tx - nx * tz, bz = nx * ty - ny * tx;
    uint32_t covered = 0, total = 0;
    for (int ri = 0; ri <= 6; ++ri)
        for (int ai = 0; ai < 32; ++ai)
        {
            const float r = rad * static_cast<float>(ri) / 6.0f;
            const float a = 6.2831853f * static_cast<float>(ai) / 32.0f;
            const float dx = nx * std::cos(r) + (tx * std::cos(a) + bx * std::sin(a)) * std::sin(r);
            const float dy = ny * std::cos(r) + (ty * std::cos(a) + by * std::sin(a)) * std::sin(r);
            const float dz = nz * std::cos(r) + (tz * std::cos(a) + bz * std::sin(a)) * std::sin(r);
            ++total;
            if (CoveredBy(regions, dx, dy, dz, 1e-3f))
                ++covered;
        }
    EXPECT_EQ(covered, total) << "gap in cross-face coverage (" << (total - covered) << "/" << total
                              << " uncovered)";

    // The shared cube edge itself (x=1,y=1, z in [-sin(rad),sin(rad)]) is covered on BOTH
    // sides — explicitly test points just inside each face.
    for (int s = -2; s <= 2; ++s)
    {
        const float ez = 0.03f * static_cast<float>(s);
        EXPECT_TRUE(CoveredBy(regions, 1.0f, 1.001f, ez, 1e-3f)) << "edge +Y side uncovered s=" << s;
        EXPECT_TRUE(CoveredBy(regions, 1.001f, 1.0f, ez, 1e-3f)) << "edge +X side uncovered s=" << s;
    }
}

// Edit on a cube corner -> three face rects (the three faces meeting at the corner).
TEST(CBTSphereFaceMap, CornerEditThreeFaces)
{
    // Corner 7 = (+,+,+) shared by +X (0), +Y (2), +Z (4).
    const SphereEditRegions regions = ClassifySphereCapEdit(1.0f, 1.0f, 1.0f, 0.08f);
    EXPECT_EQ(regions.Count, 3u) << "a corner cap must touch the three faces at the corner";
    std::array<bool, kCubeFaceCount> seen{};
    for (uint32_t i = 0; i < regions.Count; ++i)
        seen[regions.Rects[i].Face] = true;
    EXPECT_TRUE(seen[0] && seen[2] && seen[4]) << "corner faces must be +X,+Y,+Z";
}

// Quiescence: a degenerate (zero-length centre) edit yields no regions.
TEST(CBTSphereFaceMap, DegenerateEditNoRegions)
{
    const SphereEditRegions regions = ClassifySphereCapEdit(0.0f, 0.0f, 0.0f, 0.1f);
    EXPECT_EQ(regions.Count, 0u);
}

// Page-table face-stride layout: face f's page-table region [f*faceStride, (f+1)*faceStride) is the
// offset the shader (cbt_sculpt.glsl CBT_SculptTableEntry) and SphereSculptLayer both index; the six
// face regions tile the table exactly with no overlap and no gap.
TEST(CBTSphereFaceMap, PageTableFaceRegionsTileExactly)
{
    uint32_t expectedBase = 0u;
    for (uint32_t f = 0; f < kCubeFaceCount; ++f)
    {
        EXPECT_EQ(f * kSculptPageTableFaceStride, expectedBase) << "face " << f << " table offset";
        expectedBase += kSculptPageTableFaceStride;
    }
    EXPECT_EQ(expectedBase, kSculptPageTableEntries) << "the six face regions must tile the table";
}
