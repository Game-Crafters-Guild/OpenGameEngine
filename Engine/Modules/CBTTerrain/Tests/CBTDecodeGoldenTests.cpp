// Objective decode oracle (no visual verify). The C4 seam/dark-triangle artifacts
// are depth-correlated, so these CPU-side golden tests read CurrentVertex back after
// a deep refinement and check the LEB decode in the EXACT integer domain (re-quantize
// the fp32 UV corners x 2^numSubdiv — lossless when the decode is exact):
//   1. Winding-sign uniformity across all live bisectors at all depths. A parity-
//      dependent corner order flips the signed area sign -> non-uniform -> the
//      graphics winding flips half the triangles (dark-triangle / backface artifact).
//   2. Exact partition of the unit square: |signed areas| sum to the whole square and
//      every sampled interior point is covered by EXACTLY one triangle (no wrong-cell
//      overlaps/gaps).
//   3. Golden corners for hand-computed heapIDs at depths 2/3 (even AND odd path bits).
// Identity terrain params make each corner's world XZ equal its LEB UV, so the read-
// back corners ARE the decode output.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "CBTTerrain/CBTInstance.h"
#include "CBTTerrain/CBTKernelSet.h"
#include "CBTTerrain/CBTLayout.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Core/CommandList.h"
#include "CBTTestHarness.h"

using namespace GameEngine::CBTTerrain;
using namespace GameEngine::CBTTerrain::Test;
using namespace GameEngine::Rendering;

namespace
{
constexpr uint32_t kVertexWordsPerSlot = sizeof(CBTVertexData) / 4u; // 24 — S2a 96B layout (sector tail appended; corner/meta word offsets unchanged)

CBTFrameParams IdentityParams()
{
    CBTFrameParams p = CBTIdentityFrameParams(); // size 1, origin 0, heightScale 0
    return p;
}

// Refine root 0 (focus) to `targetDepth` via the deterministic depth-target metric —
// asymmetric, so the live set spans every depth from baseDepth to targetDepth (all
// winding parities). Returns the number of frames run.
void RefineFocus0(IDevice& device, CBTInstance& instance, uint32_t targetDepth, uint32_t frames)
{
    CBTClassifyDesc refine{};
    refine.Mode = kClassifyDepthTarget;
    refine.FocusRoot = 0u;
    refine.TargetDepth = targetDepth;
    for (uint32_t f = 0; f < frames; ++f)
    {
        auto cl = device.CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        instance.RecordUpdate(*cl, refine, IdentityParams(), f);
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        device.ExecuteCommandLists(lists);
        device.WaitForIdle();
    }
}

struct Tri
{
    std::array<double, 2> c[3];
};

// Read every live bisector's 3 UV corners (world XZ under identity params).
std::vector<Tri> ReadLiveTriangles(CBTInstance& instance, uint32_t slotCount)
{
    const auto heap = instance.DebugReadWords(CBTBinding::HeapID, slotCount * 2u);
    const auto verts =
        instance.DebugReadWords(CBTBinding::CurrentVertex, slotCount * kVertexWordsPerSlot);
    std::vector<Tri> tris;
    for (uint32_t slot = 0; slot < slotCount; ++slot)
    {
        // HeapID is u64; depth <= 12 fits the low word. 0 == free slot.
        if (heap[slot * 2u] == 0u && heap[slot * 2u + 1u] == 0u)
            continue;
        Tri t;
        const uint32_t base = slot * kVertexWordsPerSlot;
        for (uint32_t k = 0; k < 3u; ++k)
        {
            float x, z;
            std::memcpy(&x, &verts[base + k * 4u + 0u], sizeof(float)); // worldX == uv.x
            std::memcpy(&z, &verts[base + k * 4u + 2u], sizeof(float)); // worldZ == uv.y
            t.c[k] = {static_cast<double>(x), static_cast<double>(z)};
        }
        tris.push_back(t);
    }
    return tris;
}

double DoubledSignedArea(const Tri& t)
{
    return (t.c[1][0] - t.c[0][0]) * (t.c[2][1] - t.c[0][1]) -
           (t.c[2][0] - t.c[0][0]) * (t.c[1][1] - t.c[0][1]);
}

double Orient(const std::array<double, 2>& a, const std::array<double, 2>& b, double px, double py)
{
    return (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
}

// Point strictly inside the triangle (consistent-sign edge test; each triangle judged
// by its own winding so a mixed-winding set is still handled).
bool PointInTri(const Tri& t, double px, double py)
{
    const double d0 = Orient(t.c[0], t.c[1], px, py);
    const double d1 = Orient(t.c[1], t.c[2], px, py);
    const double d2 = Orient(t.c[2], t.c[0], px, py);
    const bool allNeg = d0 < 0.0 && d1 < 0.0 && d2 < 0.0;
    const bool allPos = d0 > 0.0 && d1 > 0.0 && d2 > 0.0;
    return allNeg || allPos;
}

// --- Edge-conformity (T-junction) oracle, exact integer domain ---
// T-junctions have ZERO UV area, so the area/coverage oracle is blind to them, yet
// they open real 3D cracks under displacement (the coarse side interpolates height
// linearly along an edge while the fine side samples the true midpoint). Re-quantize
// every corner to a grid far finer than any live depth (2^20; lossless since the UV
// corners are dyadic), build the undirected integer-edge multiset, and assert every
// INTERIOR edge is shared by exactly two triangles (boundary edges by one). A missed
// split/merge conforming propagation leaves a coarse edge with a neighbour vertex on
// it -> that edge's count is 1 (its two halves each 1) -> caught here.
constexpr int64_t kConformGrid = 1 << 20;

struct ITri
{
    std::array<int64_t, 2> c[3];
};

// Reads each live triangle's stored TERRAIN UV (corner.w = uv.x, meta[k] = uv.y),
// re-quantized to the 2^20 grid. UV is in [0,1] regardless of terrain size/origin, so
// conformity works with any terrain params (identity or a real screen-space camera).
std::vector<ITri> ReadLiveTrianglesInt(CBTInstance& instance, uint32_t slotCount)
{
    const auto heap = instance.DebugReadWords(CBTBinding::HeapID, slotCount * 2u);
    const auto verts =
        instance.DebugReadWords(CBTBinding::CurrentVertex, slotCount * kVertexWordsPerSlot);
    std::vector<ITri> tris;
    for (uint32_t slot = 0; slot < slotCount; ++slot)
    {
        if (heap[slot * 2u] == 0u && heap[slot * 2u + 1u] == 0u)
            continue;
        ITri t;
        const uint32_t base = slot * kVertexWordsPerSlot;
        for (uint32_t k = 0; k < 3u; ++k)
        {
            float ux, uy;
            std::memcpy(&ux, &verts[base + k * 4u + 3u], sizeof(float)); // corner.w = uv.x
            std::memcpy(&uy, &verts[base + 12u + k], sizeof(float));     // meta[k]  = uv.y
            t.c[k] = {std::llround(static_cast<double>(ux) * static_cast<double>(kConformGrid)),
                      std::llround(static_cast<double>(uy) * static_cast<double>(kConformGrid))};
        }
        tris.push_back(t);
    }
    return tris;
}

// Count interior edges NOT shared by exactly two triangles (T-junctions / overlaps).
uint32_t CountNonConformingEdges(const std::vector<ITri>& tris)
{
    std::map<std::array<int64_t, 4>, int> counts;
    auto add = [&](const std::array<int64_t, 2>& a, const std::array<int64_t, 2>& b) {
        std::array<int64_t, 4> key = (a[0] < b[0] || (a[0] == b[0] && a[1] < b[1]))
                                         ? std::array<int64_t, 4>{a[0], a[1], b[0], b[1]}
                                         : std::array<int64_t, 4>{b[0], b[1], a[0], a[1]};
        ++counts[key];
    };
    for (const ITri& t : tris)
    {
        add(t.c[0], t.c[1]);
        add(t.c[1], t.c[2]);
        add(t.c[2], t.c[0]);
    }
    uint32_t bad = 0;
    for (const auto& [key, cnt] : counts)
    {
        const bool border = (key[0] == 0 && key[2] == 0) ||
                            (key[0] == kConformGrid && key[2] == kConformGrid) ||
                            (key[1] == 0 && key[3] == 0) ||
                            (key[1] == kConformGrid && key[3] == kConformGrid);
        const int expected = border ? 1 : 2;
        if (cnt != expected)
            ++bad;
    }
    return bad;
}
} // namespace

// Parameterized over the kernel blob: the shipped u64 arm and the narrow-heap
// (GE_CBT_HEAP32) arm the web cook translates to WGSL. Before this, the u32 arm
// had never met any test — only the browser ran it. Same oracles, both arms.
class CBTDecodeGoldenTest : public ::testing::TestWithParam<const char*>
{
  protected:
    void SetUp() override
    {
        m_Device = MakeHeadlessDevice();
        if (!m_Device)
            GTEST_SKIP() << "no headless Vulkan device";
        const bool heap32 = std::string(GetParam()) == "cbt_kernels_heap32.comp.spv";
        // The u32 arm needs no 64-bit integers — that is its reason to exist.
        if (!heap32 && !m_Device->GetCapabilities().supportsShaderInt64)
            GTEST_SKIP() << "device lacks shaderInt64";
        if (!m_KernelSet.Initialize(*m_Device, ShaderOutputDir(), GetParam()))
            GTEST_SKIP() << GetParam() << " missing (glslc unavailable at build)";
    }
    void TearDown() override
    {
        m_KernelSet.Shutdown();
        if (m_Device)
            m_Device->Shutdown();
    }
    std::unique_ptr<IDevice> m_Device;
    CBTKernelSet m_KernelSet;
};

// ORACLE 1: the RAW decode must produce a uniform winding sign across all depths.
// The reference splitting matrix preserves orientation (each child's signed area is
// half the parent's, same sign), so every triangle is CW like the roots — verified
// directly here, with no per-triangle winding correction in the kernel. A decode
// convention that flipped orientation per subdivision would fail this.
TEST_P(CBTDecodeGoldenTest, WindingSignIsUniformAcrossDepths)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());
    RefineFocus0(*m_Device, instance, kDefaultBaseDepth + 11u, 16u); // to depth 12

    const std::vector<Tri> tris = ReadLiveTriangles(instance, 4096u);
    ASSERT_GT(tris.size(), 64u) << "refinement did not produce a deep tree";

    uint32_t positive = 0, negative = 0, degenerate = 0;
    for (const Tri& t : tris)
    {
        const double a = DoubledSignedArea(t);
        if (a > 1e-9)
            ++positive;
        else if (a < -1e-9)
            ++negative;
        else
            ++degenerate;
    }
    EXPECT_EQ(degenerate, 0u) << "degenerate triangle in the decode";
    EXPECT_TRUE(positive == 0u || negative == 0u)
        << "winding sign is NOT uniform: " << positive << " CCW + " << negative
        << " CW triangles — the decode splitting convention does not preserve orientation";
}

// ORACLE 2: the live triangles tile the unit square EXACTLY — total absolute area
// equals the square, and a dense grid of interior points is each covered exactly once
// (no wrong-cell overlaps or gaps).
TEST_P(CBTDecodeGoldenTest, LiveTrianglesTileUnitSquareExactly)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());
    RefineFocus0(*m_Device, instance, kDefaultBaseDepth + 11u, 16u);

    const std::vector<Tri> tris = ReadLiveTriangles(instance, 4096u);
    ASSERT_GT(tris.size(), 64u);

    // Total area = the unit square (2 triangles of doubled-area 1 each -> total 1.0).
    double totalDoubled = 0.0;
    for (const Tri& t : tris)
        totalDoubled += std::abs(DoubledSignedArea(t));
    EXPECT_NEAR(totalDoubled, 2.0, 1e-4)
        << "live triangles do not sum to the unit square (wrong-cell overlap/gap)";

    // Coverage: interior sample grid, each point in EXACTLY one triangle. Points that
    // fall on/near a shared edge are ambiguous under a strict inside-test at this
    // tessellation (perpendicular distance < kEdgeEps), so skip them — a genuine
    // wrong-cell bug shows as strictHits != 1 AWAY from every edge (real gap/overlap).
    constexpr int kN = 61; // coprime-ish with dyadic grid lines
    constexpr double kEdgeEps = 1.0e-4; // UV perpendicular distance to an edge
    uint32_t wrong = 0;
    uint32_t skipped = 0;
    uint32_t firstBadCount = 0;
    for (int i = 0; i < kN; ++i)
    {
        for (int j = 0; j < kN; ++j)
        {
            const double px = (i + 0.5) / kN;
            const double py = (j + 0.5) / kN;
            uint32_t strictHits = 0;
            bool nearEdge = false;
            for (const Tri& t : tris)
            {
                if (PointInTri(t, px, py))
                    ++strictHits;
                // Perpendicular distance to each edge; near an edge inside its span.
                for (uint32_t e = 0; e < 3u; ++e)
                {
                    const auto& a = t.c[e];
                    const auto& b = t.c[(e + 1u) % 3u];
                    const double len = std::hypot(b[0] - a[0], b[1] - a[1]);
                    if (len < 1e-12)
                        continue;
                    const double perp = std::abs(Orient(a, b, px, py)) / len;
                    // within the edge's span (dot in [0,len])
                    const double dot = ((px - a[0]) * (b[0] - a[0]) + (py - a[1]) * (b[1] - a[1])) / len;
                    if (perp < kEdgeEps && dot > -kEdgeEps && dot < len + kEdgeEps)
                        nearEdge = true;
                }
            }
            if (strictHits == 1u)
                continue;
            if (nearEdge)
            {
                ++skipped;
                continue;
            }
            if (wrong == 0)
                firstBadCount = strictHits;
            ++wrong;
        }
    }
    EXPECT_EQ(wrong, 0u) << wrong << " of " << (kN * kN) << " interior points (skipped "
                         << skipped << " near edges) were not covered exactly once (first bad = "
                         << firstBadCount << ") — decode maps a bisector to the wrong cell";
}

// ORACLE 3: golden corners for hand-computed heapIDs. After one/two splits of root 0,
// locate each child by heapID and assert its corner SET matches the hand-derived LEB
// decode. Catches a systematic corner-order/cell error independent of oracles 1-2.
TEST_P(CBTDecodeGoldenTest, GoldenCornersMatchHandComputed)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());
    RefineFocus0(*m_Device, instance, kDefaultBaseDepth + 2u, 8u); // to depth 3

    constexpr uint32_t kSlots = 64u;
    const auto heap = instance.DebugReadWords(CBTBinding::HeapID, kSlots * 2u);
    const auto verts =
        instance.DebugReadWords(CBTBinding::CurrentVertex, kSlots * kVertexWordsPerSlot);

    auto cornerSet = [&](uint32_t slot) {
        std::array<std::array<float, 2>, 3> c{};
        const uint32_t base = slot * kVertexWordsPerSlot;
        for (uint32_t k = 0; k < 3u; ++k)
        {
            std::memcpy(&c[k][0], &verts[base + k * 4u + 0u], sizeof(float));
            std::memcpy(&c[k][1], &verts[base + k * 4u + 2u], sizeof(float));
        }
        return c;
    };
    auto contains = [](const std::array<std::array<float, 2>, 3>& c, float x, float y) {
        for (const auto& p : c)
            if (std::abs(p[0] - x) < 1e-4f && std::abs(p[1] - y) < 1e-4f)
                return true;
        return false;
    };
    auto findSlot = [&](uint32_t heapId) -> uint32_t {
        for (uint32_t s = 0; s < kSlots; ++s)
            if (heap[s * 2u] == heapId && heap[s * 2u + 1u] == 0u)
                return s;
        return 0xFFFFFFFFu;
    };

    // Hand-derived LEB corner sets (root 0 = {(1,0),(0,0),(0,1)}, reference split
    // matrix: bit 0 -> (v2,mid,v1), bit 1 -> (v1,mid,v0)):
    //   heapID 8 (bits 0,0): {(0,0),(0,0.5),(0.5,0.5)}
    //   heapID 9 (bits 0,1): {(0.5,0.5),(0,0.5),(0,1)}
    struct Golden
    {
        uint32_t heapId;
        std::array<std::array<float, 2>, 3> corners;
    };
    const std::array<Golden, 2> golden = {{
        {8u, {{{0.0f, 0.0f}, {0.0f, 0.5f}, {0.5f, 0.5f}}}},
        {9u, {{{0.5f, 0.5f}, {0.0f, 0.5f}, {0.0f, 1.0f}}}},
    }};

    for (const Golden& g : golden)
    {
        const uint32_t slot = findSlot(g.heapId);
        ASSERT_NE(slot, 0xFFFFFFFFu) << "heapID " << g.heapId << " not found at depth 3";
        const auto c = cornerSet(slot);
        for (const auto& expected : g.corners)
            EXPECT_TRUE(contains(c, expected[0], expected[1]))
                << "heapID " << g.heapId << " missing corner (" << expected[0] << ", "
                << expected[1] << ")";
    }
}

// ORACLE 4: a statically refined tree must have no T-junctions. The permanent guard
// against the decode/topology-convention divergence that C4 fixed (the LEB split
// matrix in VertexEval must match the reference's bit->half mapping, else the
// neighbour topology and the decoded geometry disagree and every depth transition
// opens a crack — invisible to the area/coverage oracle, flat C3 hid it).
TEST_P(CBTDecodeGoldenTest, EdgeConformityStaticRefined)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());
    RefineFocus0(*m_Device, instance, kDefaultBaseDepth + 11u, 16u); // to depth 12

    const std::vector<ITri> tris = ReadLiveTrianglesInt(instance, 4096u);
    ASSERT_GT(tris.size(), 64u);
    EXPECT_EQ(CountNonConformingEdges(tris), 0u)
        << "static refined tree has T-junctions — the split path did not conform";
}

// ORACLE 5: edge conformity through a MERGE cycle. Merging a diamond while a leg
// neighbour is one level deeper would reintroduce a midpoint T-junction (invisible to
// the area/coverage oracle, but a real crack). Refine, then relax so the merge path
// runs, and assert conformity on EVERY frame of the collapse.
TEST_P(CBTDecodeGoldenTest, EdgeConformityThroughMergeCycle)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    auto runFrame = [&](const CBTClassifyDesc& desc, uint32_t f) {
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        instance.RecordUpdate(*cl, desc, IdentityParams(), f);
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();
    };

    CBTClassifyDesc refine{};
    refine.Mode = kClassifyDepthTarget;
    refine.FocusRoot = 0u;
    refine.TargetDepth = kDefaultBaseDepth + 8u; // depth 9
    uint32_t frame = 0;
    for (uint32_t i = 0; i < 12u; ++i, ++frame)
        runFrame(refine, frame);

    // Relax everything back toward the base: the merge path collapses the tree one
    // level per frame. Conformity must hold on every intermediate frame (that is what
    // the editor renders mid-collapse).
    const CBTClassifyDesc mergeAll = CBTInertClassify();
    for (uint32_t i = 0; i < 12u; ++i, ++frame)
    {
        runFrame(mergeAll, frame);
        const std::vector<ITri> tris = ReadLiveTrianglesInt(instance, 4096u);
        ASSERT_GE(tris.size(), 2u);
        EXPECT_EQ(CountNonConformingEdges(tris), 0u)
            << "T-junction during merge at frame " << frame << " (" << tris.size()
            << " live triangles) — the merge path left a leg neighbour non-conforming";
    }
}

// ORACLE 6a: PRODUCTION conformity — the screen-space metric with a real perspective
// camera (what the editor actually renders). Unlike the depth-target focus metric
// (which can be handed an unsatisfiable hard depth step), screen-space produces a
// smooth distance-based gradient — the case the editor hits. Converge, then assert
// zero T-junctions. THIS is the oracle that speaks to the on-screen sliver report.
TEST_P(CBTDecodeGoldenTest, EdgeConformityScreenSpacePerspectiveConverged)
{
    using namespace GameEngine::Mathematics;
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    // Grazing perspective camera over a 500 m terrain centred at the origin — a
    // strong near/far depth gradient, exactly the editor's stress.
    const Matrix4x4 view = MakeLookAtLH(Vector3(0.0f, 60.0f, -320.0f), Vector3(0.0f, 0.0f, 0.0f),
                                        Vector3(0.0f, 1.0f, 0.0f));
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(1.0472f /*60 deg*/, 16.0f / 9.0f, 0.5f,
                                                         3000.0f);
    const Matrix4x4 vp = proj * view;

    CBTFrameParams p{};
    // Near origin (RenderOriginSector default 0): viewProjRel == the world viewProj.
    std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
    p.CameraPos[0] = 0.0f; p.CameraPos[1] = 60.0f; p.CameraPos[2] = -320.0f; p.CameraPos[3] = 1.0f;
    p.Screen[0] = 1600.0f; p.Screen[1] = 900.0f; p.Screen[2] = 24.0f; p.Screen[3] = 12.0f;
    p.TerrainSize[0] = 500.0f; p.TerrainSize[1] = 500.0f; p.TerrainSize[2] = 0.0f; p.TerrainSize[3] = 0.0f;
    p.TerrainOrigin[0] = -250.0f; p.TerrainOrigin[1] = -250.0f; p.TerrainOrigin[2] = 16.0f; p.TerrainOrigin[3] = 0.0f;

    CBTClassifyDesc classify{};
    classify.Mode = kClassifyScreenSpace;
    classify.TargetDepth = 16u;
    for (uint32_t f = 0; f < 32u; ++f)
    {
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        instance.RecordUpdate(*cl, classify, p, f);
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();
    }

    // Read the WHOLE pool: near-camera refinement can exceed a few thousand live
    // bisectors, and slot reuse scatters them — a truncated read would report false
    // non-conformity.
    const std::vector<ITri> tris = ReadLiveTrianglesInt(instance, kDefaultBisectorPoolSize);
    ASSERT_GT(tris.size(), 64u) << "screen-space refinement produced no gradient";
    const uint32_t bad = CountNonConformingEdges(tris);
    std::printf("[screenspace] %zu tris, non-conforming edges %u\n", tris.size(), bad);
    EXPECT_EQ(bad, 0u) << "screen-space (production) terrain has T-junctions — real crack source";
}

// ORACLE 6: edge conformity on EVERY frame during a camera sweep. The editor renders
// whatever topology exists at the end of each RecordUpdate; if a split/merge only
// half-propagates within one frame, that transient state has T-junctions -> per-frame
// cracks. Alternate the focus root each frame (teleporting refinement, like the soak)
// and assert conformity every frame — proves splits+merges complete (conform) within
// a single RecordUpdate.
TEST_P(CBTDecodeGoldenTest, EdgeConformityEveryFrameDuringSweep)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    constexpr uint32_t kFrames = 24u;
    for (uint32_t frame = 0; frame < kFrames; ++frame)
    {
        CBTClassifyDesc desc{};
        desc.Mode = kClassifyDepthTarget;
        desc.FocusRoot = frame % 2u;                                // teleport focus each frame
        desc.TargetDepth = kDefaultBaseDepth + 2u + (frame % 5u);   // depth 3..7
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        instance.RecordUpdate(*cl, desc, IdentityParams(), frame);
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();

        const std::vector<ITri> tris = ReadLiveTrianglesInt(instance, 2048u);
        EXPECT_EQ(CountNonConformingEdges(tris), 0u)
            << "T-junction at sweep frame " << frame << " (focus " << (frame % 2u) << ", "
            << tris.size() << " live triangles) — the rendered topology is non-conforming mid-update";
    }
}

// ORACLE 7: the GPU's own Validate kernel must stay silent on both heap arms. The
// geometric oracles above read decoded corners; this reads the neighbor LINKS the
// decode rides on (reciprocity, twin ordering, occupancy/HeapID agreement). A link
// break is what opens sliver punctures that per-triangle conformity can still miss,
// and until this ran the narrow-heap arm's links had never been asserted at all.
TEST_P(CBTDecodeGoldenTest, GpuValidateIsSilentDuringSweep)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());
    ASSERT_TRUE(instance.ValidatesEachUpdate()) << "fixture default changed; Validate never ran";

    constexpr uint32_t kFrames = 24u;
    for (uint32_t frame = 0; frame < kFrames; ++frame)
    {
        CBTClassifyDesc desc{};
        desc.Mode = kClassifyDepthTarget;
        desc.FocusRoot = frame % 2u;
        desc.TargetDepth = kDefaultBaseDepth + 2u + (frame % 5u);
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        instance.RecordUpdate(*cl, desc, IdentityParams(), frame);
        instance.RecordReadback(*cl);
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();

        EXPECT_EQ(instance.ReadValidationErrorCount(), 0u)
            << "non-reciprocal neighbor links at sweep frame " << frame;
        EXPECT_EQ(instance.ReadValidationCounter(kValidationZombieCounter), 0u)
            << "occupancy/HeapID disagreement at sweep frame " << frame;
        EXPECT_EQ(instance.ReadValidationCounter(kValidationCompactCounter), 0u)
            << "compact stream does not cover the live set at sweep frame " << frame;
        EXPECT_EQ(instance.ReadValidationCounter(kValidationBudgetCounter), 0u)
            << "free-slot budget went negative at sweep frame " << frame;
    }
}

// ORACLE 8: the same link invariants at the DEPTH THE EDITOR ACTUALLY REACHES. The
// sweep above tops out at depth 7 with a few thousand bisectors; a 512 m terrain
// drives depth ~19 and several hundred thousand, which is a different regime for the
// allocator, the sum-tree and the propagate kernels. Deep refinement is where a link
// break would show up as sliver punctures in the drawn mesh.
TEST_P(CBTDecodeGoldenTest, GpuValidateIsSilentAtEditorDepth)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    constexpr uint32_t kTargetDepth = kDefaultBaseDepth + 18u; // depth 19, as the editor reaches
    constexpr uint32_t kFrames = 40u;
    for (uint32_t frame = 0; frame < kFrames; ++frame)
    {
        CBTClassifyDesc desc{};
        desc.Mode = kClassifyDepthTarget;
        desc.FocusRoot = 0u;
        desc.TargetDepth = kTargetDepth;
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        instance.RecordUpdate(*cl, desc, IdentityParams(), frame);
        instance.RecordReadback(*cl);
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();

        const uint32_t links = instance.ReadValidationErrorCount();
        const uint32_t live = instance.ReadDrawIndexCount(kDrawStreamAll) / 3u;
        if (links != 0u)
            std::printf("[deep] frame %u: %u live, %u non-reciprocal links\n", frame, live, links);
        ASSERT_EQ(links, 0u) << "non-reciprocal neighbor links at depth " << kTargetDepth
                             << ", frame " << frame << " (" << live << " live bisectors)";
    }
    const uint32_t finalLive = instance.ReadDrawIndexCount(kDrawStreamAll) / 3u;
    std::printf("[deep] converged at %u live bisectors, links clean\n", finalLive);
    std::printf("[deep] work split=%d alloc=%d propBisect=%d propSimplify=%d simplify=%d "
                "simplifyClass=%d overflow=%d\n",
                instance.ReadWorkQueueCounter(kWQSplitCounter),
                instance.ReadWorkQueueCounter(kWQAllocateCounter),
                instance.ReadWorkQueueCounter(kWQPropagateBisectCounter),
                instance.ReadWorkQueueCounter(kWQPropagateSimplifyCounter),
                instance.ReadWorkQueueCounter(kWQSimplifyCounter),
                instance.ReadWorkQueueCounter(kWQSimplifyClassCounter),
                instance.ReadWorkQueueCounter(kWQOverflowCounter));
    EXPECT_GT(finalLive, 100000u) << "refinement never reached the editor's regime";
}

INSTANTIATE_TEST_SUITE_P(HeapArms, CBTDecodeGoldenTest,
                         ::testing::Values("cbt_kernels.comp.spv", "cbt_kernels_heap32.comp.spv"),
                         [](const ::testing::TestParamInfo<const char*>& info) {
                             return std::string(info.param) == "cbt_kernels.comp.spv" ? "u64"
                                                                                      : "heap32";
                         });
