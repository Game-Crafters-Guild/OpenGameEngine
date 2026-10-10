// Earth-scale decode-precision probes (arc slice S1 — the arc's regression baseline).
//
// The first block is CPU probes (no Vulkan device): they measure the DECODE + STORAGE
// representation itself, using the exact int64 LEB walk (CBTDeepDecode.h WalkBary, the
// bit-twin of Kernel_VertexEval) against a double-precision reference. The CBTDeepDecodeGpu
// block below (S2a) then holds the LIVE pipeline to the library: GPU<->CPU bit-exactness,
// the dark-ship tail-zero oracle, flag-ON parity/no-swim at R = 50 km, and the quiescent
// cost A/B. What the CPU probes pin:
//
//   1. THE MEASURED WALL (fails-before for the whole arc): at R = 6.371e6 m the int64
//      walk stays exact well past the cap — what breaks is fp32 WORLD-corner storage
//      (gVertex), whose ~0.5 m/component quantization at Earth magnitude exceeds the
//      facet size a few subdivisions past the shipped cap (kMaxDecodeSubdiv = 40,
//      facet floor ~9.54 m). NOT the u64 heap key (holds depth ~58), NOT the walk
//      (exact to ~52). The #524 camera-relative consumption cannot mask it: the
//      quantization happens at store, before the origin subtraction.
//   2. THE CURE'S REPRESENTATION (gated prototype): the (sector, local) deep decode
//      (df64 arithmetic, GE_ClipFromSectorLocal-style consumption) holds the decode
//      error orders of magnitude below the facet size through numSubdiv 50 — where the
//      Earth facet floor is 0.298 m, under the 0.5 m walking bar.
//   3. DETERMINISM (the crack-freedom contract the S2 GPU port must keep): shared
//      corners decode bit-identically across siblings, across depth levels (2N vs N),
//      and across cube-face seams.
//
// Dark-ship: nothing here touches the live pipeline; GE_CBT_DEEP_DECODE gates only the
// prototype library's cap accessor (EnvFlagGatesDeepCap pins both states).

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "CBTTerrain/CBTDeepDecode.h"
#include "CBTTerrain/CBTInstance.h"
#include "CBTTerrain/CBTKernelSet.h"
#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrain/CBTSphereRoots.h"
#include "CBTTestHarness.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Core/CommandList.h"

using namespace GameEngine::CBTTerrain;
namespace DD = GameEngine::CBTTerrain::DeepDecode;

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthRadius = 6.371e6; // metres — the arc's product target
constexpr double kWalkingBarMeters = 0.5; // the product bar ("at least 0.5 m detail")

// Same helper as CBTSphereDomainTests: a valid sphere HeapID for root `rootIndex` at
// numSubdiv = subdiv with the given LEB path bits.
uint64_t MakeSphereHeapID(uint32_t rootIndex, uint64_t pathBits, uint32_t subdiv)
{
    const uint64_t rootHeap = (uint64_t(1) << kSphereBaseDepth) + rootIndex;
    const uint64_t mask = (subdiv >= 64u) ? ~uint64_t(0) : ((uint64_t(1) << subdiv) - 1u);
    return (rootHeap << subdiv) | (pathBits & mask);
}

// The engine's own facet-floor model (TerrainProvisioning DepthFromRatio inverse):
// the finest split edge a cube-sphere cap allows = (pi*R/2) * 2^(-subdiv/2).
double CapFloorMeters(double radius, uint32_t subdiv)
{
    return (kPi * radius * 0.5) * std::exp2(-0.5 * static_cast<double>(subdiv));
}

struct ExactTri
{
    double C[3][3];
};

ExactTri DecodeExactTri(uint64_t heapID, const std::array<CBTSphereRoot, kSphereRootCount>& roots,
                        double radius)
{
    const DD::Bary bary = DD::WalkBary(heapID, kSphereBaseDepth);
    uint32_t depth = 0u;
    for (uint64_t h = heapID; h > 1u; h >>= 1)
        ++depth;
    const uint32_t rootIndex =
        static_cast<uint32_t>(heapID >> (depth - kSphereBaseDepth)) - (1u << kSphereBaseDepth);
    ExactTri t{};
    for (int corner = 0; corner < 3; ++corner)
    {
        const DD::CubeNum num = DD::CornerNumerator(bary, corner, roots[rootIndex]);
        DD::DecodeExact(num, radius, 0.0, t.C[corner]);
    }
    return t;
}

double Dist3(const double a[3], const double b[3])
{
    const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Simulated gVertex store: cast each world component to fp32 (exactly what
// Kernel_VertexEval's `vd.corner = vec4(dir * (radius + relief), ...)` persists).
double StorageErrorMeters(const double w[3])
{
    const double fx = static_cast<double>(static_cast<float>(w[0]));
    const double fy = static_cast<double>(static_cast<float>(w[1]));
    const double fz = static_cast<double>(static_cast<float>(w[2]));
    const double d[3] = {fx, fy, fz};
    return Dist3(w, d);
}

// Deep-path sample set: three roots x three LEB bit patterns, all face-interior-ish.
// Enough spread to make the max-over-samples quantization measurement robust without
// cherry-picking a lucky (exactly-representable) corner.
constexpr uint32_t kSampleRoots[3] = {0u, 9u, 17u};
constexpr uint64_t kSamplePaths[3] = {0xAAAAAAAAAAAAAAAAull, 0x5555555555555555ull,
                                      0x6DB6DB6DB6DB6DB6ull};
} // namespace

// ---------------------------------------------------------------------------
// Probe 1 — the measured wall at Earth radius (the arc's fails-before)
// ---------------------------------------------------------------------------
TEST(CBTDeepDecode, EarthWallIsFp32WorldStorageNotTheKey)
{
    const auto roots = BuildSphereRoots();

    // (a) The shipped cap floors the Earth facet at ~9.54 m — 19x the walking bar.
    const double capFloor = CapFloorMeters(kEarthRadius, kMaxDecodeSubdiv);
    EXPECT_NEAR(capFloor, 9.543, 0.02) << "Earth facet floor at the shipped cap (numSubdiv 40)";
    EXPECT_GT(capFloor, 19.0 * kWalkingBarMeters)
        << "the shipped cap must be the binding wall for the 0.5 m walking bar";

    // (b) The u64 heap key is NOT the wall: numSubdiv 50 still fits with depth
    // headroom (findMSB bound 63; base depth 5), and the int64 walk stays exact
    // (bary sums == 2^numSubdiv, every |N| <= scale).
    for (uint32_t nd : {40u, 44u, 48u, 50u, 52u})
    {
        const uint64_t h = MakeSphereHeapID(kSampleRoots[0], kSamplePaths[0], nd);
        uint32_t depth = 0u;
        for (uint64_t v = h; v > 1u; v >>= 1)
            ++depth;
        ASSERT_EQ(depth, kSphereBaseDepth + nd) << "u64 key holds the deep path";
        const DD::Bary b = DD::WalkBary(h, kSphereBaseDepth);
        for (const int64_t* bc : {b.B0, b.B1, b.B2})
        {
            ASSERT_EQ(bc[0] + bc[1] + bc[2], b.Scale) << "exact walk invariant at nd=" << nd;
        }
        for (int corner = 0; corner < 3; ++corner)
        {
            const DD::CubeNum num = DD::CornerNumerator(b, corner, roots[kSampleRoots[0]]);
            for (int c = 0; c < 3; ++c)
                ASSERT_LE(std::llabs(num.N[c]), num.Scale) << "|N| <= scale (exact) at nd=" << nd;
        }
    }

    // (c) What DOES break: fp32 world storage. Measure the store quantization against
    // the true geometry across the sample set, per depth.
    std::printf("[earth-wall] R=%.4g  storage-ULP model sqrt(3)*R*2^-23 = %.3f m\n", kEarthRadius,
                std::sqrt(3.0) * kEarthRadius * std::exp2(-23.0));
    double errAt40 = 0.0, errAt50 = 0.0, shortAt50 = 1e30;
    uint32_t wallNd = 0u; // first depth where quantization exceeds 25% of the short edge
    for (uint32_t nd : {36u, 38u, 39u, 40u, 42u, 44u, 46u, 48u, 50u})
    {
        double maxErr = 0.0, minShort = 1e30, minSplit = 1e30;
        for (uint32_t root : kSampleRoots)
        {
            for (uint64_t path : kSamplePaths)
            {
                const uint64_t h = MakeSphereHeapID(root, path, nd);
                const ExactTri t = DecodeExactTri(h, roots, kEarthRadius);
                for (int corner = 0; corner < 3; ++corner)
                    maxErr = std::max(maxErr, StorageErrorMeters(t.C[corner]));
                const double e01 = Dist3(t.C[0], t.C[1]);
                const double e12 = Dist3(t.C[1], t.C[2]);
                const double e20 = Dist3(t.C[2], t.C[0]); // (c0,c2) is the LEB split edge
                minShort = std::min({minShort, e01, e12, e20});
                minSplit = std::min(minSplit, e20);
            }
        }
        const double ratio = maxErr / minShort;
        std::printf("[earth-wall] nd=%2u  splitEdge=%10.4f m  shortEdge=%10.4f m  "
                    "fp32StoreErr(max)=%.4f m  err/short=%6.1f%%\n",
                    nd, minSplit, minShort, maxErr, 100.0 * ratio);
        if (wallNd == 0u && ratio > 0.25)
            wallNd = nd;
        if (nd == 40u)
            errAt40 = maxErr;
        if (nd == 50u)
        {
            errAt50 = maxErr;
            shortAt50 = minShort;
        }
    }

    // The quantization is REAL and depth-independent (storage-bound, not walk-bound):
    // ~a half-ULP of the Earth-magnitude fp32 octave, at every depth.
    EXPECT_GT(errAt40, 0.1) << "fp32 world storage must show Earth-magnitude quantization";
    EXPECT_LT(errAt40, 0.9) << "storage error above the sqrt(3)*ULP bound — measurement bug";
    EXPECT_LT(std::abs(errAt50 - errAt40), 0.5)
        << "storage error must be ~depth-independent (it is a function of R, not of nd)";

    // At the walking-bar depth (nd 50, facet floor 0.298 m) the storage noise EXCEEDS
    // the facet's short edge: fp32 world storage cannot represent walking-bar facets at
    // Earth radius. This is the wall the cure must remove — and it is measured, not
    // assumed: the walk above stayed exact to 52.
    EXPECT_GT(errAt50, shortAt50)
        << "expected fp32-storage degeneracy at the walking-bar depth (Earth radius)";
    EXPECT_GE(wallNd, 41u) << "the shipped cap 40 must sit BELOW the measured degeneracy onset";
    EXPECT_LE(wallNd, 48u) << "degeneracy onset must arrive before the walking-bar depth band ends";
    std::printf("[earth-wall] measured wall: err/short > 25%% from nd=%u; cap floor %.3f m; "
                "walking bar %.2f m needs nd>=49 -> unreachable under fp32 world storage\n",
                wallNd, capFloor, kWalkingBarMeters);

    // (d) #524 cannot mask it: the origin subtraction happens at CONSUMPTION, after the
    // fp32 store — subtracting the (exact) origin from the stored corner leaves exactly
    // the storage error. Shown on one sample for the record.
    {
        const uint64_t h = MakeSphereHeapID(kSampleRoots[0], kSamplePaths[0], 44u);
        const ExactTri t = DecodeExactTri(h, roots, kEarthRadius);
        const double origin[3] = {std::round(t.C[0][0] / 1024.0) * 1024.0,
                                  std::round(t.C[0][1] / 1024.0) * 1024.0,
                                  std::round(t.C[0][2] / 1024.0) * 1024.0};
        const double storedRel[3] = {static_cast<float>(t.C[0][0]) - origin[0],
                                     static_cast<float>(t.C[0][1]) - origin[1],
                                     static_cast<float>(t.C[0][2]) - origin[2]};
        const double exactRel[3] = {t.C[0][0] - origin[0], t.C[0][1] - origin[1],
                                    t.C[0][2] - origin[2]};
        EXPECT_NEAR(Dist3(storedRel, exactRel), StorageErrorMeters(t.C[0]), 1e-9)
            << "origin subtraction after the fp32 store preserves the storage error verbatim";
    }
}

// ---------------------------------------------------------------------------
// Probe 2 — the gated cure's representation moves the wall (in-representation)
// ---------------------------------------------------------------------------
TEST(CBTDeepDecode, DeepRepresentationMovesTheWall)
{
    const auto roots = BuildSphereRoots();

    // The deep cap lands the Earth facet floor under the walking bar.
    const double deepFloor = CapFloorMeters(kEarthRadius, DD::kDeepDecodeSubdiv);
    EXPECT_NEAR(deepFloor, 0.298, 0.005) << "Earth facet floor at the deep cap (numSubdiv 50)";
    EXPECT_LE(deepFloor, kWalkingBarMeters) << "deep cap must clear the 0.5 m walking bar";

    const float radiusF = static_cast<float>(kEarthRadius);
    double worstErr = 0.0, worstFp32 = 0.0, shortAtDeep = 1e30;
    for (uint32_t nd : {40u, 44u, 48u, 50u})
    {
        double maxErr = 0.0, maxFp32 = 0.0, minShort = 1e30;
        for (uint32_t root : kSampleRoots)
        {
            for (uint64_t path : kSamplePaths)
            {
                const uint64_t h = MakeSphereHeapID(root, path, nd);
                const DD::Bary bary = DD::WalkBary(h, kSphereBaseDepth);
                const ExactTri t = DecodeExactTri(h, roots, kEarthRadius);
                minShort = std::min({minShort, Dist3(t.C[0], t.C[1]), Dist3(t.C[1], t.C[2]),
                                     Dist3(t.C[2], t.C[0])});
                // Render origin at the triangle's own neighborhood — the walking case.
                const int32_t originSector[3] = {
                    static_cast<int32_t>(std::lround(t.C[0][0] / 1024.0)),
                    static_cast<int32_t>(std::lround(t.C[0][1] / 1024.0)),
                    static_cast<int32_t>(std::lround(t.C[0][2] / 1024.0))};
                for (int corner = 0; corner < 3; ++corner)
                {
                    const DD::CubeNum num = DD::CornerNumerator(bary, corner, roots[root]);
                    const DD::DeepVertex dv = DD::DecodeCorner(num, radiusF, 0.0f);
                    float rel[3];
                    DD::ReconstructRel(dv, originSector, rel);
                    const double exactRel[3] = {
                        t.C[corner][0] - 1024.0 * originSector[0],
                        t.C[corner][1] - 1024.0 * originSector[1],
                        t.C[corner][2] - 1024.0 * originSector[2]};
                    const double relD[3] = {rel[0], rel[1], rel[2]};
                    maxErr = std::max(maxErr, Dist3(relD, exactRel));
                    maxFp32 = std::max(maxFp32, StorageErrorMeters(t.C[corner]));
                }
            }
        }
        std::printf("[deep-decode] nd=%2u  shortEdge=%10.4f m  deepErr(max)=%.3g m  "
                    "fp32WorldErr(max)=%.4f m  improvement=%.0fx\n",
                    nd, minShort, maxErr, maxFp32, maxFp32 / std::max(maxErr, 1e-30));
        worstErr = std::max(worstErr, maxErr);
        worstFp32 = std::max(worstFp32, maxFp32);
        if (nd == DD::kDeepDecodeSubdiv)
            shortAtDeep = minShort;
    }

    // The representation bound: sector-local fp32 near the origin. Expect ~1e-4 m
    // (local ULP 6.1e-5 + reconstruction rounding); assert with margin.
    EXPECT_LT(worstErr, 5e-4) << "deep (sector, local) decode error at Earth radius";
    // The wall moves: error orders of magnitude under both the fp32-world noise and the
    // deep-cap facet size (the falsifiable improvement, not a vibe).
    EXPECT_GT(worstFp32 / worstErr, 100.0) << "deep decode must beat fp32 world storage >=100x";
    EXPECT_LT(worstErr, 0.01 * shortAtDeep)
        << "deep decode error must stay under 1% of the walking-bar facet";
}

// ---------------------------------------------------------------------------
// Probe 3 — shared-corner determinism (the crack-freedom contract for the S2 port)
// ---------------------------------------------------------------------------
TEST(CBTDeepDecode, SharedCornerDeterminism)
{
    const auto roots = BuildSphereRoots();
    const float radiusF = static_cast<float>(kEarthRadius);

    auto expectBitIdentical = [&](const DD::CubeNum& a, const DD::CubeNum& b, const char* what) {
        const DD::DeepVertex va = DD::DecodeCorner(a, radiusF, 0.0f);
        const DD::DeepVertex vb = DD::DecodeCorner(b, radiusF, 0.0f);
        EXPECT_EQ(0, std::memcmp(va.Sector, vb.Sector, sizeof(va.Sector))) << what << " (sector)";
        EXPECT_EQ(0, std::memcmp(va.Local, vb.Local, sizeof(va.Local))) << what << " (local)";
    };

    for (uint32_t nd : {44u, 48u, 50u, 52u})
    {
        // (a) Siblings share the split-edge midpoint (corner 1 of both children) and the
        // parent apex: identical (N, Scale) must decode bit-identically.
        const uint64_t parent = MakeSphereHeapID(kSampleRoots[1], kSamplePaths[2], nd - 1u);
        const DD::Bary c0 = DD::WalkBary(parent << 1, kSphereBaseDepth);
        const DD::Bary c1 = DD::WalkBary((parent << 1) | 1u, kSphereBaseDepth);
        const DD::CubeNum mid0 = DD::CornerNumerator(c0, 1, roots[kSampleRoots[1]]);
        const DD::CubeNum mid1 = DD::CornerNumerator(c1, 1, roots[kSampleRoots[1]]);
        ASSERT_EQ(0, std::memcmp(mid0.N, mid1.N, sizeof(mid0.N)))
            << "sibling midpoint numerators must be exactly equal (nd=" << nd << ")";
        expectBitIdentical(mid0, mid1, "sibling shared midpoint");

        // (b) Across one depth level (conforming neighbors differ by <= 1 level): the
        // parent's corner 1 reappears as child 0's corner 2 with (2N, 2*Scale). The df64
        // decode must be exactly invariant under the power-of-two rescale.
        const DD::Bary pb = DD::WalkBary(parent, kSphereBaseDepth);
        const DD::CubeNum parentApex = DD::CornerNumerator(pb, 1, roots[kSampleRoots[1]]);
        const DD::CubeNum childSame = DD::CornerNumerator(c0, 2, roots[kSampleRoots[1]]);
        ASSERT_EQ(childSame.Scale, parentApex.Scale * 2) << "child scale doubles";
        for (int c = 0; c < 3; ++c)
            ASSERT_EQ(childSame.N[c], parentApex.N[c] * 2)
                << "child sees the parent corner as exactly 2N";
        expectBitIdentical(parentApex, childSame, "cross-depth shared corner");

        // (c) Across a cube-face seam: a root's split-edge corner V0 is its twin root's
        // V2 (CBTSphereRoots twin wiring). Decode the shared cube-edge corner from both
        // faces at DIFFERENT depths: N = Scale * V0 vs N' = Scale' * V0, a power-of-two
        // multiple pair — must decode bit-identically or the seam cracks.
        const uint32_t r = kSampleRoots[0];
        const uint32_t twin = roots[r].Neighbors.Twin;
        ASSERT_NE(twin, kInvalidPointer);
        ASSERT_EQ(roots[r].V0, roots[twin].V2) << "twin split-edge orientation (roots contract)";
        const DD::Bary sideA = DD::WalkBary(MakeSphereHeapID(r, 0u, nd), kSphereBaseDepth);
        const DD::Bary sideB = DD::WalkBary(MakeSphereHeapID(twin, 0u, nd - 2u), kSphereBaseDepth);
        // Path bits 0 keep corner0/corner2 barycentrics on the root frame's split edge
        // only in aggregate; use the root-corner barycentric directly instead: corner 0
        // of the UNDIVIDED root frame is (Scale, 0, 0) -> N = Scale * V0 by construction.
        DD::CubeNum edgeA{}, edgeB{};
        edgeA.Scale = sideA.Scale;
        edgeB.Scale = sideB.Scale;
        for (int c = 0; c < 3; ++c)
        {
            edgeA.N[c] = sideA.Scale * roots[r].V0[c];
            edgeB.N[c] = sideB.Scale * roots[twin].V2[c];
        }
        expectBitIdentical(edgeA, edgeB, "cross-face shared cube-edge corner");
    }
}

// ---------------------------------------------------------------------------
// Probe 4 — df64 arithmetic bounds (falsifies contraction / fast-math breakage)
// ---------------------------------------------------------------------------
TEST(CBTDeepDecode, Df64ArithmeticHoldsExtendedPrecision)
{
    // Deterministic LCG over the deep numerator range; compare df64 against double.
    uint64_t state = 0x9E3779B97F4A7C15ull;
    auto next = [&state]() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return state;
    };
    double worstMulRel = 0.0, worstRsqrtRel = 0.0;
    for (int i = 0; i < 4096; ++i)
    {
        const int64_t a = static_cast<int64_t>(next() >> 14); // ~2^50 range
        const int64_t b = static_cast<int64_t>(next() >> 14);
        const DD::DF da = DD::FromI64(a);
        const DD::DF db = DD::FromI64(b);
        // FromI64 representation error is bounded (~2^-47 relative), never fp32-coarse.
        const double aRep = static_cast<double>(da.Hi) + static_cast<double>(da.Lo);
        ASSERT_LT(std::abs(aRep - static_cast<double>(a)),
                  std::max(1.0, std::abs(static_cast<double>(a))) * 1e-13);

        const DD::DF m = DD::Mul(da, db);
        const double mDf = static_cast<double>(m.Hi) + static_cast<double>(m.Lo);
        const double mRef = aRep * (static_cast<double>(db.Hi) + static_cast<double>(db.Lo));
        if (mRef != 0.0)
            worstMulRel = std::max(worstMulRel, std::abs(mDf - mRef) / std::abs(mRef));

        const double sRef = std::abs(mRef) + 1.0;
        const DD::DF s = DD::Add(DD::DF{m.Hi < 0 ? -m.Hi : m.Hi, m.Hi < 0 ? -m.Lo : m.Lo},
                                 DD::DF{1.0f, 0.0f});
        const DD::DF r = DD::RSqrt(s);
        const double rDf = static_cast<double>(r.Hi) + static_cast<double>(r.Lo);
        const double rRef = 1.0 / std::sqrt(static_cast<double>(s.Hi) + static_cast<double>(s.Lo));
        (void)sRef;
        worstRsqrtRel = std::max(worstRsqrtRel, std::abs(rDf - rRef) / rRef);
    }
    std::printf("[df64] worst mul rel=%.3g  worst rsqrt rel=%.3g (fp32 alone would be ~6e-8)\n",
                worstMulRel, worstRsqrtRel);
    // df64 must be far past fp32's 2^-24 (~6e-8). A broken error-free transform (FP
    // contraction, fast-math) collapses these to ~1e-7 and fails loudly here.
    EXPECT_LT(worstMulRel, 1e-12) << "df64 multiply lost extended precision";
    EXPECT_LT(worstRsqrtRel, 1e-11) << "df64 rsqrt lost extended precision";
}

// ===========================================================================
// S2a GPU gates — the (sector, local) storage LIVE in the pipeline.
// Real-device probes: GPU<->CPU bit-exactness of the df64 decode, the dark-ship
// tail-zero oracle, flag-ON parity/determinism at R = 50 km, and the quiescent
// CBT.Update cost A/B. (The CPU probes above stay device-free.)
// ===========================================================================

namespace
{
using namespace GameEngine::CBTTerrain::Test;
using namespace GameEngine::Rendering;

constexpr uint32_t kVW = sizeof(CBTVertexData) / 4u; // 24 words/slot (S2a 96B layout)

CBTFrameParams SphereOnlyParams(float radius)
{
    CBTFrameParams p{};
    p.PlanetParams[0] = radius;
    p.PlanetParams[1] = 0.0f; // relief amp 0: CBT_PlanetRelief returns exactly 0 on BOTH sides,
    p.PlanetParams[2] = 5.0f; // so the decode inputs (N, radius, relief=0) are bit-shared CPU<->GPU
    p.PlanetParams[3] = 4.0f;
    return p;
}

// The CPU-library expectation for one corner of one deep key (the bit authority).
DD::DeepVertex CpuDeepCorner(const std::array<CBTSphereRoot, kSphereRootCount>& roots, uint64_t h,
                             int corner, float radiusF)
{
    const DD::Bary bary = DD::WalkBary(h, kSphereBaseDepth);
    uint32_t depth = 0u;
    for (uint64_t v = h; v > 1u; v >>= 1)
        ++depth;
    const uint32_t rootIndex =
        static_cast<uint32_t>(h >> (depth - kSphereBaseDepth)) - (1u << kSphereBaseDepth);
    const DD::CubeNum num = DD::CornerNumerator(bary, corner, roots[rootIndex]);
    return DD::DecodeCorner(num, radiusF, 0.0f);
}

// Compare one readback slot against the CPU library, bit-for-bit. Returns mismatch count
// (0 == bit-exact) and EXPECTs with diagnostics.
uint32_t ExpectSlotMatchesCpu(const std::vector<uint32_t>& verts, uint32_t slot, uint64_t h,
                              const std::array<CBTSphereRoot, kSphereRootCount>& roots,
                              float radiusF, const char* what)
{
    uint32_t mismatches = 0;
    const uint32_t base = slot * kVW;
    EXPECT_EQ(verts[base + 22u], 1u) << what << " slot " << slot << ": deepTag.x != 1";
    for (int corner = 0; corner < 3; ++corner)
    {
        const DD::DeepVertex dv = CpuDeepCorner(roots, h, corner, radiusF);
        const uint32_t sw0 = PackSectorPair(dv.Sector[0], dv.Sector[1]);
        const uint32_t sw1 = PackSectorPair(dv.Sector[2], 0);
        const uint32_t g0 = verts[base + 16u + static_cast<uint32_t>(corner) * 2u];
        const uint32_t g1 = verts[base + 17u + static_cast<uint32_t>(corner) * 2u];
        if (g0 != sw0 || g1 != sw1)
        {
            ++mismatches;
            ADD_FAILURE() << what << " slot " << slot << " corner " << corner
                          << ": sector words GPU (" << g0 << ", " << g1 << ") != CPU (" << sw0
                          << ", " << sw1 << ")";
        }
        for (int c = 0; c < 3; ++c)
        {
            const uint32_t cpuBits = std::bit_cast<uint32_t>(dv.Local[c]);
            const uint32_t gpuBits = verts[base + static_cast<uint32_t>(corner) * 4u +
                                           static_cast<uint32_t>(c)];
            if (cpuBits != gpuBits)
            {
                ++mismatches;
                float gf;
                std::memcpy(&gf, &gpuBits, sizeof(gf));
                ADD_FAILURE() << what << " slot " << slot << " corner " << corner << " local[" << c
                              << "]: GPU " << gf << " (0x" << std::hex << gpuBits << ") != CPU "
                              << dv.Local[c] << " (0x" << cpuBits << ")" << std::dec;
            }
        }
        // Representation sanity: |local| <= 512 + a tie half-sector, sector within the fp32
        // sector*1024 exactness bound (2^24 m -> +/-16384) — the honest 16-bit-lane range check.
        for (int c = 0; c < 3; ++c)
        {
            EXPECT_LE(std::abs(dv.Local[c]), 512.5f);
            EXPECT_LE(std::abs(dv.Sector[c]), 16384);
        }
    }
    return mismatches;
}
} // namespace

class CBTDeepDecodeGpu : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = MakeHeadlessDevice();
        if (!m_Device)
            GTEST_SKIP() << "no headless Vulkan device";
        if (!m_Device->GetCapabilities().supportsShaderInt64)
            GTEST_SKIP() << "device lacks shaderInt64";
        if (!m_KernelSet.Initialize(*m_Device, ShaderOutputDir()))
            GTEST_SKIP() << "cbt_kernels.comp.spv missing (glslc unavailable at build)";
    }
    void TearDown() override
    {
        m_KernelSet.Shutdown();
        if (m_Device)
            m_Device->Shutdown();
    }

    void RunFrame(CBTInstance& instance, const CBTClassifyDesc& desc, const CBTFrameParams& params,
                  uint32_t frame)
    {
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        instance.RecordUpdate(*cl, desc, params, frame);
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();
    }

    // Seed `keys` as live bisectors at physical slots firstSlot.. (a transfer write plus an
    // explicit CopyDest->UAV settle, then a wait). Seeded slots
    // carry INVALID neighbors (Validate skips them as domain boundaries), UNCHANGED state (the
    // first update's Classify cannot visit them anyway: its indirect dispatch covers only the
    // prior sum-tree live count — the roots), and MODIFIED|VISIBLE flags; the same update's tail
    // reduce folds their occupancy bits in, BisectorIndexation compacts them into IndicesAll, and
    // VertexEval then decodes them — which is the whole point of the seeding.
    void SeedDeepKeys(CBTInstance& instance, const std::vector<uint64_t>& keys, uint32_t firstSlot)
    {
        // Synthetic host writes must follow the asynchronous initialization upload.
        ASSERT_TRUE(m_Device->WaitGpuSyncToken(m_Device->LastGraphicsSubmissionToken()));
        ASSERT_EQ(firstSlot % 32u, 0u) << "seed base must be bitfield-word aligned";
        auto& res = instance.GetResources();
        m_Device->UpdateBuffer(res.GetBuffer(CBTBinding::HeapID), firstSlot * sizeof(uint64_t),
                               keys.size() * sizeof(uint64_t), keys.data());
        std::vector<CBTNeighbors> nbr(keys.size()); // defaults: kInvalidPointer links
        m_Device->UpdateBuffer(res.GetBuffer(CBTBinding::NeighborsA),
                               firstSlot * sizeof(CBTNeighbors), nbr.size() * sizeof(CBTNeighbors),
                               nbr.data());
        m_Device->UpdateBuffer(res.GetBuffer(CBTBinding::NeighborsB),
                               firstSlot * sizeof(CBTNeighbors), nbr.size() * sizeof(CBTNeighbors),
                               nbr.data());
        std::vector<CBTBisectorData> bd(keys.size());
        for (CBTBisectorData& b : bd)
            b.Flags = kFlagVisible | kFlagModified;
        m_Device->UpdateBuffer(res.GetBuffer(CBTBinding::BisectorData),
                               firstSlot * sizeof(CBTBisectorData),
                               bd.size() * sizeof(CBTBisectorData), bd.data());
        const uint32_t firstWord = firstSlot / 32u;
        const uint32_t wordCount = (static_cast<uint32_t>(keys.size()) + 31u) / 32u;
        std::vector<uint32_t> words(wordCount, 0u);
        for (uint32_t i = 0; i < keys.size(); ++i)
            words[i / 32u] |= 1u << (i % 32u);
        m_Device->UpdateBuffer(res.GetBuffer(CBTBinding::Bitfield), firstWord * 4u, wordCount * 4u,
                               words.data());

        // Settle every seeded buffer for the next update's shader reads (the InitializeRoots
        // Bitfield-barrier discipline, applied to all five).
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        for (CBTBinding b : {CBTBinding::HeapID, CBTBinding::NeighborsA, CBTBinding::NeighborsB,
                             CBTBinding::BisectorData, CBTBinding::Bitfield})
            cl->Barrier(ResourceBarrier::CreateBufferBarrier(res.GetBuffer(b),
                                                             ResourceState::CopyDest,
                                                             ResourceState::UnorderedAccess));
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();
    }

    std::unique_ptr<IDevice> m_Device;
    CBTKernelSet m_KernelSet;
};

// GATE 1 — GPU<->CPU bit-exactness. Seed deep keys at nd 38..50 (including a split-edge
// sibling pair), run ONE production update with DeepDecode on at Earth radius, read gVertex
// back and require the stored (sector, local) to equal CBTDeepDecode.h bit-for-bit — the
// lockstep contract, held on real hardware, at depths past what pool-bounded refinement can
// materialise. Also pins the OFF-mode guard: at nd 44 with the flag off, VertexEval must
// refuse the decode (the output record stays untouched).
TEST_F(CBTDeepDecodeGpu, GpuDecodeMatchesCpuLibraryBitExact)
{
    const auto roots = BuildSphereRoots();
    const float radiusF = static_cast<float>(kEarthRadius);
    const CBTFrameParams p = SphereOnlyParams(radiusF);
    constexpr uint32_t kSeedBase = 64u;

    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));

    uint32_t frame = 0;
    uint32_t totalCorners = 0;
    for (const uint32_t nd : {38u, 40u, 44u, 48u, 50u})
    {
        ASSERT_TRUE(instance.InitializeRoots(kDomainSpherical)); // fresh tree per depth batch
        std::vector<uint64_t> keys;
        for (const uint32_t root : {kSampleRoots[0], kSampleRoots[1]})
            for (const uint64_t path : {kSamplePaths[0], kSamplePaths[2]})
                keys.push_back(MakeSphereHeapID(root, path, nd));
        // A split-edge sibling pair: both children share the parent's midpoint corner — the
        // GPU must store bit-identical (sector, local) for it (crack-freedom, on-device).
        const uint64_t parent = MakeSphereHeapID(kSampleRoots[1], kSamplePaths[2], nd - 1u);
        keys.push_back(parent << 1);
        keys.push_back((parent << 1) | 1u);
        SeedDeepKeys(instance, keys, kSeedBase);

        CBTClassifyDesc classify{}; // depth-target at base depth: roots inert; seeds unvisited
        classify.Mode = kClassifyDepthTarget;
        classify.TargetDepth = kSphereBaseDepth;
        classify.DeepDecode = 1u;
        RunFrame(instance, classify, p, frame++);

        const uint32_t maxSlot = kSeedBase + static_cast<uint32_t>(keys.size());
        const auto heap = instance.DebugReadWords(CBTBinding::HeapID, maxSlot * 2u);
        const auto verts = instance.DebugReadWords(CBTBinding::CurrentVertex, maxSlot * kVW);
        for (uint32_t i = 0; i < keys.size(); ++i)
        {
            const uint32_t slot = kSeedBase + i;
            const uint64_t h = static_cast<uint64_t>(heap[slot * 2u]) |
                               (static_cast<uint64_t>(heap[slot * 2u + 1u]) << 32);
            ASSERT_EQ(h, keys[i]) << "seeded key was disturbed by the update (nd=" << nd << ")";
            char what[64];
            std::snprintf(what, sizeof(what), "nd=%u key[%u]", nd, i);
            EXPECT_EQ(ExpectSlotMatchesCpu(verts, slot, h, roots, radiusF, what), 0u);
            totalCorners += 3;
        }
        // The sibling pair's shared midpoint (corner 1 of both children), on-device.
        const uint32_t s0 = kSeedBase + static_cast<uint32_t>(keys.size()) - 2u;
        const uint32_t s1 = s0 + 1u;
        for (uint32_t wordIdx = 0; wordIdx < 4u; ++wordIdx) // corner1 vec4 words 4..7... compare xyz+sector
        {
            if (wordIdx < 3u)
                EXPECT_EQ(verts[s0 * kVW + 4u + wordIdx], verts[s1 * kVW + 4u + wordIdx])
                    << "sibling shared-midpoint local[" << wordIdx << "] differs (nd=" << nd << ")";
        }
        EXPECT_EQ(verts[s0 * kVW + 18u], verts[s1 * kVW + 18u]) << "sibling midpoint sector word 0";
        EXPECT_EQ(verts[s0 * kVW + 19u], verts[s1 * kVW + 19u]) << "sibling midpoint sector word 1";
    }
    std::printf("[deep-gpu] GPU==CPU bit-exact over %u corners at nd 38..50 (Earth radius)\n",
                totalCorners);

    // OFF-mode guard: nd 44 with the flag off must NOT decode (legacy cap 40 intact).
    ASSERT_TRUE(instance.InitializeRoots(kDomainSpherical));
    const std::vector<uint64_t> offKeys = {MakeSphereHeapID(kSampleRoots[0], kSamplePaths[0], 44u)};
    SeedDeepKeys(instance, offKeys, kSeedBase);
    // Sparse root initialization deliberately retains dead payload records. The guard's contract
    // is to skip this invalid decode, so compare its actual incoming bytes rather than assuming
    // the allocator or a whole-pool clear supplied zeros.
    const auto beforeRejectedDecode =
        instance.DebugReadWords(CBTBinding::CurrentVertex, kVW, kSeedBase * kVW);
    ASSERT_EQ(beforeRejectedDecode.size(), kVW);
    CBTClassifyDesc offClassify{};
    offClassify.Mode = kClassifyDepthTarget;
    offClassify.TargetDepth = kSphereBaseDepth;
    offClassify.DeepDecode = 0u;
    RunFrame(instance, offClassify, p, frame++);
    const auto offVerts =
        instance.DebugReadWords(CBTBinding::CurrentVertex, (kSeedBase + 1u) * kVW);
    for (uint32_t w = 0; w < kVW; ++w)
        EXPECT_EQ(offVerts[kSeedBase * kVW + w], beforeRejectedDecode[w])
            << "flag-off VertexEval decoded past kMaxDecodeSubdiv (word " << w << ")";
}

// GATE 2 (dark-ship, storage-level) — with the flag OFF, a refined spherical pool's gVertex
// must be byte-indistinguishable from the pre-S2a layout: every live slot's sector tail
// zero, deepTag zero, corners the legacy fp32 world store. (The suite-at-baseline is the
// behavioral half of the gate; this is the byte-level half. Also prints the VRAM delta —
// the honest cost of the 96B layout, paid in both modes.)
TEST_F(CBTDeepDecodeGpu, DarkShipOffLeavesSectorTailZero)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots(kDomainSpherical));

    const uint64_t vertexBytes = instance.GetResources().GetBufferByteSize(CBTBinding::CurrentVertex);
    std::printf("[deep-vram] gVertex %llu MiB at the %u pool (was %u MiB at 64B -> +%u MiB)\n",
                static_cast<unsigned long long>(vertexBytes >> 20), kDefaultBisectorPoolSize,
                (kDefaultBisectorPoolSize * 64u) >> 20,
                static_cast<uint32_t>((vertexBytes - kDefaultBisectorPoolSize * 64ull) >> 20));
    EXPECT_EQ(vertexBytes, static_cast<uint64_t>(kDefaultBisectorPoolSize) * sizeof(CBTVertexData));

    CBTClassifyDesc classify{};
    classify.Mode = kClassifyDepthTarget;
    classify.TargetDepth = kSphereBaseDepth + 4u; // uniform depth-9 refinement: 384 live
    classify.DeepDecode = 0u;
    const CBTFrameParams p = SphereOnlyParams(2000.0f);
    for (uint32_t f = 0; f < 8u; ++f)
        RunFrame(instance, classify, p, f);

    constexpr uint32_t kSlots = 4096u;
    const auto heap = instance.DebugReadWords(CBTBinding::HeapID, kSlots * 2u);
    const auto verts = instance.DebugReadWords(CBTBinding::CurrentVertex, kSlots * kVW);
    uint32_t live = 0, tainted = 0;
    for (uint32_t slot = 0; slot < kSlots; ++slot)
    {
        if (heap[slot * 2u] == 0u && heap[slot * 2u + 1u] == 0u)
            continue;
        ++live;
        for (uint32_t w = 16u; w < kVW; ++w) // the S2a tail: sector0..2 + deepTag
            if (verts[slot * kVW + w] != 0u)
                ++tainted;
    }
    ASSERT_GT(live, 64u) << "refinement did not converge";
    EXPECT_EQ(tainted, 0u) << "flag-off VertexEval wrote a non-zero sector tail (dark-ship broken)";
}

// GATE 3 — flag ON at today's scale (R = 50 km): a full descent to a low pose with the deep
// store live must (a) keep the Validate invariants at zero, (b) track the OFF run's
// tessellation within the df64-vs-fp32-rounding tolerance (quantified below — at 5e4 m the
// legacy store's ULP is ~3.9e-3 m, so the metric inputs differ by millimetres and the
// converged topology should differ by at most run-to-run jitter), (c) hold FULL-POOL
// GPU<->CPU bit-parity — every live bisector's stored (sector, local) recomputable from its
// HeapID through CBTDeepDecode.h, which subsumes shared-corner crack-freedom on device —
// and (d) show no storage-level swim: a 1 mm camera micro-step with a forced full re-eval
// must reproduce every unchanged-topology slot byte-for-byte (#524 methodology at the
// storage level; the decode is camera-independent by construction and this pins it).
TEST_F(CBTDeepDecodeGpu, FlagOnR50kParityConformityAndNoSwim)
{
    using namespace GameEngine::Mathematics;
    const float radius = 50000.0f;
    const float holdAlt = 150.0f;
    const auto roots = BuildSphereRoots();
    const uint32_t cap = kSphereBaseDepth + kMaxDecodeSubdiv;
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(1.2f, 16.0f / 9.0f, 0.05f,
                                                         3.0f * radius + 4000.0f);

    struct RunResult
    {
        uint32_t Live = 0;
        double MedEdge = 0.0;
        double MinEdge = 0.0;
        uint32_t Validation = 0;
    };

    CBTFrameParams p = SphereOnlyParams(radius);
    p.Screen[0] = 1600.0f;
    p.Screen[1] = 900.0f;
    p.Screen[2] = 8.0f;
    p.Screen[3] = 4.0f;
    p.TerrainOrigin[2] = static_cast<float>(cap);

    auto poseAt = [&](CBTFrameParams& fp, double alt, double eyeXOffset) {
        const Vector3 eye(static_cast<float>(eyeXOffset), 0.0f,
                          -static_cast<float>(radius + alt));
        const Matrix4x4 vp =
            proj * MakeLookAtLH(eye, Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f));
        std::memcpy(fp.ViewProjRel, &vp.GetGLM()[0][0], sizeof(fp.ViewProjRel));
        fp.CameraPos[0] = eye.x;
        fp.CameraPos[1] = eye.y;
        fp.CameraPos[2] = eye.z;
        fp.CameraPos[3] = 1.0f;
    };

    auto converge = [&](CBTInstance& instance, uint32_t deep) {
        CBTClassifyDesc classify{};
        classify.Mode = kClassifyScreenSpace;
        classify.TargetDepth = cap;
        classify.DeepDecode = deep;
        const float startAlt = 2000.0f;
        uint32_t f = 0;
        for (; f < 130u; ++f)
        {
            poseAt(p, std::max(static_cast<double>(holdAlt),
                               startAlt * std::pow(0.96, static_cast<double>(f))), 0.0);
            RunFrame(instance, classify, p, f);
        }
        poseAt(p, holdAlt, 0.0);
        for (uint32_t s = 0; s < 30u; ++s)
            RunFrame(instance, classify, p, f + s);
        return classify;
    };

    // Edge stats from the readback, mode-aware (deep slots reconstruct world = sector*1024+local).
    auto scan = [&](CBTInstance& instance, RunResult& out) {
        const auto heap =
            instance.DebugReadWords(CBTBinding::HeapID, kDefaultBisectorPoolSize * 2u);
        const auto verts = instance.DebugReadWords(CBTBinding::CurrentVertex,
                                                         kDefaultBisectorPoolSize * kVW);
        std::vector<double> edges;
        edges.reserve(1 << 18);
        auto cornerWorld = [&](uint32_t base, uint32_t k, double outW[3]) {
            float l[3];
            std::memcpy(l, &verts[base + k * 4u], sizeof(l));
            if (verts[base + 22u] != 0u)
            {
                const uint32_t w0 = verts[base + 16u + k * 2u];
                const uint32_t w1 = verts[base + 17u + k * 2u];
                outW[0] = 1024.0 * UnpackSectorLo(w0) + l[0];
                outW[1] = 1024.0 * UnpackSectorHi(w0) + l[1];
                outW[2] = 1024.0 * UnpackSectorLo(w1) + l[2];
            }
            else
            {
                outW[0] = l[0];
                outW[1] = l[1];
                outW[2] = l[2];
            }
        };
        for (uint32_t slot = 0; slot < kDefaultBisectorPoolSize; ++slot)
        {
            if (heap[slot * 2u] == 0u && heap[slot * 2u + 1u] == 0u)
                continue;
            ++out.Live;
            double c0[3], c2[3];
            const uint32_t base = slot * kVW;
            cornerWorld(base, 0u, c0);
            cornerWorld(base, 2u, c2);
            const double len = Dist3(c0, c2);
            if (len > 0.0)
                edges.push_back(len);
        }
        std::sort(edges.begin(), edges.end());
        out.MedEdge = edges.empty() ? 0.0 : edges[edges.size() / 2u];
        out.MinEdge = edges.empty() ? 0.0 : edges.front();
        out.Validation = instance.ReadValidationErrorCount();
    };

    // OFF run (the baseline this ON run is compared against).
    RunResult off{};
    {
        CBTInstance instance;
        ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
        ASSERT_TRUE(instance.InitializeRoots(kDomainSpherical));
        converge(instance, 0u);
        scan(instance, off);
    }

    // ON run + parity + no-swim on the SAME instance.
    RunResult on{};
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots(kDomainSpherical));
    const CBTClassifyDesc classify = converge(instance, 1u);
    scan(instance, on);

    std::printf("[deep-r50k] OFF live=%u med=%.4fm min=%.4fm val=%u | ON live=%u med=%.4fm "
                "min=%.4fm val=%u | dLive=%+d dMed=%+.4fm (fp32 store ULP at 5e4 m ~ 3.9e-3 m)\n",
                off.Live, off.MedEdge, off.MinEdge, off.Validation, on.Live, on.MedEdge, on.MinEdge,
                on.Validation, static_cast<int>(on.Live) - static_cast<int>(off.Live),
                on.MedEdge - off.MedEdge);

    ASSERT_GT(on.Live, 1000u) << "descent did not refine";
    EXPECT_EQ(on.Validation, 0u) << "deep store broke a Validate invariant at R=50k";
    EXPECT_EQ(off.Validation, 0u);
    // Tolerance (quantified): mm-scale metric-input deltas can flip individual hysteresis
    // decisions, bounded by the same two-run jitter band the neutrality oracles use.
    EXPECT_NEAR(static_cast<double>(on.Live), static_cast<double>(off.Live),
                std::max(2000.0, off.Live * 0.02))
        << "flag-ON tessellation diverged from OFF beyond the rounding/jitter band";
    EXPECT_NEAR(on.MedEdge, off.MedEdge, std::max(0.05, off.MedEdge * 0.05))
        << "flag-ON median facet diverged from OFF beyond the rounding/jitter band";

    // (c) FULL-POOL GPU<->CPU bit-parity: every live deep slot's (sector, local) must equal
    // the CPU library recomputed from its HeapID (relief amp 0 -> shared inputs).
    {
        const auto heap =
            instance.DebugReadWords(CBTBinding::HeapID, kDefaultBisectorPoolSize * 2u);
        const auto verts = instance.DebugReadWords(CBTBinding::CurrentVertex,
                                                         kDefaultBisectorPoolSize * kVW);
        uint32_t checked = 0, badSlots = 0, zeroTag = 0;
        for (uint32_t slot = 0; slot < kDefaultBisectorPoolSize && badSlots < 8u; ++slot)
        {
            const uint64_t h = static_cast<uint64_t>(heap[slot * 2u]) |
                               (static_cast<uint64_t>(heap[slot * 2u + 1u]) << 32);
            if (h == 0u)
                continue;
            if (verts[slot * kVW + 22u] != 1u)
            {
                // A just-created transient VertexEval has not evaluated yet reads all-zero —
                // tolerate only the all-zero form (matches the legacy "unevaluated" contract).
                ++zeroTag;
                continue;
            }
            char what[32];
            std::snprintf(what, sizeof(what), "r50k slot");
            if (ExpectSlotMatchesCpu(verts, slot, h, roots, radius, what) != 0u)
                ++badSlots;
            ++checked;
        }
        std::printf("[deep-r50k] full-pool parity: %u live slots bit-exact vs CPU library "
                    "(%u unevaluated transients)\n", checked, zeroTag);
        EXPECT_EQ(badSlots, 0u);
        EXPECT_GT(checked, on.Live / 2u);
        EXPECT_LT(zeroTag, on.Live / 20u) << "too many unevaluated slots — settle did not drain";
    }

    // (d) No storage swim under a camera micro-step: 1 mm eye offset + forced full re-eval;
    // every slot whose HeapID is unchanged must reproduce its 96 bytes exactly.
    {
        const auto heapA =
            instance.DebugReadWords(CBTBinding::HeapID, kDefaultBisectorPoolSize * 2u);
        const auto vertsA = instance.DebugReadWords(CBTBinding::CurrentVertex,
                                                          kDefaultBisectorPoolSize * kVW);
        CBTFrameParams p2 = p;
        poseAt(p2, holdAlt, 1.0e-3);
        RunFrame(instance, classify, p2, 400u);
        const auto heapB =
            instance.DebugReadWords(CBTBinding::HeapID, kDefaultBisectorPoolSize * 2u);
        const auto vertsB = instance.DebugReadWords(CBTBinding::CurrentVertex,
                                                          kDefaultBisectorPoolSize * kVW);
        uint32_t stable = 0, swum = 0, retopo = 0;
        for (uint32_t slot = 0; slot < kDefaultBisectorPoolSize; ++slot)
        {
            const bool liveA = heapA[slot * 2u] != 0u || heapA[slot * 2u + 1u] != 0u;
            if (!liveA)
                continue;
            if (heapA[slot * 2u] != heapB[slot * 2u] || heapA[slot * 2u + 1u] != heapB[slot * 2u + 1u])
            {
                ++retopo; // the micro-step flipped a hysteresis decision — not a swim
                continue;
            }
            ++stable;
            if (std::memcmp(&vertsA[slot * kVW], &vertsB[slot * kVW], kVW * 4u) != 0)
                ++swum;
        }
        std::printf("[deep-r50k] no-swim: %u stable slots byte-identical, %u swum, %u re-topologized "
                    "by the 1 mm step\n", stable, swum, retopo);
        EXPECT_EQ(swum, 0u) << "a camera micro-step changed stored (sector, local) geometry — swim";
        EXPECT_GT(stable, on.Live * 9u / 10u) << "micro-step re-topologized too much for the oracle";
    }
}

// GATE 4 — quiescent CBT.Update cost, ON vs OFF (min-of-N, the least-contended sample on a
// noisy shared box). The df64 decode runs only inside VertexEval, which the production
// quiescence gate reduces to ~0 evaluations on a converged frame — so ON's quiescent floor
// must stay in OFF's band. The changing-frame (descent) cost is printed for the PR record.
TEST_F(CBTDeepDecodeGpu, QuiescentUpdateCostOnVsOff)
{
    using namespace GameEngine::Mathematics;
    const float radius = 50000.0f;
    const float holdAlt = 150.0f;
    const uint32_t cap = kSphereBaseDepth + kMaxDecodeSubdiv;
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(1.2f, 16.0f / 9.0f, 0.05f,
                                                         3.0f * radius + 4000.0f);

    CBTFrameParams p = SphereOnlyParams(radius);
    p.Screen[0] = 1600.0f;
    p.Screen[1] = 900.0f;
    p.Screen[2] = 8.0f;
    p.Screen[3] = 4.0f;
    p.TerrainOrigin[2] = static_cast<float>(cap);

    auto poseAt = [&](double alt) {
        const Vector3 eye(0.0f, 0.0f, -static_cast<float>(radius + alt));
        const Matrix4x4 vp =
            proj * MakeLookAtLH(eye, Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f));
        std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
        p.CameraPos[0] = eye.x;
        p.CameraPos[1] = eye.y;
        p.CameraPos[2] = eye.z;
        p.CameraPos[3] = 1.0f;
    };

    auto measure = [&](uint32_t deep, double& quiescentMin, double& changingMin) {
        CBTInstance instance;
        EXPECT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
        EXPECT_TRUE(instance.InitializeRoots(kDomainSpherical));
        instance.SetValidateEachUpdate(false); // the ship configuration (idle-floor honest)

        CBTClassifyDesc classify{};
        classify.Mode = kClassifyScreenSpace;
        classify.TargetDepth = cap;
        classify.DeepDecode = deep;

        changingMin = 1e30;
        uint32_t f = 0;
        for (; f < 130u; ++f)
        {
            poseAt(std::max(static_cast<double>(holdAlt),
                            2000.0 * std::pow(0.96, static_cast<double>(f))));
            auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
            cl->Begin();
            instance.RecordUpdate(*cl, classify, p, f);
            cl->End();
            std::vector<CommandList*> lists{cl.get()};
            const auto t0 = std::chrono::high_resolution_clock::now();
            m_Device->ExecuteCommandLists(lists);
            m_Device->WaitForIdle();
            const auto t1 = std::chrono::high_resolution_clock::now();
            if (f >= 100u)
                changingMin = std::min(changingMin,
                                       std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        poseAt(holdAlt);
        for (uint32_t s = 0; s < 30u; ++s)
        {
            auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
            cl->Begin();
            instance.RecordUpdate(*cl, classify, p, f + s);
            cl->End();
            std::vector<CommandList*> lists{cl.get()};
            m_Device->ExecuteCommandLists(lists);
            m_Device->WaitForIdle();
        }
        CBTClassifyDesc timed = classify;
        timed.GateVertexEval = 1u; // the production quiescence gate
        quiescentMin = 1e30;
        for (uint32_t s = 0; s < 40u; ++s)
        {
            auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
            cl->Begin();
            instance.RecordUpdate(*cl, timed, p, f + 40u + s);
            cl->End();
            std::vector<CommandList*> lists{cl.get()};
            const auto t0 = std::chrono::high_resolution_clock::now();
            m_Device->ExecuteCommandLists(lists);
            m_Device->WaitForIdle();
            const auto t1 = std::chrono::high_resolution_clock::now();
            quiescentMin = std::min(quiescentMin,
                                    std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
    };

    double offQ = 0, offC = 0, onQ = 0, onC = 0;
    measure(0u, offQ, offC);
    measure(1u, onQ, onC);
    std::printf("[deep-cost] R=50000 alt=%.0fm quiescentMs(min) OFF=%.3f ON=%.3f (%+.1f%%) | "
                "changingMs(min) OFF=%.3f ON=%.3f (%+.1f%%)\n",
                holdAlt, offQ, onQ, 100.0 * (onQ - offQ) / offQ, offC, onC,
                100.0 * (onC - offC) / offC);
    // Loose gate (shared noisy box — the view-priority gate-4 discipline): a real regression
    // doubles the floor; box noise does not.
    EXPECT_LT(onQ, offQ * 2.0 + 1.0)
        << "deep decode roughly doubled the quiescent CBT.Update floor (OFF=" << offQ
        << "ms ON=" << onQ << "ms)";
}

// ---------------------------------------------------------------------------
// Probe 5 — the env flag (dark-ship contract)
// ---------------------------------------------------------------------------
TEST(CBTDeepDecode, EnvFlagGatesDeepCap)
{
    using GameEngine::CBTTerrain::Test::SetEnvVar;
    using GameEngine::CBTTerrain::Test::UnsetEnvVar;

    // Preserve any ambient value so this test is order-independent.
    const char*       prev    = std::getenv("GE_CBT_DEEP_DECODE");
    const bool        hadPrev = prev != nullptr;
    const std::string saved   = hadPrev ? prev : "";

    UnsetEnvVar("GE_CBT_DEEP_DECODE");
    EXPECT_EQ(DD::SubdivCap(), kMaxDecodeSubdiv) << "flag unset -> the shipped cap (dark-ship)";
    SetEnvVar("GE_CBT_DEEP_DECODE", "0");
    EXPECT_EQ(DD::SubdivCap(), kMaxDecodeSubdiv) << "flag 0 -> the shipped cap";
    SetEnvVar("GE_CBT_DEEP_DECODE", "1");
    EXPECT_EQ(DD::SubdivCap(), DD::kDeepDecodeSubdiv) << "flag 1 -> the deep prototype cap";

    if (hadPrev)
        SetEnvVar("GE_CBT_DEEP_DECODE", saved.c_str());
    else
        UnsetEnvVar("GE_CBT_DEEP_DECODE");
}
