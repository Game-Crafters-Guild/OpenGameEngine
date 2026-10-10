// Real-device oracle for the spherical (cube-sphere) CBT domain (plan §8 C7). The hard
// part is CROSS-FACE conformity: refinement must stay a conforming closed manifold as it
// crosses cube-face boundaries. These tests refine a spherical instance on a real Vulkan
// device, read the live HeapIDs back, decode them to EXACT integer cube coordinates on a
// common grid (an independent CPU mirror of the shader's shared barycentric LEB walk),
// and assert every undirected edge is shared by exactly two triangles — a T-junction from
// a wrong cross-face neighbor link would show as an edge shared once. Also: Validate green
// (link reciprocity across faces, no zombies, budget non-negative), uniform winding, and
// the decoded corners actually landing on the sphere shell.
//
// The conformity oracle decodes from HeapID (topology), independent of the shader's world
// output, so it isolates the neighbor-link correctness; a separate on-shell test confirms
// the shader's spherical projection ran. This is the "extend the conformity oracle across
// faces" requirement of the C7 arc law.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <vector>

#include "CBTTerrain/CBTDeepDecode.h" // kDeepDecodeSubdiv (S2b deep-cap pose probes)
#include "CBTTerrain/CBTDemandTuning.h"
#include "CBTTerrain/CBTInstance.h"
#include "CBTTerrain/CBTKernelSet.h"
#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrain/CBTSphereRoots.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Core/CommandList.h"
#include "CBTTestHarness.h"

using namespace GameEngine::CBTTerrain;
using namespace GameEngine::CBTTerrain::Test;
using namespace GameEngine::Rendering;

namespace
{
constexpr double kPiConst = 3.14159265358979323846;
constexpr uint32_t kVertexWordsPerSlot = sizeof(CBTVertexData) / 4u; // 24 — S2a 96B layout (sector tail appended; corner/meta word offsets unchanged)
// Common integer grid the exact int64 barycentric decode is re-quantized onto so vertices
// decoded at different depths compare bit-exactly. Raised 23 -> 40 for decode-precision
// slice 1, and 40 -> 50 for the S2b deep-decode cap lift: the deep (sector, local) pose
// probes drive live bisectors to numSubdiv 50 (DeepDecode::kDeepDecodeSubdiv), so the grid
// must resolve every corner at that depth (num << (50 - numSubdiv) <= 2^50, well within
// int64). Scale-invariant checks (conformity equality, winding sign) are unaffected by the
// shift value; a deeper grid just lets deeper decodes be compared exactly.
constexpr int kConformShift = static_cast<int>(DeepDecode::kDeepDecodeSubdiv); // common grid 2^50
static_assert(kConformShift <= 62, "conform grid corners must stay within int64");

uint32_t HeapDepth(uint64_t h)
{
    uint32_t d = 0;
    while (h > 1u)
    {
        h >>= 1;
        ++d;
    }
    return d;
}

using IVec3 = std::array<int64_t, 3>;
struct CubeTri
{
    IVec3 c[3];
};

// CPU mirror of the shader's shared barycentric LEB walk (cbt_kernels.comp
// Kernel_VertexEval), mapped to exact integer cube coordinates via the same sphere root
// corners the GPU decodes from (CBTSphereRoots.h). Result is on the common 2^kConformShift grid
// (now 2^40) so vertices decoded from different depths compare bit-exactly.
CubeTri DecodeCubeTri(uint64_t heapID, const std::array<CBTSphereRoot, kSphereRootCount>& roots)
{
    const uint32_t depth = HeapDepth(heapID);
    const uint32_t numSubdiv = depth - kSphereBaseDepth;
    const int64_t scale = int64_t(1) << numSubdiv;

    IVec3 b0{scale, 0, 0};
    IVec3 b1{0, scale, 0};
    IVec3 b2{0, 0, scale};
    for (uint32_t s = numSubdiv; s > 0u; --s)
    {
        const uint32_t bit = static_cast<uint32_t>(heapID >> (s - 1u)) & 1u;
        const IVec3 mid{(b0[0] + b2[0]) / 2, (b0[1] + b2[1]) / 2, (b0[2] + b2[2]) / 2};
        if (bit == 0u)
        {
            const IVec3 nb0 = b2, nb2 = b1;
            b0 = nb0;
            b1 = mid;
            b2 = nb2;
        }
        else
        {
            const IVec3 nb0 = b1, nb2 = b0;
            b0 = nb0;
            b1 = mid;
            b2 = nb2;
        }
    }

    const uint32_t rootIndex = static_cast<uint32_t>(heapID >> numSubdiv) - (1u << kSphereBaseDepth);
    const CBTSphereRoot& root = roots[rootIndex];
    const int shift = kConformShift - static_cast<int>(numSubdiv);

    auto toCube = [&](const IVec3& bary) {
        IVec3 out{};
        for (int c = 0; c < 3; ++c)
        {
            const int64_t num = bary[0] * root.V0[c] + bary[1] * root.V1[c] + bary[2] * root.V2[c];
            out[c] = num << shift; // (num/scale) * 2^kConformShift, exact since numSubdiv <= kConformShift
        }
        return out;
    };
    CubeTri t;
    t.c[0] = toCube(b0);
    t.c[1] = toCube(b1);
    t.c[2] = toCube(b2);
    return t;
}

std::array<int64_t, 6> EdgeKey(const IVec3& a, const IVec3& b)
{
    const bool aFirst = a < b;
    const IVec3& lo = aFirst ? a : b;
    const IVec3& hi = aFirst ? b : a;
    return {lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]};
}

// Reads every live bisector's HeapID and decodes it to an exact cube triangle.
std::vector<CubeTri> ReadLiveCubeTris(CBTInstance& instance, uint32_t slotCount)
{
    const auto roots = BuildSphereRoots();
    const auto heap = instance.DebugReadWords(CBTBinding::HeapID, slotCount * 2u);
    std::vector<CubeTri> tris;
    for (uint32_t slot = 0; slot < slotCount; ++slot)
    {
        if (heap[slot * 2u] == 0u && heap[slot * 2u + 1u] == 0u)
            continue;
        const uint64_t heapID = static_cast<uint64_t>(heap[slot * 2u]) |
                                (static_cast<uint64_t>(heap[slot * 2u + 1u]) << 32);
        tris.push_back(DecodeCubeTri(heapID, roots));
    }
    return tris;
}

// Closed manifold: every undirected edge shared by EXACTLY two triangles. No boundary
// edges (unlike the planar base) — a cross-face T-junction shows as a count != 2.
uint32_t CountNonConformingEdges(const std::vector<CubeTri>& tris)
{
    std::map<std::array<int64_t, 6>, int> counts;
    for (const CubeTri& t : tris)
    {
        ++counts[EdgeKey(t.c[0], t.c[1])];
        ++counts[EdgeKey(t.c[1], t.c[2])];
        ++counts[EdgeKey(t.c[2], t.c[0])];
    }
    uint32_t bad = 0;
    for (const auto& [key, cnt] : counts)
    {
        (void)key;
        if (cnt != 2)
            ++bad;
    }
    return bad;
}

CBTFrameParams SphereParams(float radius, float reliefAmp, float reliefFreq)
{
    CBTFrameParams p{};
    p.PlanetParams[0] = radius;
    p.PlanetParams[1] = reliefAmp;
    p.PlanetParams[2] = reliefFreq;
    return p;
}

// Build a valid sphere HeapID for `rootIndex` at numSubdiv = subdiv, with the given LEB path
// bits. Root r sits at heap id (1 << kSphereBaseDepth) + r; the path bits append below it.
// Lets the CPU decode oracle drive depths far beyond what a pool-bounded GPU refinement can
// materialise, isolating the DECODE precision (the thing slice 1 changes) from tree growth.
uint64_t MakeSphereHeapID(uint32_t rootIndex, uint64_t pathBits, uint32_t subdiv)
{
    const uint64_t rootHeap = (uint64_t(1) << kSphereBaseDepth) + rootIndex;
    const uint64_t mask = (subdiv >= 64u) ? ~uint64_t(0) : ((uint64_t(1) << subdiv) - 1u);
    return (rootHeap << subdiv) | (pathBits & mask);
}

// Exact cube integer corner (grid 2^kConformShift) -> geometric world position on the shell.
// Normalize in double so this is the TRUE facet geometry (independent of the fp32 storage the
// shader writes) — the reference the facet-size acceptance measures against.
std::array<double, 3> CubeIntToWorld(const IVec3& ci, double radius)
{
    const double s = std::ldexp(1.0, kConformShift); // 2^kConformShift
    const double x = static_cast<double>(ci[0]) / s;
    const double y = static_cast<double>(ci[1]) / s;
    const double z = static_cast<double>(ci[2]) / s;
    const double len = std::sqrt(x * x + y * y + z * z);
    return {x / len * radius, y / len * radius, z / len * radius};
}

double WorldDist(const std::array<double, 3>& a, const std::array<double, 3>& b)
{
    const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Signed volume of (origin, c0, c1, c2) in exact-ish long double — its sign is the triangle's
// orientation about the planet centre; 0 means the three cube integers are collinear (a
// degenerate facet the integer decode must never produce, at any depth).
long double SignedVolume(const CubeTri& t)
{
    const IVec3& a = t.c[0];
    const IVec3& b = t.c[1];
    const IVec3& c = t.c[2];
    const long double cx = static_cast<long double>(b[1]) * c[2] - static_cast<long double>(b[2]) * c[1];
    const long double cy = static_cast<long double>(b[2]) * c[0] - static_cast<long double>(b[0]) * c[2];
    const long double cz = static_cast<long double>(b[0]) * c[1] - static_cast<long double>(b[1]) * c[0];
    return static_cast<long double>(a[0]) * cx + static_cast<long double>(a[1]) * cy +
           static_cast<long double>(a[2]) * cz;
}

// fp32 world-corner storage ULP at radius R (√3·R·2⁻²³): the largest gap between two adjacent
// distinct fp32 world corners. A facet edge below this collapses on store (degenerate); the cap
// is chosen so the finest facet stays a healthy multiple above it.
double StorageUlpMeters(double radius)
{
    return std::sqrt(3.0) * radius * std::ldexp(1.0, -23); // √3 · R · 2^-23
}

// One altitude checkpoint of the multi-radius descent probe (large-planet validation slice).
// WholeMed is the whole-live-pool median split-edge (the shipped product-bar metric); NearMed is
// the median inside a small disc under the camera nadir ("detail under the feet"), which
// disentangles near-field detail from the larger visible-horizon disc a big planet shows.
// DroppedOverSettle is the delta of the since-init overflow counter across the settle frames — the
// steady-state dropped-split rate at the pose (screen-bounded demand => ~0).
struct DescentRow
{
    float Alt;
    uint32_t Live;
    double Occ;
    double WholeMed;
    double NearMed;
    double MinEdge;
    uint32_t MaxND;
    int SplitDemand;       // per-frame split candidates (kWQSplitCounter, reset each frame)
    int DroppedOverSettle; // overflow delta across the settle window
};

// Converged 150 m-class surface pose of the multi-radius cost probe. Adds wall-clock timing
// (min-of-N under the shared box's noise) and the steady-state dropped-split count over the timed
// quiescent window.
struct CostRow
{
    float Radius;
    float Alt;
    uint32_t Live;
    double Occ;
    double MinEdge;
    double WholeMed;
    double NearMed;
    double MaxEdge;
    uint32_t MaxND;
    int Overflow;             // cumulative since init
    int DroppedOverQuiescent; // overflow delta across the timed quiescent window
    double QuiescentMinMs;
    double QuiescentMeanMs;
    double ChangingMinMs;
    uint32_t Bad;
};

// A GRAZING walk pose: camera 1.7-5 m above the surface looking ALONG it (not top-down), the
// scenario the user's Facets screenshot exposed. AheadMed / FeetMed are the median facet in a disc
// at the screen-centre ground hit and at the nearest visible ground ("at the walker's feet") — the
// honest walking-detail metric. WholeMed is the whole-pool median for reference; the gap between
// AheadMed and the top-down probe's near median at the same altitude is the grazing-starvation
// signal (the projected-AREA metric collapses on the foreshortened near ground).
struct GrazingRow
{
    float Radius;
    float Alt;
    uint32_t Live;
    double Occ;
    double WholeMed;
    double AheadMed;
    double FeetMed;
    double FeetMin;
    uint32_t MaxND;
    int SplitDemand;
    int DroppedOverSettle;
    uint32_t Bad;
};

// Rotation-churn signature at a fixed grazing pose: per-frame split + simplify (merge) DEMAND during
// a steady camera yaw vs a forward-translation control. The #598 frustum demand gate coarsens
// off-frustum geometry to base and re-refines it one level/frame, so a yaw (which sweeps bisectors
// in/out of the ±1.1 NDC margin) churns far more than a translate — the signature of the user's
// "massive rotational jumps". SimplifyMean is the coarsen half of the churn (facets merging to base
// as they leave the frustum); a fix that keeps depth off-frustum drops both.
struct RotationRow
{
    float Radius;
    float Alt;
    double YawSplitMean;
    double YawSplitMax;
    double YawSimplifyMean;
    double YawSimplifyMax;
    double TransSplitMean;
    double TransSimplifyMean;
    int YawDropped;
    double Occ; // live occupancy at the converged yaw pose — tells headroom (keep-depth) from saturation
    uint32_t Bad;
};

// Probe configuration (walking-headroom slice). CapSubdiv is the numSubdiv the classifier is capped
// at: the existing #599 probes leave it at kMaxDecodeSubdiv (the "+40" characterization); the
// production-cap probes set it to what ResolveTerrainMaxDepth derives ("what she sees"). DemandOn +
// the four knobs mirror the editor's CBTFrameParams.DemandTuning (findings 2/3/4); every field left
// off => byte-identical to the pre-slice metric. Defaults reproduce the #599 probe behavior exactly,
// so a caller that passes {} measures the shipped baseline.
struct ProbeConfig
{
    uint32_t CapSubdiv = kMaxDecodeSubdiv; // numSubdiv cap (production cap for "what she sees")
    bool DemandOn = false;                 // findings 2/3/4 demand shaping
    float NearFieldTargetM = 0.25f;        // finding 2: behind-eye/straddle force-split floor (m)
    float KeepOcc = 0.90f;                 // finding 3: keep off-frustum depth below this occupancy
    float RescuePxMul = 0.0f;              // finding 4: rescue px = this * TPE; 0 = rescue off
    float RescueOcc = 0.90f;               // finding 4: rescue only below this occupancy
    // Pool-scaling slice S3: measure the SHIP idle floor, not the test-harness one. ValidateEachUpdate
    // false drops the whole-pool debug Validate kernel (CBTRenderFeature does the same in ship);
    // GateTimedFrames true runs the timed quiescent frames with GateVertexEval=1 (the production
    // quiescence gate — a converged frame re-evaluates ~0 vertices). Both default to the pre-slice
    // harness behavior so the existing cost probes are byte-identical.
    bool ValidateEachUpdate = true;
    bool GateTimedFrames = false;
};

// numSubdiv the spherical cap derivation lands on for a radius + facet target — the CPU mirror of
// DeriveTerrainMaxDepth's spherical branch (subdiv = round(2*log2(pi*R/2 / target)), clamped to the
// int64 decode cap). Lets the production-cap probes run at exactly the depth the editor would.
// capSubdiv defaults to the shipped fp32-store clamp so every flag-OFF probe is byte-
// unchanged; the S2b deep-decode pose probes pass DeepDecode::kDeepDecodeSubdiv (the
// flag-ON provisioning clamp — TerrainProvisioning SubdivCapFor).
uint32_t ProductionCapSubdiv(double radius, double targetM, uint32_t capSubdiv = kMaxDecodeSubdiv)
{
    const double rootArc = kPiConst * std::max(radius, 0.0) * 0.5;
    const double ratio = rootArc / targetM;
    if (!(ratio > 1.0))
        return 0u;
    const long sub = std::lround(2.0 * std::log2(ratio));
    return static_cast<uint32_t>(std::clamp<long>(sub, 0, static_cast<long>(capSubdiv)));
}

// The finest facet a cube-sphere cap can produce: (pi*R/2) * 2^(-subdiv/2). Below this NO altitude can
// refine — the deterministic, convergence-free core of finding 1 (the shipped 1 m cap's floor sits
// above the 0.5 m product bar, so walking-eye detail was unreachable at any pose).
double CapFloorMeters(double radius, uint32_t subdiv)
{
    return (kPiConst * std::max(radius, 0.0) * 0.5) * std::ldexp(1.0, -static_cast<int>(subdiv) / 2);
}

// Fills the CBTFrameParams DemandTuning fields from a config (findings 2/3/4). No-op when DemandOn
// is false, so a default-config probe is byte-identical to #599. Call after Screen is populated
// (the edge-rescue threshold scales with the split-threshold px).
void ApplyDemandTuning(CBTFrameParams& p, const ProbeConfig& cfg)
{
    if (!cfg.DemandOn)
        return;
    p.DemandTuning[0] = cfg.NearFieldTargetM;
    p.DemandTuning[1] = cfg.KeepOcc;
    p.DemandTuning[2] = cfg.RescuePxMul * p.Screen[2];
    p.DemandTuning[3] = cfg.RescueOcc;
}
} // namespace

class CBTSphereDomainTest : public ::testing::Test
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

    // Graded exponential descent (orbit -> ~4 m) on a full planet of the given radius at the shipped
    // default relief + TPE, capturing occupancy / facet / demand at each altitude checkpoint. Shared
    // by the R=2000 regression anchor and the R=20000 / R=50000 large-planet validation tests so all
    // three radii are measured by identical code (only the radius + checkpoint list differ).
    std::vector<DescentRow> RunAltitudeDescent(float radius, const std::vector<float>& checkpoints,
                                               const char* tag, const ProbeConfig& cfg = {});

    // Converge a fixed near-surface pose (finalAlt m above the surface) on a full planet of the given
    // radius, then report occupancy + facet distribution + a quiescent/changing wall-clock cost and
    // the steady-state dropped-split count. Shared by the three-radius cost/occupancy comparison.
    CostRow RunCostProbe(float radius, float finalAlt, const char* tag, const ProbeConfig& cfg = {});

    // Converge a GRAZING walk pose (camera `alt` m above the surface looking along it) and report the
    // facet size at the screen-centre ground hit and at the walker's feet — the real walking-detail
    // metric (top-down probes hide grazing starvation). Relief amp 0 (clean sphere, matches the
    // flatten-modifier repro).
    GrazingRow RunGrazingProbe(float radius, float alt, const char* tag, const ProbeConfig& cfg = {});

    // Converge a fixed grazing pose then measure the per-frame split/merge demand under a steady yaw
    // vs a forward translation — the rotation-churn signature of the #598 frustum demand gate.
    RotationRow RunRotationChurn(float radius, float alt, const char* tag, const ProbeConfig& cfg = {});

    std::unique_ptr<IDevice> m_Device;
    CBTKernelSet m_KernelSet;
};

// The spherical instance seeds 24 roots and reports baseDepth 5.
TEST_F(CBTSphereDomainTest, SeedsTwentyFourRoots)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots(kDomainSpherical));
    EXPECT_EQ(instance.GetDomainMode(), kDomainSpherical);
    EXPECT_EQ(instance.GetRootCount(), 24u);
    EXPECT_EQ(instance.GetBaseDepth(), kSphereBaseDepth);

    // One inert frame (target == baseDepth) leaves the 24 roots live; the ALL-stream draw
    // count is 3 indices per live bisector.
    CBTClassifyDesc inert{};
    inert.Mode = kClassifyDepthTarget;
    inert.TargetDepth = kSphereBaseDepth;
    RunFrame(instance, inert, SphereParams(1000.0f, 0.0f, 4.0f), 0);

    auto readback = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
    readback->Begin();
    instance.RecordReadback(*readback);
    readback->End();
    std::vector<CommandList*> lists{readback.get()};
    m_Device->ExecuteCommandLists(lists);
    m_Device->WaitForIdle();
    EXPECT_EQ(instance.ReadDrawIndexCount(kDrawStreamAll) / 3u, 24u) << "24 roots must be live";
    EXPECT_EQ(instance.ReadValidationErrorCount(), 0u) << "base mesh link reciprocity";
}

// Quiescence law on the GPU (plan §planet-shading perf): with the VertexEval gate ON, a
// CONVERGED frame that changes nothing re-evaluates ~0 vertices, not the whole live pool — the
// fix for the CBT.Update regression where full-pool multi-octave relief ran every frame. A
// frame that DEEPENS the tree evaluates the newly created bisectors (proving the counter tracks
// real work and the pool is large). Discriminating: a gate that ignores the MODIFIED flag would
// evaluate the whole pool on the quiescent frame and fail the bound.
TEST_F(CBTSphereDomainTest, GatedVertexEvalIsQuiescent)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots(kDomainSpherical));

    CBTClassifyDesc refine{};
    refine.Mode = kClassifyDepthTarget;
    refine.FocusRoot = kFocusRootAll;
    refine.TargetDepth = kSphereBaseDepth + 6u; // 24 * 2^6 = 1536 leaves — a non-trivial pool
    const CBTFrameParams p = SphereParams(1000.0f, 60.0f, 6.0f);

    // Frame 0: force a full-pool eval (gate off) so every root's corners are seeded.
    refine.GateVertexEval = 0u;
    RunFrame(instance, refine, p, 0);
    // Refine to convergence WITH the gate on: only newly split bisectors get evaluated.
    refine.GateVertexEval = 1u;
    for (uint32_t f = 1; f <= 24; ++f)
        RunFrame(instance, refine, p, f);

    // A further frame at the SAME target is quiescent: no splits/merges -> ~0 vertices evaluated.
    RunFrame(instance, refine, p, 25);
    const int32_t quiescent = instance.ReadVertexEvalCount();
    EXPECT_LE(quiescent, 8) << "quiescent gated frame re-evaluated ~the whole pool (gate broken)";

    // Deepening one level creates many bisectors; the gate evaluates exactly those.
    refine.TargetDepth = kSphereBaseDepth + 7u;
    RunFrame(instance, refine, p, 26);
    const int32_t changed = instance.ReadVertexEvalCount();
    EXPECT_GT(changed, 100) << "a refining frame must evaluate the newly created bisectors";
    EXPECT_GT(changed, quiescent * 10 + 50) << "quiescent must be far cheaper than a changing frame";
    EXPECT_EQ(instance.ReadValidationErrorCount(), 0u) << "gate must not corrupt the tree";
}

// Cross-face conformity under the PRODUCTION screen-space metric with a near-surface
// perspective camera — the low-altitude planet case with a strong near/far gradient
// across cube faces. Converge, then assert the whole live set is a conforming closed
// manifold (every edge shared exactly twice, across faces).
TEST_F(CBTSphereDomainTest, CrossFaceConformityScreenSpace)
{
    using namespace GameEngine::Mathematics;
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots(kDomainSpherical));

    const float radius = 1000.0f;
    // Camera just outside the surface, looking at the planet centre: the near face
    // refines deep, the horizon faces stay coarse -> cross-face depth transitions.
    const Vector3 eye(0.0f, 0.0f, -(radius + 60.0f));
    const Matrix4x4 view = MakeLookAtLH(eye, Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f));
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(1.2f, 16.0f / 9.0f, 1.0f, 100000.0f);
    const Matrix4x4 vp = proj * view;

    CBTFrameParams p = SphereParams(radius, 8.0f, 5.0f);
    // Origin inactive (RenderOriginSector default 0): viewProjRel == the world viewProj.
    std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
    p.CameraPos[0] = eye.x; p.CameraPos[1] = eye.y; p.CameraPos[2] = eye.z; p.CameraPos[3] = 1.0f;
    p.Screen[0] = 1600.0f; p.Screen[1] = 900.0f; p.Screen[2] = 20.0f; p.Screen[3] = 10.0f;
    p.TerrainOrigin[2] = static_cast<float>(kSphereBaseDepth + 10u); // max depth cap

    CBTClassifyDesc classify{};
    classify.Mode = kClassifyScreenSpace;
    classify.TargetDepth = kSphereBaseDepth + 10u;
    for (uint32_t f = 0; f < 40u; ++f)
        RunFrame(instance, classify, p, f);

    const std::vector<CubeTri> tris = ReadLiveCubeTris(instance, kDefaultBisectorPoolSize);
    ASSERT_GT(tris.size(), 64u) << "screen-space refinement produced no gradient on the sphere";
    const uint32_t bad = CountNonConformingEdges(tris);
    std::printf("[sphere-screenspace] %zu tris, non-conforming edges %u\n", tris.size(), bad);
    EXPECT_EQ(bad, 0u) << "cross-face T-junction under the production metric (real crack source)";
    EXPECT_EQ(instance.ReadValidationErrorCount(), 0u);
    EXPECT_EQ(instance.ReadValidationCounter(kValidationZombieCounter), 0u);
    EXPECT_GE(instance.ReadWorkQueueCounter(kWQFreeCount), 0);
}

// Cross-face conformity under a DETERMINISTIC focused-depth gradient: drive one root's
// subtree deep while the rest stay near the base, forcing a hard depth step across the
// focused root's cube edges (including its cross-face twin). Assert conformity on every
// frame of the sweep — a half-propagated cross-face split would show a transient crack.
TEST_F(CBTSphereDomainTest, CrossFaceConformityFocusedSweep)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots(kDomainSpherical));

    const CBTFrameParams params = SphereParams(500.0f, 0.0f, 4.0f);
    uint32_t frame = 0;
    for (uint32_t focus = 0; focus < 6u; ++focus) // one root per cube face
    {
        CBTClassifyDesc refine{};
        refine.Mode = kClassifyDepthTarget;
        refine.FocusRoot = focus * 4u; // a pie slice on face `focus`
        refine.TargetDepth = kSphereBaseDepth + 7u;
        for (uint32_t i = 0; i < 10u; ++i, ++frame)
        {
            RunFrame(instance, refine, params, frame);
            const std::vector<CubeTri> tris = ReadLiveCubeTris(instance, 4096u);
            ASSERT_GE(tris.size(), 24u);
            EXPECT_EQ(CountNonConformingEdges(tris), 0u)
                << "cross-face T-junction at frame " << frame << " (focus root " << (focus * 4u)
                << ", " << tris.size() << " live triangles)";
        }
    }
    EXPECT_EQ(instance.ReadValidationErrorCount(), 0u);
}

// The shader's spherical projection actually ran: every stored corner lands on the sphere
// shell (radius +/- relief bound). This checks the world output (vs. the CPU-topology
// conformity oracle above), so both the topology and the geometry are covered.
TEST_F(CBTSphereDomainTest, CornersLandOnSphereShell)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots(kDomainSpherical));

    const float radius = 800.0f;
    const float amp = 12.0f;
    CBTClassifyDesc refine{};
    refine.Mode = kClassifyDepthTarget;
    refine.FocusRoot = kFocusRootAll;
    refine.TargetDepth = kSphereBaseDepth + 4u;
    for (uint32_t f = 0; f < 8u; ++f)
        RunFrame(instance, refine, SphereParams(radius, amp, 5.0f), f);

    constexpr uint32_t kSlots = 4096u;
    const auto heap = instance.DebugReadWords(CBTBinding::HeapID, kSlots * 2u);
    const auto verts =
        instance.DebugReadWords(CBTBinding::CurrentVertex, kSlots * kVertexWordsPerSlot);
    // Relief bound: amplitude * (1 + 0.5) from the two sin octaves in CBT_PlanetRelief.
    const float maxRelief = amp * 1.5f + 1.0f;
    uint32_t checked = 0;
    for (uint32_t slot = 0; slot < kSlots; ++slot)
    {
        if (heap[slot * 2u] == 0u && heap[slot * 2u + 1u] == 0u)
            continue;
        const uint32_t base = slot * kVertexWordsPerSlot;
        for (uint32_t k = 0; k < 3u; ++k)
        {
            float xyz[3];
            std::memcpy(xyz, &verts[base + k * 4u], sizeof(xyz));
            const float len = std::sqrt(xyz[0] * xyz[0] + xyz[1] * xyz[1] + xyz[2] * xyz[2]);
            ASSERT_GE(len, radius - maxRelief) << "corner inside the shell (slot " << slot << ")";
            ASSERT_LE(len, radius + maxRelief) << "corner outside the shell (slot " << slot << ")";
            ++checked;
        }
    }
    EXPECT_GT(checked, 64u * 3u) << "refinement produced too few corners to be meaningful";
}

// Uniform winding across depths (the decode preserves orientation on the sphere too, so
// no per-triangle backface flip). Signed volume of the tetra (origin, c0, c1, c2) has a
// consistent sign for every decoded cube triangle.
TEST_F(CBTSphereDomainTest, WindingUniformOnSphere)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots(kDomainSpherical));

    CBTClassifyDesc refine{};
    refine.Mode = kClassifyDepthTarget;
    refine.FocusRoot = 0u;
    refine.TargetDepth = kSphereBaseDepth + 6u;
    for (uint32_t f = 0; f < 12u; ++f)
        RunFrame(instance, refine, SphereParams(500.0f, 0.0f, 4.0f), f);

    const std::vector<CubeTri> tris = ReadLiveCubeTris(instance, 4096u);
    ASSERT_GT(tris.size(), 32u);
    uint32_t positive = 0, negative = 0, degenerate = 0;
    for (const CubeTri& t : tris)
    {
        // Signed volume of (origin, c0, c1, c2) = c0 . (c1 x c2); its sign is the
        // triangle's outward/inward orientation about the planet centre.
        const IVec3& a = t.c[0];
        const IVec3& b = t.c[1];
        const IVec3& c = t.c[2];
        const long double cx = static_cast<long double>(b[1]) * c[2] - static_cast<long double>(b[2]) * c[1];
        const long double cy = static_cast<long double>(b[2]) * c[0] - static_cast<long double>(b[0]) * c[2];
        const long double cz = static_cast<long double>(b[0]) * c[1] - static_cast<long double>(b[1]) * c[0];
        const long double vol = static_cast<long double>(a[0]) * cx +
                                static_cast<long double>(a[1]) * cy +
                                static_cast<long double>(a[2]) * cz;
        if (vol > 0.0L)
            ++positive;
        else if (vol < 0.0L)
            ++negative;
        else
            ++degenerate;
    }
    EXPECT_EQ(degenerate, 0u) << "degenerate triangle in the sphere decode";
    EXPECT_TRUE(positive == 0u || negative == 0u)
        << "winding is NOT uniform on the sphere: " << positive << " + / " << negative << " -";
}

// ---------------------------------------------------------------------------
// Decode-precision slice 1 — CPU oracles beyond the old fp32-exact cap (numSubdiv 23)
// ---------------------------------------------------------------------------
// These decode directly from constructed deep HeapIDs, so they exercise numSubdiv 24..40
// WITHOUT needing a pool-bounded GPU refinement to physically reach depth 40 (2^40 leaves is
// far past the 131072-slot pool). They validate the DECODE — the exact int64 numerator N and
// the winding/on-shell/facet-size properties — which is precisely what slice 1 changes. The
// GPU cross-face conformity + on-shell at extended depth are covered separately below.

// Winding stays uniform and NO facet is degenerate through numSubdiv 40. A degenerate cube
// triangle (two integer corners collapsing) would mean the int64 walk lost precision; because
// the walk is exact to 2^52, distinct LEB corners stay distinct integers at 40.
TEST_F(CBTSphereDomainTest, DecodeWindingAndNonDegenerateThroughDepth40)
{
    const auto roots = BuildSphereRoots();
    const uint64_t paths[] = {0x0ull,        0xFFFFFFFFFFull, 0xAAAAAAAAAAull, 0x5555555555ull,
                              0x123456789Aull, 0x0F0F0F0F0Full, 0xC3C3C3C3C3ull, 0x2468ACE013ull};

    for (uint32_t subdiv : {24u, 32u, 40u})
    {
        uint32_t positive = 0, negative = 0, degenerate = 0, checked = 0;
        for (uint32_t r = 0; r < kSphereRootCount; ++r)
        {
            for (uint64_t path : paths)
            {
                const uint64_t heapID = MakeSphereHeapID(r, path, subdiv);
                const CubeTri t = DecodeCubeTri(heapID, roots);
                const long double vol = SignedVolume(t);
                if (vol > 0.0L)
                    ++positive;
                else if (vol < 0.0L)
                    ++negative;
                else
                    ++degenerate;
                ++checked;
            }
        }
        EXPECT_EQ(degenerate, 0u)
            << "degenerate facet at numSubdiv " << subdiv << " (int64 decode lost precision)";
        EXPECT_TRUE(positive == 0u || negative == 0u)
            << "winding NOT uniform at numSubdiv " << subdiv << ": " << positive << " + / "
            << negative << " -";
        EXPECT_GT(checked, 100u);
    }
}

// A shared vertex reached via TWO different bit-paths decodes to a BIT-IDENTICAL integer N at
// numSubdiv 40 — the crack-free-by-construction property, exercised past the old cap. A
// triangle's split-edge midpoint m is the corner[1] of BOTH its children (heapID 2h and 2h+1);
// the two children walk independent bit-paths (differing in the final bit) yet must land on the
// identical integer m. This is the deepest-depth analogue of the design probe's Table 2 seam.
TEST_F(CBTSphereDomainTest, SharedMidpointDecodesBitIdenticalAtDepth40)
{
    const auto roots = BuildSphereRoots();
    const uint64_t paths[] = {0x0ull, 0x9E3779B97Full, 0xAAAAAAAAAAull, 0x13579BDF02ull};
    constexpr uint32_t kSubdiv = 39u; // parent at 39 -> children at 40
    uint32_t checked = 0;
    for (uint32_t r = 0; r < kSphereRootCount; ++r)
    {
        for (uint64_t path : paths)
        {
            const uint64_t parent = MakeSphereHeapID(r, path, kSubdiv);
            const CubeTri childA = DecodeCubeTri(parent * 2ull, roots);       // final bit 0
            const CubeTri childB = DecodeCubeTri(parent * 2ull + 1ull, roots); // final bit 1
            // Both children carry the split midpoint as corner[1]; must be identical integers.
            EXPECT_EQ(childA.c[1], childB.c[1])
                << "shared midpoint diverged at numSubdiv 40 (root " << r << ") — a T-junction crack";
            ++checked;
        }
    }
    EXPECT_GT(checked, 64u);
}

// Facet-size-vs-radius acceptance (design §7). At the cap (numSubdiv 40) the minimum facet edge
// must (a) meet the close-range detail target — ~9 mm @R=20 km, ~23 mm @R=50 km, a ~1000x
// improvement over the old ~7-27 m cap floor — and (b) stay a healthy margin above the fp32
// world-storage ULP so no facet degenerates on store. The edge is measured on the TRUE geometry
// (double normalize of the exact int64 cube corner).
TEST_F(CBTSphereDomainTest, MinFacetEdgeAtCapMeetsRadiusTarget)
{
    const auto roots = BuildSphereRoots();
    // Sample a broad set of depth-40 facets (an LCG walk of the 40-bit path per root plus the
    // corner spines) so the min tracks the smallest — the sphere-warp-compressed facets near a
    // cube corner. The absolute value depends on which facet the fan reaches; the acceptance is
    // the order of magnitude (metres -> millimetres) and the anti-degeneracy margin.
    std::vector<uint64_t> paths = {0x0ull, 0xFFFFFFFFFFull, 0xAAAAAAAAAAull, 0x5555555555ull};
    uint64_t lcg = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 256; ++i)
    {
        lcg = lcg * 6364136223846793005ull + 1442695040888963407ull;
        paths.push_back(lcg);
    }

    struct Case { double radius; double targetMeters; };
    // Old fp32-exact cap (numSubdiv 23) facet floor for reference (~10.9 m @20 km, ~27 m @50 km):
    // (πR/2)·2^(-23/2). The acceptance is a >=100x improvement AND staying above the storage ULP.
    const Case cases[] = {{20000.0, 0.020}, {50000.0, 0.050}}; // measured ~13.5 mm / ~33.8 mm

    for (const Case& kase : cases)
    {
        const uint32_t subdiv = kMaxDecodeSubdiv; // 40
        double minEdge = std::numeric_limits<double>::max();
        double maxEdge = 0.0;
        for (uint32_t r = 0; r < kSphereRootCount; ++r)
        {
            for (uint64_t path : paths)
            {
                const CubeTri t = DecodeCubeTri(MakeSphereHeapID(r, path, subdiv), roots);
                const std::array<double, 3> w0 = CubeIntToWorld(t.c[0], kase.radius);
                const std::array<double, 3> w1 = CubeIntToWorld(t.c[1], kase.radius);
                const std::array<double, 3> w2 = CubeIntToWorld(t.c[2], kase.radius);
                for (double e : {WorldDist(w0, w1), WorldDist(w1, w2), WorldDist(w2, w0)})
                {
                    minEdge = std::min(minEdge, e);
                    maxEdge = std::max(maxEdge, e);
                }
            }
        }
        const double ulp = StorageUlpMeters(kase.radius);
        const double floor23 = (kPiConst * kase.radius * 0.5) * std::ldexp(1.0, -23 / 2) *
                               std::sqrt(0.5); // (πR/2)·2^(-11.5), the old-cap facet floor
        std::printf("[facet-cap] R=%.0f m  numSubdiv=%u  minEdge=%.4f mm  maxEdge=%.4f mm  "
                    "storageULP=%.4f mm  minEdge/ULP=%.2f  nd23Floor=%.2f m  improvement=%.0fx\n",
                    kase.radius, subdiv, minEdge * 1000.0, maxEdge * 1000.0, ulp * 1000.0,
                    minEdge / ulp, floor23, floor23 / minEdge);
        EXPECT_LT(minEdge, kase.targetMeters)
            << "min facet edge at cap did not reach the millimetre-scale target for R=" << kase.radius;
        EXPECT_GT(floor23 / minEdge, 100.0)
            << "min facet edge at cap is not >=100x finer than the old numSubdiv-23 floor for R="
            << kase.radius;
        EXPECT_GT(minEdge, 2.0 * ulp)
            << "min facet edge at cap is within 2x the fp32 storage ULP (degeneracy risk) for R="
            << kase.radius;
    }
}

// GPU cross-face conformity + on-shell at the EXTENDED cap (baseDepth + 40), decoded on the 2^40
// grid — the real int64 shader run under the production screen-space metric. This proves the
// widened decode + higher cap + 2^40 grid stay a conforming closed manifold (zero cross-face
// T-junctions) and land on the shell, at every depth the GPU actually materialises.
//
// Scope note (matches the design's §Scope caveat): this slice raises the DECODE ceiling; whether a
// region physically REACHES a deep numSubdiv on the GPU is gated by the 131072-slot bisector pool,
// an orthogonal lever the spike does not touch. A close-up of a full sphere refines its whole
// mid-field to numSubdiv ~13-18 and saturates the pool there (scale-invariant — the metric is
// angular), so the GPU-reachable depth is pool-capped around 18, well short of the 40 ceiling. The
// EXTENDED-depth (24-40) exactness is therefore proven by the CPU oracles above, which mirror this
// exact shader decode bit-for-bit (SharedMidpoint... / DecodeWinding... / MinFacetEdge...); this
// GPU test is the companion regression that the int64 shader + higher cap run crack-free and
// on-shell at the live pool's depth. A camera descent (300 m -> 1 m) keeps the mesh graded so it
// reaches the pool-limited max cleanly rather than locking during a cold breadth-first grow.
TEST_F(CBTSphereDomainTest, ScreenSpaceExtendedCapConformsAndLandsOnShell)
{
    using namespace GameEngine::Mathematics;
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots(kDomainSpherical));

    const float radius = 2000.0f;
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(1.2f, 16.0f / 9.0f, 0.05f, 20000.0f);

    CBTFrameParams p = SphereParams(radius, 0.0f, 5.0f); // relief 0 -> corners land exactly on R
    p.Screen[0] = 1600.0f; p.Screen[1] = 900.0f; p.Screen[2] = 16.0f; p.Screen[3] = 8.0f;
    const uint32_t capDepth = kSphereBaseDepth + kMaxDecodeSubdiv; // baseDepth + 40
    p.TerrainOrigin[2] = static_cast<float>(capDepth);

    CBTClassifyDesc classify{};
    classify.Mode = kClassifyScreenSpace;
    classify.TargetDepth = capDepth;

    // Descend the camera from 300 m to ~1 m over the surface. A COLD close-up refinement grows
    // breadth-first and exhausts the pool at a shallow uniform depth before it can grade; an
    // incremental descent (as the editor does frame-to-frame) keeps the mesh graded — the far
    // field merges as the near field deepens — so the near patch reaches deep numSubdiv while the
    // live count stays bounded. Exponential decay spends most frames at low altitude where the
    // deep refinement converges.
    constexpr uint32_t kFrames = 200u;
    for (uint32_t f = 0; f < kFrames; ++f)
    {
        const float alt = std::max(1.0f, 300.0f * std::pow(0.95f, static_cast<float>(f)));
        const Vector3 eye(0.0f, 0.0f, -(radius + alt));
        const Matrix4x4 vp =
            proj * MakeLookAtLH(eye, Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f));
        std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
        p.CameraPos[0] = eye.x; p.CameraPos[1] = eye.y; p.CameraPos[2] = eye.z; p.CameraPos[3] = 1.0f;
        RunFrame(instance, classify, p, f);
    }

    // Max numSubdiv actually reached (from the live HeapIDs) + a coarse depth histogram.
    const auto heap = instance.DebugReadWords(CBTBinding::HeapID, kDefaultBisectorPoolSize * 2u);
    uint32_t maxNumSubdiv = 0;
    uint32_t liveCount = 0;
    std::array<uint32_t, 64> depthHist{};
    for (uint32_t slot = 0; slot < kDefaultBisectorPoolSize; ++slot)
    {
        if (heap[slot * 2u] == 0u && heap[slot * 2u + 1u] == 0u)
            continue;
        const uint64_t heapID = static_cast<uint64_t>(heap[slot * 2u]) |
                                (static_cast<uint64_t>(heap[slot * 2u + 1u]) << 32);
        const uint32_t nd = HeapDepth(heapID) - kSphereBaseDepth;
        maxNumSubdiv = std::max(maxNumSubdiv, nd);
        if (nd < depthHist.size())
            ++depthHist[nd];
        ++liveCount;
    }

    const std::vector<CubeTri> tris = ReadLiveCubeTris(instance, kDefaultBisectorPoolSize);
    const uint32_t bad = CountNonConformingEdges(tris);
    std::printf("[extended-depth] %u live, %zu tris, maxNumSubdiv=%u, non-conforming edges=%u, "
                "freeCount=%d, overflow=%d\n",
                liveCount, tris.size(), maxNumSubdiv, bad,
                instance.ReadWorkQueueCounter(kWQFreeCount),
                instance.ReadWorkQueueCounter(kWQOverflowCounter));
    std::printf("[extended-depth] numSubdiv histogram (nd:count):");
    for (uint32_t nd = 0; nd < depthHist.size(); ++nd)
        if (depthHist[nd] > 0)
            std::printf(" %u:%u", nd, depthHist[nd]);
    std::printf("\n");

    // The extended cap must be engaged well past the old default screen-space tests (~nd 10) and
    // the base — but NOT necessarily past 23 (pool-capped, see the scope note above).
    EXPECT_GT(maxNumSubdiv, 14u)
        << "the extended cap did not engage — refinement stayed shallow (numSubdiv " << maxNumSubdiv
        << "), so this regression is not exercising the higher-cap / int64 path";
    EXPECT_EQ(bad, 0u)
        << "cross-face T-junction at numSubdiv up to " << maxNumSubdiv << " under the extended cap — "
           "the int64 decode failed to make shared corners identical (a real crack)";
    EXPECT_EQ(instance.ReadValidationErrorCount(), 0u);
    EXPECT_EQ(instance.ReadValidationCounter(kValidationZombieCounter), 0u);
    EXPECT_GE(instance.ReadWorkQueueCounter(kWQFreeCount), 0);

    // On-shell: with relief 0 every stored corner sits at |corner| == radius (fp32 tolerance).
    const auto verts =
        instance.DebugReadWords(CBTBinding::CurrentVertex, kDefaultBisectorPoolSize * kVertexWordsPerSlot);
    uint32_t checked = 0;
    for (uint32_t slot = 0; slot < kDefaultBisectorPoolSize; ++slot)
    {
        if (heap[slot * 2u] == 0u && heap[slot * 2u + 1u] == 0u)
            continue;
        const uint32_t base = slot * kVertexWordsPerSlot;
        for (uint32_t k = 0; k < 3u; ++k)
        {
            float xyz[3];
            std::memcpy(xyz, &verts[base + k * 4u], sizeof(xyz));
            const float len = std::sqrt(xyz[0] * xyz[0] + xyz[1] * xyz[1] + xyz[2] * xyz[2]);
            ASSERT_NEAR(len, radius, 1.0f) << "extended-depth corner off the shell (slot " << slot << ")";
            ++checked;
        }
    }
    EXPECT_GT(checked, 256u * 3u);
}

// TargetPixelError cost curve at a fixed near-surface pose (plan §planet-detail scope 1). The
// spherical default's facet size, bisector-pool occupancy, and overflow are all functions of the
// screen-space split threshold; this measures them for TPE 16/8/4/2 on the DEFAULT planet relief
// (amp 60, freq 6, oct 3) at R=2000, camera ~150 m above the surface (a realistic sculpt/inspect
// distance). It prints the honest cost curve the sphere-default decision rests on (facet edge vs
// live count vs overflow) and asserts the safe invariants: finer TPE -> finer facets + more (never
// fewer) triangles, conformity holds at every setting, and the current default (8) sustains the
// 131072-slot pool with headroom. Camera descends first so the mesh grades (a cold close-up grows
// breadth-first and exhausts the pool before it can grade — the scope note on the extended-cap test).
TEST_F(CBTSphereDomainTest, TargetPixelErrorCostCurveAtSurfacePose)
{
    using namespace GameEngine::Mathematics;
    const float radius = 2000.0f;
    const float amp = 60.0f, freq = 6.0f; // the default planet relief
    const float finalAlt = 150.0f;        // a realistic sculpt / inspection distance
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(1.2f, 16.0f / 9.0f, 0.05f, 20000.0f);
    const uint32_t cap = kSphereBaseDepth + kMaxDecodeSubdiv;

    struct Row
    {
        float Tpe;
        uint32_t Live;
        double MinEdge, MedEdge, MaxEdge;
        uint32_t MaxNumSubdiv;
        int Overflow;
        uint32_t Bad;
    };
    std::vector<Row> rows;

    for (float tpe : {16.0f, 8.0f, 4.0f, 2.0f})
    {
        CBTInstance instance;
        ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
        ASSERT_TRUE(instance.InitializeRoots(kDomainSpherical));

        CBTFrameParams p = SphereParams(radius, amp, freq);
        p.PlanetParams[3] = 3.0f; // octaves (the default)
        p.Screen[0] = 1600.0f;
        p.Screen[1] = 900.0f;
        p.Screen[2] = tpe;        // split threshold
        p.Screen[3] = tpe * 0.5f; // merge threshold (the shipped hysteresis band)
        p.TerrainOrigin[2] = static_cast<float>(cap);

        CBTClassifyDesc classify{};
        classify.Mode = kClassifyScreenSpace;
        classify.TargetDepth = cap;

        constexpr uint32_t kFrames = 180u;
        for (uint32_t f = 0; f < kFrames; ++f)
        {
            const float alt = std::max(finalAlt, 300.0f * std::pow(0.95f, static_cast<float>(f)));
            const Vector3 eye(0.0f, 0.0f, -(radius + alt));
            const Matrix4x4 vp =
                proj * MakeLookAtLH(eye, Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f));
            std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
            p.CameraPos[0] = eye.x; p.CameraPos[1] = eye.y; p.CameraPos[2] = eye.z; p.CameraPos[3] = 1.0f;
            RunFrame(instance, classify, p, f);
        }

        const auto heap = instance.DebugReadWords(CBTBinding::HeapID, kDefaultBisectorPoolSize * 2u);
        const auto verts = instance.DebugReadWords(CBTBinding::CurrentVertex,
                                                         kDefaultBisectorPoolSize * kVertexWordsPerSlot);
        std::vector<double> edges;
        edges.reserve(4096);
        uint32_t live = 0, maxND = 0;
        for (uint32_t slot = 0; slot < kDefaultBisectorPoolSize; ++slot)
        {
            if (heap[slot * 2u] == 0u && heap[slot * 2u + 1u] == 0u)
                continue;
            const uint64_t hid =
                static_cast<uint64_t>(heap[slot * 2u]) | (static_cast<uint64_t>(heap[slot * 2u + 1u]) << 32);
            maxND = std::max(maxND, HeapDepth(hid) - kSphereBaseDepth);
            const uint32_t base = slot * kVertexWordsPerSlot;
            float c0[3], c2[3];
            std::memcpy(c0, &verts[base + 0u], sizeof(c0)); // corner0.xyz
            std::memcpy(c2, &verts[base + 8u], sizeof(c2)); // corner2.xyz — the LEB split edge is (c0,c2)
            const double dx = c0[0] - c2[0], dy = c0[1] - c2[1], dz = c0[2] - c2[2];
            edges.push_back(std::sqrt(dx * dx + dy * dy + dz * dz));
            ++live;
        }
        std::sort(edges.begin(), edges.end());
        const std::vector<CubeTri> tris = ReadLiveCubeTris(instance, kDefaultBisectorPoolSize);

        Row row{};
        row.Tpe = tpe;
        row.Live = live;
        row.MinEdge = edges.empty() ? 0.0 : edges.front();
        row.MedEdge = edges.empty() ? 0.0 : edges[edges.size() / 2u];
        row.MaxEdge = edges.empty() ? 0.0 : edges.back();
        row.MaxNumSubdiv = maxND;
        row.Overflow = instance.ReadWorkQueueCounter(kWQOverflowCounter);
        row.Bad = CountNonConformingEdges(tris);
        rows.push_back(row);

        std::printf("[tpe-cost] R=2000 alt=%.0fm TPE=%.1f -> live=%u (%.1f%% of pool) minEdge=%.2fm "
                    "medEdge=%.2fm maxEdge=%.1fm maxNumSubdiv=%u overflow=%d nonConforming=%u\n",
                    finalAlt, tpe, live, 100.0 * static_cast<double>(live) / kDefaultBisectorPoolSize,
                    row.MinEdge, row.MedEdge, row.MaxEdge, maxND, row.Overflow, row.Bad);

        EXPECT_EQ(row.Bad, 0u) << "cross-face T-junction at TPE=" << tpe << " (conformity must hold under any load)";
        EXPECT_EQ(instance.ReadValidationErrorCount(), 0u) << "validation error at TPE=" << tpe;
    }

    ASSERT_EQ(rows.size(), 4u); // [16, 8, 4, 2]
    // THE FINDING, POST reference-parity demand fix (this arc): the surface pose is NO LONGER
    // pool-frozen. Pre-fix the 131072-slot pool saturated at EVERY TPE with the facet median stuck
    // at the pool-limited ~33 m and tens of millions of dropped splits — the "TPE is not the lever,
    // the pool is" conclusion. The Classify visibility gate (coarsen the far hemisphere + off-frustum
    // to base) + the screen-space AREA metric (bound the visible demand, kill grazing-limb
    // inflation) removed that: at a coarse TPE the pool now has headroom (un-saturates) and the
    // median facet is metres, not the frozen floor, with detail that tracks TPE.
    const Row& coarse = rows.front(); // TPE = 16
    const Row& finest = rows.back();  // TPE = 2
    // Coarse TPE un-saturates the surface pose and drops ~no splits — the frozen floor is gone.
    EXPECT_LT(coarse.Live, kDefaultBisectorPoolSize * 4u / 5u)
        << "TPE 16 still saturates the pool at 150 m — the demand fix did not un-freeze the surface pose";
    EXPECT_LT(coarse.Overflow, static_cast<int>(kDefaultBisectorPoolSize))
        << "TPE 16 still drops splits steadily — the visibility/area demand cure is ineffective";
    // The median facet is metres (not the ~33 m frozen floor) at every TPE, and conformity holds.
    for (const Row& r : rows)
    {
        EXPECT_EQ(r.Bad, 0u) << "TPE " << r.Tpe << ": cross-face T-junction (conformity must hold)";
        EXPECT_LT(r.MedEdge, 6.0)
            << "TPE " << r.Tpe << ": median facet still near the old ~33 m frozen floor";
    }
    // Detail now tracks TPE: a finer TPE reaches a finer minimum facet (pre-fix the min was frozen too).
    EXPECT_LT(finest.MinEdge, coarse.MinEdge * 0.6)
        << "the minimum facet does not get finer with TPE — detail is still pool-frozen, not TPE-bound";
    std::printf("[tpe-cost] CONCLUSION (post reference-parity demand fix): surface pose no longer "
                "pool-frozen — TPE 16 un-saturates (%.1f%%, overflow %d) and median facet is %.2f m "
                "(was ~33 m frozen); min facet tracks TPE (%.2f m @2 vs %.2f m @16).\n",
                100.0 * static_cast<double>(coarse.Live) / kDefaultBisectorPoolSize, coarse.Overflow,
                coarse.MedEdge, finest.MinEdge, coarse.MinEdge);
}

// ---------------------------------------------------------------------------
// Clipmap / bisector-pool measurements. These are INSTRUMENTATION, not invariants —
// they print grep-taggable cost lines for a human to read off a run. They
// assert only cheap sanity bounds so they never flap under the noisy multi-agent
// GPU load on this box; the numbers themselves are read from stdout.
// ---------------------------------------------------------------------------

namespace
{
// Live-bisector scan on the whole pool: occupancy + the split-edge (corner0..corner2)
// length distribution + the deepest numSubdiv reached. Shared by both probes below so a
// pool-size change (a compile-time kDefaultBisectorPoolSize edit for the sweep) flows
// through one place.
struct PoolScan
{
    uint32_t Live = 0;
    uint32_t MaxNumSubdiv = 0;
    double MinEdge = 0.0, MedEdge = 0.0, MaxEdge = 0.0;
};

// DeepTag-aware world corner fetch (S2b): a legacy slot's Corner*.xyz IS the fp32 world
// position; a deep slot (DeepTag[0] == 1, S2a (sector, local) storage) holds the sector-
// local offset, and the world position is sector * 1024 + local — reconstructed in double
// (exact: the sector product is integer, |local| <= 512), so the pose scans measure true
// geometry in BOTH storage modes. Word offsets per CBTVertexData (locked by the layout
// static_asserts): corner0 @ 0, corner2 @ 8, Sector0 @ 16, Sector2 @ 20, DeepTag @ 22.
void FetchCornerWorld(const std::vector<uint32_t>& verts, uint32_t slotBase, uint32_t cornerOfs,
                      uint32_t sectorOfs, double out[3])
{
    float local[3];
    std::memcpy(local, &verts[slotBase + cornerOfs], sizeof(local));
    if (verts[slotBase + 22u] == 0u) // DeepTag[0]: legacy world-corner slot
    {
        out[0] = local[0];
        out[1] = local[1];
        out[2] = local[2];
        return;
    }
    const uint32_t w0 = verts[slotBase + sectorOfs];
    const uint32_t w1 = verts[slotBase + sectorOfs + 1u];
    out[0] = 1024.0 * static_cast<double>(UnpackSectorLo(w0)) + static_cast<double>(local[0]);
    out[1] = 1024.0 * static_cast<double>(UnpackSectorHi(w0)) + static_cast<double>(local[1]);
    out[2] = 1024.0 * static_cast<double>(UnpackSectorLo(w1)) + static_cast<double>(local[2]);
}

PoolScan ScanLivePool(CBTInstance& instance)
{
    const auto heap = instance.DebugReadWords(CBTBinding::HeapID, kDefaultBisectorPoolSize * 2u);
    const auto verts = instance.DebugReadWords(CBTBinding::CurrentVertex,
                                                     kDefaultBisectorPoolSize * kVertexWordsPerSlot);
    std::vector<double> edges;
    edges.reserve(8192);
    PoolScan s{};
    for (uint32_t slot = 0; slot < kDefaultBisectorPoolSize; ++slot)
    {
        if (heap[slot * 2u] == 0u && heap[slot * 2u + 1u] == 0u)
            continue;
        const uint64_t hid =
            static_cast<uint64_t>(heap[slot * 2u]) | (static_cast<uint64_t>(heap[slot * 2u + 1u]) << 32);
        s.MaxNumSubdiv = std::max(s.MaxNumSubdiv, HeapDepth(hid) - kSphereBaseDepth);
        const uint32_t base = slot * kVertexWordsPerSlot;
        double c0[3], c2[3];
        FetchCornerWorld(verts, base, 0u, 16u, c0);
        FetchCornerWorld(verts, base, 8u, 20u, c2); // corner2 — the LEB split edge is (c0,c2)
        const double dx = c0[0] - c2[0], dy = c0[1] - c2[1], dz = c0[2] - c2[2];
        const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
        ++s.Live; // occupancy counts every live slot
        // Skip just-split bisectors whose CurrentVertex is still zero (edge == 0) — VertexEval has
        // not run on them yet, so they are unevaluated transients, not real geometry. Including them
        // drags the median to 0 at a churning low-altitude pose (they are excluded from occupancy
        // reporting only for the facet-size distribution).
        if (len > 0.0)
            edges.push_back(len);
    }
    std::sort(edges.begin(), edges.end());
    s.MinEdge = edges.empty() ? 0.0 : edges.front();
    s.MedEdge = edges.empty() ? 0.0 : edges[edges.size() / 2u];
    s.MaxEdge = edges.empty() ? 0.0 : edges.back();
    return s;
}

// Live-bisector scan RESTRICTED to a near disc: only bisectors whose split-edge midpoint is
// within `discRadius` metres of `center` (world space). The near-patch median is the S2 gate's
// falsifiable datum — the view-priority metric must drive it far below the frozen whole-pool
// median near the camera while leaving the pool saturated (design §6 gate 1).
PoolScan ScanNearPatch(CBTInstance& instance, const std::array<double, 3>& center, double discRadius)
{
    const auto heap = instance.DebugReadWords(CBTBinding::HeapID, kDefaultBisectorPoolSize * 2u);
    const auto verts = instance.DebugReadWords(CBTBinding::CurrentVertex,
                                                     kDefaultBisectorPoolSize * kVertexWordsPerSlot);
    std::vector<double> edges;
    edges.reserve(8192);
    PoolScan s{};
    const double r2 = discRadius * discRadius;
    for (uint32_t slot = 0; slot < kDefaultBisectorPoolSize; ++slot)
    {
        if (heap[slot * 2u] == 0u && heap[slot * 2u + 1u] == 0u)
            continue;
        const uint32_t base = slot * kVertexWordsPerSlot;
        double c0[3], c2[3];
        FetchCornerWorld(verts, base, 0u, 16u, c0);
        FetchCornerWorld(verts, base, 8u, 20u, c2); // corner2 — the LEB split edge is (c0,c2)
        const double mx = 0.5 * (c0[0] + c2[0]);
        const double my = 0.5 * (c0[1] + c2[1]);
        const double mz = 0.5 * (c0[2] + c2[2]);
        const double dx = mx - center[0], dy = my - center[1], dz = mz - center[2];
        if (dx * dx + dy * dy + dz * dz > r2)
            continue;
        const double ex = c0[0] - c2[0];
        const double ey = c0[1] - c2[1];
        const double ez = c0[2] - c2[2];
        const double len = std::sqrt(ex * ex + ey * ey + ez * ez);
        const uint64_t hid =
            static_cast<uint64_t>(heap[slot * 2u]) | (static_cast<uint64_t>(heap[slot * 2u + 1u]) << 32);
        s.MaxNumSubdiv = std::max(s.MaxNumSubdiv, HeapDepth(hid) - kSphereBaseDepth);
        ++s.Live;
        if (len > 0.0) // skip unevaluated (zero-vertex) transients from the facet-size distribution
            edges.push_back(len);
    }
    std::sort(edges.begin(), edges.end());
    s.MinEdge = edges.empty() ? 0.0 : edges.front();
    s.MedEdge = edges.empty() ? 0.0 : edges[edges.size() / 2u];
    s.MaxEdge = edges.empty() ? 0.0 : edges.back();
    return s;
}
} // namespace

std::vector<DescentRow> CBTSphereDomainTest::RunAltitudeDescent(float radius,
                                                               const std::vector<float>& checkpoints,
                                                               const char* tag, const ProbeConfig& cfg)
{
    using namespace GameEngine::Mathematics;
    CBTInstance instance;
    EXPECT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    EXPECT_TRUE(instance.InitializeRoots(kDomainSpherical));

    const float amp = 60.0f, freq = 6.0f; // the shipped default planet relief
    const float startAlt = checkpoints.front();
    // The frustum test rejects geometry beyond the far plane, so far must contain the whole planet
    // as seen from orbit: cam sits at radius+startAlt from the centre, the far limb at 2*radius+alt.
    const float farPlane = 3.0f * radius + 2.0f * startAlt;
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(1.2f, 16.0f / 9.0f, 0.05f, farPlane);
    const uint32_t cap = kSphereBaseDepth + std::min(cfg.CapSubdiv, kMaxDecodeSubdiv);

    CBTFrameParams p = SphereParams(radius, amp, freq);
    p.PlanetParams[3] = 4.0f; // octaves (the shipped default)
    p.Screen[0] = 1600.0f;
    p.Screen[1] = 900.0f;
    p.Screen[2] = 8.0f; // the shipped default TPE (split threshold px)
    p.Screen[3] = 4.0f; // merge threshold
    p.TerrainOrigin[2] = static_cast<float>(cap);
    ApplyDemandTuning(p, cfg);

    CBTClassifyDesc classify{};
    classify.Mode = kClassifyScreenSpace;
    classify.TargetDepth = cap;

    // Surface point directly under the camera (camera on -Z looking at the centre): the near disc
    // "detail under the feet" median is measured about it.
    const std::array<double, 3> nadir = {0.0, 0.0, -static_cast<double>(radius)};

    std::vector<DescentRow> rows;
    size_t nextCp = 0;
    for (uint32_t f = 0; f < 700u && nextCp < checkpoints.size(); ++f)
    {
        const float alt = std::max(4.0f, startAlt * std::pow(0.97f, static_cast<float>(f)));
        const Vector3 eye(0.0f, 0.0f, -(radius + alt));
        const Matrix4x4 vp =
            proj * MakeLookAtLH(eye, Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f));
        std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
        p.CameraPos[0] = eye.x; p.CameraPos[1] = eye.y; p.CameraPos[2] = eye.z; p.CameraPos[3] = 1.0f;
        RunFrame(instance, classify, p, f);

        if (alt <= checkpoints[nextCp])
        {
            const int dropBefore = instance.ReadWorkQueueCounter(kWQOverflowCounter);
            for (uint32_t s = 0; s < 6u; ++s)
                RunFrame(instance, classify, p, 1000000u + f * 8u + s);
            const int dropAfter = instance.ReadWorkQueueCounter(kWQOverflowCounter);
            const int splitDemand = instance.ReadWorkQueueCounter(kWQSplitCounter);
            const PoolScan whole = ScanLivePool(instance);
            const double nearDisc = std::max(20.0, static_cast<double>(alt));
            const PoolScan nearScan = ScanNearPatch(instance, nadir, nearDisc);
            const double occ = 100.0 * static_cast<double>(whole.Live) / kDefaultBisectorPoolSize;
            rows.push_back({alt, whole.Live, occ, whole.MedEdge, nearScan.MedEdge, whole.MinEdge,
                            whole.MaxNumSubdiv, splitDemand, dropAfter - dropBefore});
            std::printf("[%s] R=%.0f alt=%.0fm live=%u occ=%.1f%% wholeMed=%.3fm nearMed(<%.0fm)=%.3fm "
                        "minEdge=%.3fm maxND=%u splitDemand/frame=%d droppedOverSettle=%d poolSize=%u\n",
                        tag, radius, alt, whole.Live, occ, whole.MedEdge, nearDisc, nearScan.MedEdge,
                        whole.MinEdge, whole.MaxNumSubdiv, splitDemand, dropAfter - dropBefore,
                        kDefaultBisectorPoolSize);
            ++nextCp;
        }
    }
    return rows;
}

CostRow CBTSphereDomainTest::RunCostProbe(float radius, float finalAlt, const char* tag,
                                          const ProbeConfig& cfg)
{
    using namespace GameEngine::Mathematics;
    CBTInstance instance;
    EXPECT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    EXPECT_TRUE(instance.InitializeRoots(kDomainSpherical));
    instance.SetValidateEachUpdate(cfg.ValidateEachUpdate); // S3: measure the ship idle floor

    const float amp = 60.0f, freq = 6.0f;
    const float startAlt = std::max(300.0f, finalAlt * 2.0f);
    const float farPlane = 3.0f * radius + 2.0f * startAlt;
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(1.2f, 16.0f / 9.0f, 0.05f, farPlane);
    const uint32_t cap = kSphereBaseDepth + std::min(cfg.CapSubdiv, kMaxDecodeSubdiv);

    CBTFrameParams p = SphereParams(radius, amp, freq);
    p.PlanetParams[3] = 4.0f;
    p.Screen[0] = 1600.0f;
    p.Screen[1] = 900.0f;
    p.Screen[2] = 8.0f;
    p.Screen[3] = 4.0f;
    p.TerrainOrigin[2] = static_cast<float>(cap);
    ApplyDemandTuning(p, cfg);

    CBTClassifyDesc classify{};
    classify.Mode = kClassifyScreenSpace;
    classify.TargetDepth = cap;

    auto poseAt = [&](float alt) {
        const Vector3 eye(0.0f, 0.0f, -(radius + alt));
        const Matrix4x4 vp =
            proj * MakeLookAtLH(eye, Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f));
        std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
        p.CameraPos[0] = eye.x; p.CameraPos[1] = eye.y; p.CameraPos[2] = eye.z; p.CameraPos[3] = 1.0f;
    };

    // Graded descent startAlt -> finalAlt, timing the tail as the CHANGING-frame cost.
    constexpr uint32_t kDescend = 160u;
    double changingMinMs = 1e30;
    for (uint32_t f = 0; f < kDescend; ++f)
    {
        poseAt(std::max(finalAlt, startAlt * std::pow(0.96f, static_cast<float>(f))));
        if (f >= kDescend - 30u)
        {
            auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
            cl->Begin();
            instance.RecordUpdate(*cl, classify, p, f);
            cl->End();
            std::vector<CommandList*> lists{cl.get()};
            const auto t0 = std::chrono::high_resolution_clock::now();
            m_Device->ExecuteCommandLists(lists);
            m_Device->WaitForIdle();
            const auto t1 = std::chrono::high_resolution_clock::now();
            changingMinMs =
                std::min(changingMinMs, std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        else
        {
            RunFrame(instance, classify, p, f);
        }
    }

    // Settle at the final pose (splits/merges drain to ~0).
    poseAt(finalAlt);
    for (uint32_t f = 0; f < 40u; ++f)
        RunFrame(instance, classify, p, kDescend + f);

    // QUIESCENT wall-clock (min-of-N, least-contended sample) + steady-state dropped splits.
    const int dropBefore = instance.ReadWorkQueueCounter(kWQOverflowCounter);
    // S3: the timed quiescent frames optionally run the production VertexEval quiescence gate
    // (re-evaluate only MODIFIED bisectors, ~0 on a converged frame) — the ship idle floor.
    CBTClassifyDesc timed = classify;
    timed.GateVertexEval = cfg.GateTimedFrames ? 1u : 0u;
    constexpr uint32_t kTimed = 40u;
    double quiescentMinMs = 1e30, quiescentSumMs = 0.0;
    for (uint32_t f = 0; f < kTimed; ++f)
    {
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        instance.RecordUpdate(*cl, timed, p, kDescend + 40u + f);
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        const auto t0 = std::chrono::high_resolution_clock::now();
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();
        const auto t1 = std::chrono::high_resolution_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        quiescentMinMs = std::min(quiescentMinMs, ms);
        quiescentSumMs += ms;
    }
    const int dropAfter = instance.ReadWorkQueueCounter(kWQOverflowCounter);

    const PoolScan whole = ScanLivePool(instance);
    const double nearDisc = std::max(20.0, static_cast<double>(finalAlt));
    const std::array<double, 3> nadir = {0.0, 0.0, -static_cast<double>(radius)};
    const PoolScan nearScan = ScanNearPatch(instance, nadir, nearDisc);
    const std::vector<CubeTri> tris = ReadLiveCubeTris(instance, kDefaultBisectorPoolSize);

    CostRow row{};
    row.Radius = radius;
    row.Alt = finalAlt;
    row.Live = whole.Live;
    row.Occ = 100.0 * static_cast<double>(whole.Live) / kDefaultBisectorPoolSize;
    row.MinEdge = whole.MinEdge;
    row.WholeMed = whole.MedEdge;
    row.NearMed = nearScan.MedEdge;
    row.MaxEdge = whole.MaxEdge;
    row.MaxND = whole.MaxNumSubdiv;
    row.Overflow = instance.ReadWorkQueueCounter(kWQOverflowCounter);
    row.DroppedOverQuiescent = dropAfter - dropBefore;
    row.QuiescentMinMs = quiescentMinMs;
    row.QuiescentMeanMs = quiescentSumMs / kTimed;
    row.ChangingMinMs = changingMinMs;
    row.Bad = CountNonConformingEdges(tris);

    std::printf("[%s] R=%.0f alt=%.0fm live=%u occ=%.1f%% minEdge=%.3fm wholeMed=%.3fm "
                "nearMed(<%.0fm)=%.3fm maxEdge=%.1fm maxND=%u overflow=%d droppedOverQuiescent=%d "
                "nonConf=%u | quiescentMs(min)=%.3f quiescentMs(mean)=%.3f changingMs(min)=%.3f "
                "poolSize=%u\n",
                tag, radius, finalAlt, whole.Live, row.Occ, whole.MinEdge, whole.MedEdge, nearDisc,
                nearScan.MedEdge, whole.MaxEdge, whole.MaxNumSubdiv, row.Overflow,
                row.DroppedOverQuiescent, row.Bad, quiescentMinMs, quiescentSumMs / kTimed,
                changingMinMs, kDefaultBisectorPoolSize);

    EXPECT_EQ(instance.ReadValidationErrorCount(), 0u) << "validation error at the probe pose (R=" << radius << ")";
    return row;
}

namespace
{
// Near sphere-shell intersection of a normalised ray (origin outside the shell), for placing the
// grazing measurement discs on the visible ground.
GameEngine::Mathematics::Vector3 SphereHit(const GameEngine::Mathematics::Vector3& origin,
                                           const GameEngine::Mathematics::Vector3& dir, float radius)
{
    using namespace GameEngine::Mathematics;
    const float b = 2.0f * Vector3::Dot(origin, dir);
    const float c = Vector3::Dot(origin, origin) - radius * radius;
    const float disc = b * b - 4.0f * c; // ray dir is unit, a == 1
    if (disc < 0.0f)
        return origin + dir * (radius * 2.0f); // no hit (grazing miss) — unused fallback
    const float sqrtD = std::sqrt(disc);
    float s = (-b - sqrtD) * 0.5f;
    if (s < 0.0f)
        s = (-b + sqrtD) * 0.5f;
    return origin + dir * s;
}
} // namespace

GrazingRow CBTSphereDomainTest::RunGrazingProbe(float radius, float alt, const char* tag,
                                                const ProbeConfig& cfg)
{
    using namespace GameEngine::Mathematics;
    CBTInstance instance;
    EXPECT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    EXPECT_TRUE(instance.InitializeRoots(kDomainSpherical));

    // Relief amp 0 => a clean sphere (matches the user's flatten-modifier repro and removes relief
    // displacement from the near-disc placement). Grazing look: forward along the +X tangent, pitched
    // theta below horizontal so the ground recedes to the horizon — the walker's view, NOT top-down.
    const float startAlt = std::max(alt * 6.0f, 60.0f);
    const float farPlane = 3.0f * radius + 2.0f * startAlt;
    const float fovY = 1.2f;
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(fovY, 16.0f / 9.0f, 0.05f, farPlane);
    const uint32_t cap = kSphereBaseDepth + std::min(cfg.CapSubdiv, kMaxDecodeSubdiv);
    constexpr float kGrazePitch = 0.03f; // ~1.7 deg below horizontal — a true grazing walk look

    CBTFrameParams p = SphereParams(radius, 0.0f, 5.0f);
    p.PlanetParams[3] = 4.0f;
    p.Screen[0] = 1600.0f;
    p.Screen[1] = 900.0f;
    p.Screen[2] = 8.0f;
    p.Screen[3] = 4.0f;
    p.TerrainOrigin[2] = static_cast<float>(cap);
    ApplyDemandTuning(p, cfg);

    CBTClassifyDesc classify{};
    classify.Mode = kClassifyScreenSpace;
    classify.TargetDepth = cap;

    const Vector3 upLocal(0.0f, 0.0f, -1.0f); // radial outward at the nadir surface point
    const Vector3 tangent(1.0f, 0.0f, 0.0f);  // +X walk direction
    // Grazing look dir at pitch theta below the tangent horizontal (toward the planet is +Z here).
    const Vector3 dirCenter =
        (tangent * std::cos(kGrazePitch) + Vector3(0.0f, 0.0f, 1.0f) * std::sin(kGrazePitch)).Normalize();

    auto poseAt = [&](float a) {
        const Vector3 eye(0.0f, 0.0f, -(radius + a));
        const Matrix4x4 vp = proj * MakeLookAtLH(eye, eye + dirCenter, upLocal);
        std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
        p.CameraPos[0] = eye.x; p.CameraPos[1] = eye.y; p.CameraPos[2] = eye.z; p.CameraPos[3] = 1.0f;
    };

    // Graded descent startAlt -> alt (a cold grazing close-up mis-saturates breadth-first), then hold.
    constexpr uint32_t kApproach = 140u;
    for (uint32_t f = 0; f < kApproach; ++f)
        poseAt(std::max(alt, startAlt * std::pow(0.95f, static_cast<float>(f)))), RunFrame(instance, classify, p, f);
    poseAt(alt);
    const int dropBefore = instance.ReadWorkQueueCounter(kWQOverflowCounter);
    for (uint32_t f = 0; f < 60u; ++f)
        RunFrame(instance, classify, p, kApproach + f);
    const int dropAfter = instance.ReadWorkQueueCounter(kWQOverflowCounter);
    const int splitDemand = instance.ReadWorkQueueCounter(kWQSplitCounter);

    const Vector3 eye(0.0f, 0.0f, -(radius + alt));
    // Two fixed ground points straight ahead along the walk tangent: a NEAR one (the ground right in
    // front of the walker, viewed at grazing incidence — where the area metric starves) and a MID one
    // (farther ahead). Inverted LOD (FeetMed >> AheadMed) is the grazing-starvation signature: the
    // ground gets COARSER the closer it is, because approaching it drives its incidence toward 90 and
    // the projected area collapses.
    constexpr double kNearAheadM = 12.0;
    constexpr double kMidAheadM = 60.0;
    auto groundAhead = [&](double dArc) {
        const Vector3 s = (Vector3(static_cast<float>(dArc), 0.0f, 0.0f) + Vector3(0.0f, 0.0f, -radius)).Normalize() * radius;
        return std::array<double, 3>{s.x, s.y, s.z};
    };
    const std::array<double, 3> feetC = groundAhead(kNearAheadM);
    const std::array<double, 3> aheadC = groundAhead(kMidAheadM);
    const Vector3 pFeet(static_cast<float>(feetC[0]), static_cast<float>(feetC[1]), static_cast<float>(feetC[2]));
    const Vector3 viewToFeet = (pFeet - eye).Normalize();
    const Vector3 feetNormal = pFeet.Normalize();
    const double feetIncidenceDeg =
        std::acos(std::min(1.0, std::abs(static_cast<double>(Vector3::Dot(viewToFeet, feetNormal))))) * 180.0 / kPiConst;
    const double feetDisc = std::max(0.4 * kNearAheadM, 6.0);
    const double aheadDisc = std::max(0.4 * kMidAheadM, 10.0);

    const PoolScan whole = ScanLivePool(instance);
    const PoolScan ahead = ScanNearPatch(instance, aheadC, aheadDisc);
    const PoolScan feet = ScanNearPatch(instance, feetC, feetDisc);
    const std::vector<CubeTri> tris = ReadLiveCubeTris(instance, kDefaultBisectorPoolSize);

    GrazingRow row{};
    row.Radius = radius;
    row.Alt = alt;
    row.Live = whole.Live;
    row.Occ = 100.0 * static_cast<double>(whole.Live) / kDefaultBisectorPoolSize;
    row.WholeMed = whole.MedEdge;
    row.AheadMed = ahead.MedEdge;
    row.FeetMed = feet.MedEdge;
    row.FeetMin = feet.MinEdge;
    row.MaxND = whole.MaxNumSubdiv;
    row.SplitDemand = splitDemand;
    row.DroppedOverSettle = dropAfter - dropBefore;
    row.Bad = CountNonConformingEdges(tris);

    std::printf("[%s] R=%.0f alt=%.1fm GRAZING(incid=%.1fdeg) live=%u occ=%.1f%% wholeMed=%.3fm "
                "feetMed(%.0fm ahead)=%.3fm feetLive=%u midMed(%.0fm)=%.3fm midLive=%u feetMin=%.3fm "
                "maxND=%u splitDemand/frame=%d droppedOverSettle=%d nonConf=%u poolSize=%u\n",
                tag, radius, alt, feetIncidenceDeg, whole.Live, row.Occ, whole.MedEdge, kNearAheadM,
                feet.MedEdge, feet.Live, kMidAheadM, ahead.MedEdge, ahead.Live, feet.MinEdge,
                whole.MaxNumSubdiv, splitDemand, row.DroppedOverSettle, row.Bad, kDefaultBisectorPoolSize);

    EXPECT_EQ(instance.ReadValidationErrorCount(), 0u) << "validation error at the grazing pose (R=" << radius << ")";
    return row;
}

RotationRow CBTSphereDomainTest::RunRotationChurn(float radius, float alt, const char* tag,
                                                  const ProbeConfig& cfg)
{
    using namespace GameEngine::Mathematics;
    CBTInstance instance;
    EXPECT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    EXPECT_TRUE(instance.InitializeRoots(kDomainSpherical));

    const float startAlt = std::max(alt * 6.0f, 60.0f);
    const float farPlane = 3.0f * radius + 2.0f * startAlt;
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(1.2f, 16.0f / 9.0f, 0.05f, farPlane);
    const uint32_t cap = kSphereBaseDepth + std::min(cfg.CapSubdiv, kMaxDecodeSubdiv);
    constexpr float kGrazePitch = 0.14f;

    CBTFrameParams p = SphereParams(radius, 0.0f, 5.0f);
    p.PlanetParams[3] = 4.0f;
    p.Screen[0] = 1600.0f; p.Screen[1] = 900.0f;
    p.Screen[2] = 8.0f; p.Screen[3] = 4.0f;
    p.TerrainOrigin[2] = static_cast<float>(cap);
    ApplyDemandTuning(p, cfg);

    CBTClassifyDesc classify{};
    classify.Mode = kClassifyScreenSpace;
    classify.TargetDepth = cap;

    const Vector3 eye(0.0f, 0.0f, -(radius + alt));
    const Vector3 upLocal(0.0f, 0.0f, -1.0f);
    auto lookYaw = [&](float yaw, float a) {
        const Vector3 e(0.0f, 0.0f, -(radius + a));
        const Vector3 t(std::cos(yaw), std::sin(yaw), 0.0f); // tangent rotated about the local up (Z)
        const Vector3 d = (t * std::cos(kGrazePitch) + Vector3(0.0f, 0.0f, 1.0f) * std::sin(kGrazePitch)).Normalize();
        const Matrix4x4 vp = proj * MakeLookAtLH(e, e + d, upLocal);
        std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
        p.CameraPos[0] = e.x; p.CameraPos[1] = e.y; p.CameraPos[2] = e.z; p.CameraPos[3] = 1.0f;
    };

    // Graded grazing descent to alt, yaw 0, then hold to converge.
    constexpr uint32_t kApproach = 140u;
    for (uint32_t f = 0; f < kApproach; ++f)
        lookYaw(0.0f, std::max(alt, startAlt * std::pow(0.95f, static_cast<float>(f)))), RunFrame(instance, classify, p, f);
    lookYaw(0.0f, alt);
    for (uint32_t f = 0; f < 50u; ++f)
        RunFrame(instance, classify, p, kApproach + f);

    // Steady yaw sweep: record per-frame split + simplify (merge) demand. A frustum-gate that
    // coarsens off-frustum geometry to base spikes both as bisectors sweep through the NDC margin.
    constexpr uint32_t kYawFrames = 30u;
    constexpr float kYawStep = 0.05f; // ~2.9 deg / frame
    double yawSplitSum = 0.0, yawSimpSum = 0.0, yawSplitMax = 0.0, yawSimpMax = 0.0;
    const int yawDropBefore = instance.ReadWorkQueueCounter(kWQOverflowCounter);
    for (uint32_t f = 0; f < kYawFrames; ++f)
    {
        lookYaw(kYawStep * static_cast<float>(f + 1u), alt);
        RunFrame(instance, classify, p, kApproach + 50u + f);
        const double sp = instance.ReadWorkQueueCounter(kWQSplitCounter);
        const double sm = instance.ReadWorkQueueCounter(kWQSimplifyClassCounter);
        yawSplitSum += sp; yawSimpSum += sm;
        yawSplitMax = std::max(yawSplitMax, sp); yawSimpMax = std::max(yawSimpMax, sm);
    }
    const int yawDropAfter = instance.ReadWorkQueueCounter(kWQOverflowCounter);

    // Re-settle at the final yaw orientation, then a forward-translation control (the user's
    // "forward/back is mostly OK"): translate the camera along its tangent, same frame count.
    const float finalYaw = kYawStep * static_cast<float>(kYawFrames);
    for (uint32_t f = 0; f < 40u; ++f)
        RunFrame(instance, classify, p, kApproach + 200u + f);
    const Vector3 tFwd(std::cos(finalYaw), std::sin(finalYaw), 0.0f);
    double transSplitSum = 0.0, transSimpSum = 0.0;
    for (uint32_t f = 0; f < kYawFrames; ++f)
    {
        // Move forward along the tangent by ~half a camera-height/frame (a brisk walk), holding alt.
        const float step = std::max(alt * 0.5f, 2.0f) * static_cast<float>(f + 1u);
        const Vector3 e = eye + tFwd * step;
        const Vector3 d = (tFwd * std::cos(kGrazePitch) + Vector3(0.0f, 0.0f, 1.0f) * std::sin(kGrazePitch)).Normalize();
        const Matrix4x4 vp = proj * MakeLookAtLH(e, e + d, upLocal);
        std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
        p.CameraPos[0] = e.x; p.CameraPos[1] = e.y; p.CameraPos[2] = e.z; p.CameraPos[3] = 1.0f;
        RunFrame(instance, classify, p, kApproach + 300u + f);
        transSplitSum += instance.ReadWorkQueueCounter(kWQSplitCounter);
        transSimpSum += instance.ReadWorkQueueCounter(kWQSimplifyClassCounter);
    }

    const std::vector<CubeTri> tris = ReadLiveCubeTris(instance, kDefaultBisectorPoolSize);
    const PoolScan whole = ScanLivePool(instance);

    RotationRow row{};
    row.Radius = radius;
    row.Alt = alt;
    row.YawSplitMean = yawSplitSum / kYawFrames;
    row.YawSplitMax = yawSplitMax;
    row.YawSimplifyMean = yawSimpSum / kYawFrames;
    row.YawSimplifyMax = yawSimpMax;
    row.TransSplitMean = transSplitSum / kYawFrames;
    row.TransSimplifyMean = transSimpSum / kYawFrames;
    row.YawDropped = yawDropAfter - yawDropBefore;
    row.Occ = 100.0 * static_cast<double>(whole.Live) / kDefaultBisectorPoolSize;
    row.Bad = CountNonConformingEdges(tris);

    std::printf("[%s] R=%.0f alt=%.1fm ROTCHURN occ=%.1f%% yawSplit(mean/max)=%.0f/%.0f yawSimplify(mean/max)=%.0f/%.0f "
                "| transSplit(mean)=%.0f transSimplify(mean)=%.0f | yawDropped=%d nonConf=%u\n",
                tag, radius, alt, row.Occ, row.YawSplitMean, row.YawSplitMax, row.YawSimplifyMean, row.YawSimplifyMax,
                row.TransSplitMean, row.TransSimplifyMean, row.YawDropped, row.Bad);

    EXPECT_EQ(instance.ReadValidationErrorCount(), 0u) << "validation error after the rotation sweep (R=" << radius << ")";
    return row;
}

// ARC MEASUREMENT 1 — pool occupancy vs camera altitude on a full R=2000 planet at the
// default relief. One continuous exponential descent from ~8 km down to ~5 m over the
// surface (so the mesh stays graded — a cold jump grows breadth-first and mis-saturates,
// the scope note on the cost-curve test), capturing (live, occupancy%, median split edge,
// maxNumSubdiv) as the camera passes a set of altitude checkpoints. Answers "where does
// saturation begin?" — the datum Option C (view-prioritised slots) and Option B (near-field
// clipmap) both hinge on. Prints [pool-alt] lines; asserts only that saturation is monotone
// (occupancy never decreases as we descend) and that the high-altitude pose is NOT saturated.
TEST_F(CBTSphereDomainTest, PoolOccupancyVsAltitude)
{
    const std::vector<float> checkpoints = {8000.0f, 4000.0f, 2000.0f, 1000.0f, 500.0f, 200.0f,
                                            150.0f,  100.0f,  49.0f,   20.0f,   5.0f};
    const std::vector<DescentRow> rows = RunAltitudeDescent(2000.0f, checkpoints, "pool-alt");

    ASSERT_GE(rows.size(), 4u) << "descent captured too few altitude checkpoints";
    // The top-of-descent pose is not saturated — there is an altitude band with pool headroom.
    EXPECT_LT(rows.front().Occ, 95.0)
        << "even the 8 km pose saturates the pool — saturation is altitude-independent (surprising)";
    // CURED behavior (reference-parity demand fix): the frozen ~33.5 m facet floor is GONE. Pre-fix
    // every altitude <= ~1 km was pinned at 100% occupancy with a 33.5 m median (the visibility-
    // ungated metric refined the whole sphere and saturated the pool); now the visibility gate
    // coarsens the far hemisphere to base, so the freed pool refines the visible near field and the
    // median facet keeps shrinking as the camera descends — reaching the product bar at walking
    // eye height.
    const DescentRow& ground = rows.back(); // finest altitude checkpoint (~5 m — walking eye height)
    EXPECT_LT(ground.Alt, 10.0f) << "descent did not reach walking eye height";
    EXPECT_LT(ground.WholeMed, 0.5)
        << "walking-height median facet must reach <= 0.5 m (product bar) — the frozen floor persists";
    EXPECT_LT(ground.WholeMed, rows.front().WholeMed)
        << "near-field detail did not increase on descent (frozen floor)";
    EXPECT_LT(ground.Occ, 98.0)
        << "ground pose still saturates the pool — the far-hemisphere cull did not free the budget";
    EXPECT_LE(ground.MaxND, kMaxDecodeSubdiv) << "deepest subdivision exceeded the int64 decode cap";
}

// ARC MEASUREMENT 2 — the pool-size sweep probe. Converges at a fixed 150 m surface pose
// (the shipped cost-curve pose) on a full R=2000 planet, then reports (a) occupancy + split-edge
// distribution and (b) a wall-clock cost for a QUIESCENT converged frame (record+submit+wait,
// min-of-N to shed the multi-agent GPU-load noise) and a CHANGING frame. Re-run after a
// compile-time kDefaultBisectorPoolSize / CBT_POOL_SIZE edit to sweep 128k/256k/512k/1M; the
// [pool-cost] line carries poolSize so the sweep rows self-identify. Asserts only that the frame
// completed and produced live geometry — the cost numbers are read from stdout.
TEST_F(CBTSphereDomainTest, PoolCostProbeAtSurfacePose)
{
    const CostRow row = RunCostProbe(2000.0f, 150.0f, "pool-cost");
    EXPECT_GT(row.Live, 1024u) << "surface pose produced no meaningful refinement";
    EXPECT_EQ(row.Bad, 0u) << "cross-face T-junction at the probe pose";
    EXPECT_LE(row.MaxND, kMaxDecodeSubdiv) << "deepest subdivision exceeded the int64 decode cap";
}

// Pool-scaling slice S3 — the SHIP idle floor at the 150 m R=2000 surface pose: whole-pool Validate
// dropped + the production VertexEval quiescence gate on, so the timed frames reflect what
// CBTRenderFeature runs, not the debug harness. This is the row the cost table quotes and the anchor
// the pool BUMP is compared against (idle at the bumped pool must stay at/below this — the S3 gate).
// The quiescentMs is min-of-N on a shared box; read the [prod-idle] print's poolSize field, and drive
// this test at the CURRENT pool size AND the bumped size to fill the before/after column.
TEST_F(CBTSphereDomainTest, ProductionIdleAtSurfacePose)
{
    ProbeConfig ship{};
    ship.ValidateEachUpdate = false; // ship: no whole-pool debug Validate
    ship.GateTimedFrames = true;     // ship: gated VertexEval on the quiescent frames
    const CostRow row = RunCostProbe(2000.0f, 150.0f, "prod-idle", ship);
    EXPECT_GT(row.Live, 1024u) << "surface pose produced no meaningful refinement";
    EXPECT_EQ(row.Bad, 0u) << "cross-face T-junction at the probe pose";
    EXPECT_LE(row.MaxND, kMaxDecodeSubdiv) << "deepest subdivision exceeded the int64 decode cap";
}

// ---------------------------------------------------------------------------
// Large-planet validation slice — R=20000 and R=50000 (design: the #598 reference-parity demand
// gate makes split demand screen-bounded, so a bigger planet — which shows a SMALLER fraction of
// its surface at a given altitude — should be no harder than the R=2000 anchor, and the reference
// implementation reaches centimetric on Earth-scale from this same 131072-slot pool). These probe
// the SAME descent/cost machinery as the R=2000 anchors at two larger radii. They print the
// grep-taggable [pool-alt-R*] / [pool-cost-R*] lines the three-radius tables are read from and
// assert only the correctness invariants that must hold at any radius (conformity, int64 decode
// cap, un-saturated orbit pose, live geometry). The product-bar / occupancy-trend / quiescence
// GATES are read from the printed numbers — instrumentation, not flap-prone invariants, matching
// the arc-measurement philosophy above.
// ---------------------------------------------------------------------------

TEST_F(CBTSphereDomainTest, PoolOccupancyVsAltitudeR20k)
{
    const std::vector<float> checkpoints = {40000.0f, 8000.0f, 4000.0f, 2000.0f, 1000.0f, 500.0f,
                                            200.0f,   150.0f,  100.0f,  49.0f,   20.0f,   5.0f};
    const std::vector<DescentRow> rows = RunAltitudeDescent(20000.0f, checkpoints, "pool-alt-R20k");
    ASSERT_GE(rows.size(), 6u) << "R=20000 descent captured too few checkpoints";
    EXPECT_LT(rows.front().Occ, 95.0) << "R=20000 orbit pose already saturates the pool";
    const DescentRow& ground = rows.back();
    EXPECT_LT(ground.Alt, 10.0f) << "R=20000 descent did not reach walking eye height";
    EXPECT_GT(ground.Live, 1024u) << "R=20000 ground pose produced no meaningful refinement";
    uint32_t maxND = 0;
    for (const DescentRow& r : rows)
        maxND = std::max(maxND, r.MaxND);
    EXPECT_LE(maxND, kMaxDecodeSubdiv) << "R=20000 deepest subdivision exceeded the int64 decode cap";
}

TEST_F(CBTSphereDomainTest, PoolOccupancyVsAltitudeR50k)
{
    const std::vector<float> checkpoints = {100000.0f, 20000.0f, 8000.0f, 4000.0f, 2000.0f, 1000.0f,
                                            500.0f,    200.0f,   150.0f,  100.0f,  49.0f,   20.0f, 5.0f};
    const std::vector<DescentRow> rows = RunAltitudeDescent(50000.0f, checkpoints, "pool-alt-R50k");
    ASSERT_GE(rows.size(), 6u) << "R=50000 descent captured too few checkpoints";
    EXPECT_LT(rows.front().Occ, 95.0) << "R=50000 orbit pose already saturates the pool";
    const DescentRow& ground = rows.back();
    EXPECT_LT(ground.Alt, 10.0f) << "R=50000 descent did not reach walking eye height";
    EXPECT_GT(ground.Live, 1024u) << "R=50000 ground pose produced no meaningful refinement";
    uint32_t maxND = 0;
    for (const DescentRow& r : rows)
        maxND = std::max(maxND, r.MaxND);
    EXPECT_LE(maxND, kMaxDecodeSubdiv) << "R=50000 deepest subdivision exceeded the int64 decode cap";
}

TEST_F(CBTSphereDomainTest, PoolCostProbeAtSurfacePoseR20k)
{
    const CostRow row = RunCostProbe(20000.0f, 150.0f, "pool-cost-R20k");
    EXPECT_GT(row.Live, 1024u) << "R=20000 surface pose produced no meaningful refinement";
    EXPECT_EQ(row.Bad, 0u) << "R=20000 cross-face T-junction at the probe pose";
    EXPECT_LE(row.MaxND, kMaxDecodeSubdiv) << "R=20000 deepest subdivision exceeded the int64 decode cap";
}

TEST_F(CBTSphereDomainTest, PoolCostProbeAtSurfacePoseR50k)
{
    const CostRow row = RunCostProbe(50000.0f, 150.0f, "pool-cost-R50k");
    EXPECT_GT(row.Live, 1024u) << "R=50000 surface pose produced no meaningful refinement";
    EXPECT_EQ(row.Bad, 0u) << "R=50000 cross-face T-junction at the probe pose";
    EXPECT_LE(row.MaxND, kMaxDecodeSubdiv) << "R=50000 deepest subdivision exceeded the int64 decode cap";
}

// ---------------------------------------------------------------------------
// Grazing-walk characterization (round-7 finding 2) — the WALKING-eye detail metric measured in a
// GRAZING view (camera 1.7-5 m looking ALONG the surface), the user's real workflow, vs the top-down
// probes that hide grazing effects. HONEST NEGATIVE captured here: on a bare SPHERE the near ground
// under a grazing walker sits at ~80 deg incidence (not the ~90 deg of a FLAT surface), so the
// projected-AREA metric still refines it fine (feetMed ~0.17-0.31 m <= 0.5 m). The inverted-LOD
// starvation the user reported ("tessellates more when away than close") needs the near-90-deg
// incidence of a flatten-modifier disc — it is flat-geometry-specific and reproduced in the editor,
// not on the procedural sphere. These tests therefore GUARD that the sphere near field stays <= 0.5 m
// (no regression) and print the near/mid facet profile the report reads.
// ---------------------------------------------------------------------------

TEST_F(CBTSphereDomainTest, GrazingWalkR2000)
{
    const GrazingRow row = RunGrazingProbe(2000.0f, 2.0f, "graze-R2000");
    EXPECT_GT(row.Live, 1024u) << "grazing pose produced no meaningful refinement";
    EXPECT_EQ(row.Bad, 0u) << "grazing pose broke cross-face conformity";
    EXPECT_LE(row.MaxND, kMaxDecodeSubdiv) << "grazing deepest subdivision exceeded the int64 decode cap";
    EXPECT_LT(row.FeetMed, 0.5)
        << "R=2000 at-feet facet exceeds the 0.5 m product bar in a grazing walk (near-field starvation)";
}

TEST_F(CBTSphereDomainTest, GrazingWalkR20k)
{
    const GrazingRow row = RunGrazingProbe(20000.0f, 2.0f, "graze-R20k");
    EXPECT_GT(row.Live, 1024u) << "grazing pose produced no meaningful refinement";
    EXPECT_EQ(row.Bad, 0u) << "grazing pose broke cross-face conformity";
    EXPECT_LE(row.MaxND, kMaxDecodeSubdiv) << "grazing deepest subdivision exceeded the int64 decode cap";
    EXPECT_LT(row.FeetMed, 0.5)
        << "R=20000 at-feet facet exceeds the 0.5 m product bar in a grazing walk (near-field starvation)";
}

TEST_F(CBTSphereDomainTest, GrazingWalkR50k)
{
    const GrazingRow row = RunGrazingProbe(50000.0f, 2.0f, "graze-R50k");
    EXPECT_GT(row.Live, 1024u) << "grazing pose produced no meaningful refinement";
    EXPECT_EQ(row.Bad, 0u) << "grazing pose broke cross-face conformity";
    EXPECT_LE(row.MaxND, kMaxDecodeSubdiv) << "grazing deepest subdivision exceeded the int64 decode cap";
    EXPECT_LT(row.FeetMed, 0.5)
        << "R=50000 at-feet facet exceeds the 0.5 m product bar in a grazing walk (near-field starvation)";
}

// Rotation-churn CHARACTERIZATION (round-7 finding 1) — the user's R=20000 rotation-jitter repro. A
// steady yaw sweeps bisectors through the #598 frustum demand gate's NDC margin; the gate coarsens
// off-frustum geometry to base and re-refines it one level/frame, so a yaw shows a large per-frame
// SIMPLIFY (merge-to-base) demand that a forward translate does not — the mechanism behind the
// "massive rotational jumps". This probe prints that signature (the report reads it); it asserts only
// conformity + that the sweep did measurable work, since the demand-gate fix is a separate verified
// slice (it trades the churn for pool-headroom retention and must not regress the #598 anchor).
TEST_F(CBTSphereDomainTest, RotationChurnR20k)
{
    const RotationRow row = RunRotationChurn(20000.0f, 5.0f, "rotchurn-R20k");
    EXPECT_EQ(row.Bad, 0u) << "rotation sweep broke cross-face conformity";
    EXPECT_GT(row.YawSplitMean + row.YawSimplifyMean, 0.0) << "the yaw sweep drove no reclassification work";
}

// ---------------------------------------------------------------------------
// Arc slice S2 — view-prioritised split metric (Option C) A/B oracles.
// ---------------------------------------------------------------------------

// GATE 1 (design §6, fails-before/passes-after): at the shipped 150 m / R=2000 surface pose,
// converge the SAME build twice — bias OFF then ON — with the pool UNCHANGED (131072). The near
// patch (a ~100 m disc around the camera's surface hit) median split edge must be the frozen
// whole-pool median (~37 m) with the bias OFF and drop below 5 m with it ON, while the pool stays
// saturated (the bias REDISTRIBUTES the fixed budget, it does not grow it). Overflow must not grow
// meaningfully (gate 2) and conformity must hold both ways (gate 3). Prints [view-priority] lines.
TEST_F(CBTSphereDomainTest, ViewPriorityNearPatchAtSurfacePose)
{
    using namespace GameEngine::Mathematics;
    const float radius = 2000.0f;
    const float amp = 60.0f, freq = 6.0f; // the default planet relief
    const float finalAlt = 150.0f;        // the shipped sculpt/inspect pose
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(1.2f, 16.0f / 9.0f, 0.05f, 20000.0f);
    const uint32_t cap = kSphereBaseDepth + kMaxDecodeSubdiv;
    // Surface hit directly under the camera (camera on -Z, looking at the centre): the mean-sphere
    // nadir point. The near disc is measured about it. 100 m per the design's ~100 m near disc.
    const std::array<double, 3> nadir = {0.0, 0.0, -static_cast<double>(radius)};
    constexpr double kNearDiscM = 100.0;

    // The instance (and its frame params) survive converge so gate 4 can time OFF and ON PAIRED —
    // alternating submissions in one wall-clock window — instead of two windows ~4 s apart.
    struct Result
    {
        PoolScan Near; PoolScan Whole; int Overflow; uint32_t Bad; uint32_t ValErr;
        std::unique_ptr<CBTInstance> Inst; CBTFrameParams Params{}; CBTClassifyDesc Classify{};
    };
    auto converge = [&](bool biasOn) -> Result {
        Result result{};
        result.Inst = std::make_unique<CBTInstance>();
        CBTInstance& instance = *result.Inst;
        EXPECT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
        EXPECT_TRUE(instance.InitializeRoots(kDomainSpherical));

        CBTFrameParams p = SphereParams(radius, amp, freq);
        p.PlanetParams[3] = 4.0f; // octaves (the shipped default)
        p.Screen[0] = 1600.0f; p.Screen[1] = 900.0f;
        p.Screen[2] = 8.0f;    // the shipped default TPE (split threshold px)
        p.Screen[3] = 4.0f;    // merge threshold
        p.TerrainOrigin[2] = static_cast<float>(cap);
        if (biasOn)
        {
            // Radii calibrated to this pose: the 100 m near disc (camera distance 150-180 m) sits
            // inside the near radius (base threshold, full detail); everything past the far radius
            // (well inside the ~750 m horizon) is fully coarsened, freeing the bulk of the pool for
            // the near patch.
            p.NearBias[0] = 1.0f;   // enabled
            p.NearBias[1] = 180.0f; // near radius (m)
            p.NearBias[2] = 500.0f; // far radius (m)
            p.NearBias[3] = 10.0f;  // max coarsen factor
        }

        CBTClassifyDesc classify{};
        classify.Mode = kClassifyScreenSpace;
        classify.TargetDepth = cap;

        auto poseAt = [&](float alt) {
            const Vector3 eye(0.0f, 0.0f, -(radius + alt));
            const Matrix4x4 vp =
                proj * MakeLookAtLH(eye, Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f));
            std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
            p.CameraPos[0] = eye.x; p.CameraPos[1] = eye.y; p.CameraPos[2] = eye.z; p.CameraPos[3] = 1.0f;
        };

        // Graded descent 300 -> 150 m (a cold close-up mis-saturates before it can grade), then hold
        // long enough for the near-bias redistribution equilibrium to settle.
        constexpr uint32_t kDescend = 160u;
        for (uint32_t f = 0; f < kDescend; ++f)
        {
            poseAt(std::max(finalAlt, 300.0f * std::pow(0.96f, static_cast<float>(f))));
            RunFrame(instance, classify, p, f);
        }
        poseAt(finalAlt);
        for (uint32_t f = 0; f < 64u; ++f)
            RunFrame(instance, classify, p, kDescend + f);

        result.Near = ScanNearPatch(instance, nadir, kNearDiscM);
        result.Whole = ScanLivePool(instance);
        result.Overflow = instance.ReadWorkQueueCounter(kWQOverflowCounter);
        const std::vector<CubeTri> tris = ReadLiveCubeTris(instance, kDefaultBisectorPoolSize);
        result.Bad = CountNonConformingEdges(tris);
        result.ValErr = instance.ReadValidationErrorCount();
        result.Params = p;
        result.Classify = classify;
        return result;
    };

    const Result off = converge(false);
    const Result on = converge(true);

    // Gate 4 — quiescent CBT.Update cost (record+submit+GPU+wait, min-of-N). PAIRED sampling
    // (2026-07-19 deflake): the pool metrics of this un-saturated pose are bit-deterministic — the
    // only load-sensitive quantity in the test was this probe, because OFF's 24 timing frames and
    // ON's 24 timing frames ran in two windows ~4 s apart, so a shared-GPU load burst covering one
    // window but not the other faked a 2x ratio. Both converged instances are kept alive and the
    // OFF/ON submissions ALTERNATE within one window (order swapping each iteration), so any load
    // burst lands on both distributions and the min-of-N ratio stays honest. Absolute ms is still
    // noisy; only the ON/OFF ratio is read. The bias adds a handful of Classify ALU + one sum-tree
    // read per invocation (only when enabled), dwarfed by the four whole-pool sweep kernels, so ON
    // must stay within noise of OFF.
    auto timeOneFrame = [&](const Result& r, uint32_t frame) -> double {
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        r.Inst->RecordUpdate(*cl, r.Classify, r.Params, frame);
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        const auto t0 = std::chrono::high_resolution_clock::now();
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();
        const auto t1 = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(t1 - t0).count();
    };
    constexpr uint32_t kTimingFrames = 24u;
    constexpr uint32_t kTimingFrameBase = 224u; // continues each instance's 160+64 converge frames
    double offQuiescentMinMs = 1e30, onQuiescentMinMs = 1e30;
    for (uint32_t f = 0; f < kTimingFrames; ++f)
    {
        const uint32_t frame = kTimingFrameBase + f;
        if ((f & 1u) == 0u)
        {
            offQuiescentMinMs = std::min(offQuiescentMinMs, timeOneFrame(off, frame));
            onQuiescentMinMs = std::min(onQuiescentMinMs, timeOneFrame(on, frame));
        }
        else
        {
            onQuiescentMinMs = std::min(onQuiescentMinMs, timeOneFrame(on, frame));
            offQuiescentMinMs = std::min(offQuiescentMinMs, timeOneFrame(off, frame));
        }
    }

    std::printf("[view-priority] OFF near(<%.0fm): live=%u medEdge=%.2fm maxND=%u | whole live=%u "
                "(%.1f%%) medEdge=%.2fm overflow=%d nonConf=%u | quiescentMinMs=%.3f\n",
                kNearDiscM, off.Near.Live, off.Near.MedEdge, off.Near.MaxNumSubdiv, off.Whole.Live,
                100.0 * off.Whole.Live / kDefaultBisectorPoolSize, off.Whole.MedEdge, off.Overflow,
                off.Bad, offQuiescentMinMs);
    std::printf("[view-priority] ON  near(<%.0fm): live=%u medEdge=%.2fm maxND=%u | whole live=%u "
                "(%.1f%%) medEdge=%.2fm overflow=%d nonConf=%u | quiescentMinMs=%.3f\n",
                kNearDiscM, on.Near.Live, on.Near.MedEdge, on.Near.MaxNumSubdiv, on.Whole.Live,
                100.0 * on.Whole.Live / kDefaultBisectorPoolSize, on.Whole.MedEdge, on.Overflow,
                on.Bad, onQuiescentMinMs);

    // The near disc must hold enough facets to make the median meaningful, both ways.
    ASSERT_GT(off.Near.Live, 16u) << "OFF near disc captured too few facets for a median";
    ASSERT_GT(on.Near.Live, 16u) << "ON near disc captured too few facets for a median";
    // Gate 3 — conformity holds under the priority weight (the CBT stays a conforming triangulation).
    EXPECT_EQ(off.Bad, 0u) << "baseline non-conforming (unrelated regression)";
    EXPECT_EQ(on.Bad, 0u) << "view-priority near-bias broke cross-face conformity (gate 3)";
    EXPECT_EQ(off.ValErr, 0u);
    EXPECT_EQ(on.ValErr, 0u) << "view-priority near-bias corrupted the neighbor links";
    // MIGRATED for the S3 pool-scaling bump. The 150 m R=2000 pose now UN-SATURATES — the demand fix
    // bounds the live set to ~168k and the bumped pool holds it with headroom — so the near patch is
    // refined to metres by the area metric + free slots alone (no redistribution needed), and S2's
    // near-bias is MOOT here: its contention ramp is occupancy-gated (CBT_NEAR_BIAS_CONTENTION_LO) and
    // never engages below the ramp start, so ON == OFF at the same un-saturated equilibrium (the
    // neutrality the arc predicted once the pool stops binding). S2 still redistributes at genuinely-
    // contended poses (TPE<=2, the sub-decode-cap walking eye, or the big-radius planets);
    // ViewPriorityUnsaturatedIsUnchanged pins the neutral case, this now pins the bump's un-saturation.
    EXPECT_LT(off.Near.MedEdge, 5.0)
        << "the demand fix + bump did not refine the near patch (regressed)";
    EXPECT_LT(on.Near.MedEdge, 5.0) << "the near patch is not fine with the bias on";
    // The bump un-saturated the pose: live is well below the pool both ways (the whole point of S3).
    EXPECT_LT(off.Whole.Live, kDefaultBisectorPoolSize * 3u / 4u)
        << "the 150 m pose did not un-saturate at the bumped pool (S3 bump did not take)";
    EXPECT_LT(on.Whole.Live, kDefaultBisectorPoolSize * 3u / 4u)
        << "the near-bias re-saturated the un-saturated pose";
    // S2 is neutral at an un-saturated pool (its ramp never engages): the whole-pool median and the
    // dropped-split count match ON vs OFF within noise, so the bias neither helps nor harms here.
    EXPECT_NEAR(on.Whole.MedEdge, off.Whole.MedEdge, 0.5)
        << "near-bias changed the whole-pool median at an un-saturated pose (ramp wrongly engaged)";
    EXPECT_LE(on.Overflow, static_cast<int>(off.Overflow * 1.2 + 200000))
        << "view-priority raised dropped-split overflow meaningfully (gate 2 failed)";
    // Gate 4 — quiescent CBT.Update cost within noise of the OFF baseline (the added Classify work
    // is negligible next to the whole-pool sweep kernels). Loose bound: even paired min-of-N is noisy
    // on this shared box, so this only catches a gross regression (healthy paired ratio ~1.0, a real
    // gate-4 regression is the bias roughly doubling the update), not fine differences.
    EXPECT_LT(onQuiescentMinMs, offQuiescentMinMs * 2.0 + 1.0)
        << "view-priority roughly doubled the quiescent CBT.Update cost (gate 4 — likely a real "
           "regression, not box noise; OFF=" << offQuiescentMinMs << "ms ON=" << onQuiescentMinMs << "ms)";
}

// NEUTRALITY oracle (design §6, "unchanged when unsaturated"): at a clearly UNSATURATED pose the
// in-shader occupancy gate holds the bias fully off, so an enabled frame classifies exactly as a
// disabled one. Converge the same descent to ~4000 m (a ~33% pool at R=2000) OFF then ON and assert
// the live count + whole-pool median are identical (contention == 0 => coarsen == 1.0, an IEEE
// no-op). This is itself an oracle — an always-on bias would coarsen the far field here and diverge.
TEST_F(CBTSphereDomainTest, ViewPriorityUnsaturatedIsUnchanged)
{
    using namespace GameEngine::Mathematics;
    const float radius = 2000.0f;
    const float amp = 60.0f, freq = 6.0f;
    const float holdAlt = 4000.0f; // ~33% occupancy on R=2000 — well below the contention ramp floor
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(1.2f, 16.0f / 9.0f, 0.05f, 40000.0f);
    const uint32_t cap = kSphereBaseDepth + kMaxDecodeSubdiv;

    auto converge = [&](bool biasOn) -> PoolScan {
        CBTInstance instance;
        EXPECT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
        EXPECT_TRUE(instance.InitializeRoots(kDomainSpherical));

        CBTFrameParams p = SphereParams(radius, amp, freq);
        p.PlanetParams[3] = 4.0f;
        p.Screen[0] = 1600.0f; p.Screen[1] = 900.0f;
        p.Screen[2] = 8.0f; p.Screen[3] = 4.0f;
        p.TerrainOrigin[2] = static_cast<float>(cap);
        if (biasOn)
        {
            p.NearBias[0] = 1.0f;   // enabled — but the occupancy gate keeps it inert at 33%
            p.NearBias[1] = 200.0f;
            p.NearBias[2] = 750.0f;
            p.NearBias[3] = 6.0f;
        }

        CBTClassifyDesc classify{};
        classify.Mode = kClassifyScreenSpace;
        classify.TargetDepth = cap;

        auto poseAt = [&](float alt) {
            const Vector3 eye(0.0f, 0.0f, -(radius + alt));
            const Matrix4x4 vp =
                proj * MakeLookAtLH(eye, Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f));
            std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
            p.CameraPos[0] = eye.x; p.CameraPos[1] = eye.y; p.CameraPos[2] = eye.z; p.CameraPos[3] = 1.0f;
        };

        // Graded descent 8000 -> 4000 m; occupancy stays below the ramp floor throughout, so the
        // bias never engages on either run.
        const float startAlt = 8000.0f;
        for (uint32_t f = 0; f < 120u; ++f)
            poseAt(std::max(holdAlt, startAlt * std::pow(0.97f, static_cast<float>(f)))), RunFrame(instance, classify, p, f);
        poseAt(holdAlt);
        for (uint32_t f = 0; f < 32u; ++f)
            RunFrame(instance, classify, p, 200u + f);
        return ScanLivePool(instance);
    };

    const PoolScan off = converge(false);
    const PoolScan on = converge(true);
    const double occ = 100.0 * off.Live / kDefaultBisectorPoolSize;
    std::printf("[view-priority-neutral] alt=%.0fm occupancy=%.1f%% | OFF live=%u medEdge=%.3fm | "
                "ON live=%u medEdge=%.3fm\n",
                holdAlt, occ, off.Live, off.MedEdge, on.Live, on.MedEdge);

    ASSERT_LT(occ, 50.0) << "the neutrality pose is not unsaturated (occupancy >= ramp floor) — "
                            "raise the hold altitude or lower the contention ramp";
    // The occupancy gate makes ON byte-identical to OFF here; allow only the CBT's own tiny
    // atomic-ordering run-to-run jitter (the same two-run variance an OFF/OFF pair would show).
    EXPECT_NEAR(static_cast<double>(on.Live), static_cast<double>(off.Live),
                kDefaultBisectorPoolSize / 200.0)
        << "enabling the near-bias changed an UNSATURATED frame's live count (neutrality broken)";
    EXPECT_NEAR(on.MedEdge, off.MedEdge, std::max(0.5, off.MedEdge * 0.05))
        << "enabling the near-bias changed an UNSATURATED frame's median facet (neutrality broken)";
}

// ===========================================================================
// Walking-headroom slice (round-7 follow-up) — production-cap "what she sees" tables + the four-
// finding gates. The #599 probes above run at the +40 decode cap (kMaxDecodeSubdiv); these run at the
// PRODUCTION cap ResolveTerrainMaxDepth derives (the shipped editor default) so the numbers are what
// the user actually sees, and — for the gate tests — with the DemandTuning demand shaping the editor
// now ships (CBTRenderFeature.cpp). Findings 3 and 4 must hold under ONE demand config (the editor
// default), so every gate test builds from EditorDefaultDemand — proving "both gates simultaneously".
// ===========================================================================

namespace
{
const DescentRow* NearestDescentRow(const std::vector<DescentRow>& rows, float altTarget)
{
    const DescentRow* best = nullptr;
    double bestErr = 1e30;
    for (const DescentRow& r : rows)
    {
        const double e = std::abs(static_cast<double>(r.Alt) - static_cast<double>(altTarget));
        if (e < bestErr) { bestErr = e; best = &r; }
    }
    return best;
}

// The editor-default DemandTuning config (CBTDemandTuning.h), at the given production cap. THE
// single config findings 2/3/4 are validated under.
ProbeConfig EditorDefaultDemand(uint32_t capSubdiv)
{
    ProbeConfig c;
    c.CapSubdiv = capSubdiv;
    c.DemandOn = true;
    c.NearFieldTargetM = kNearFieldFacetTargetM;
    c.KeepOcc = kOffFrustumKeepOcc;
    c.RescuePxMul = kEdgeRescueTpeMul;
    c.RescueOcc = kEdgeRescueOcc;
    return c;
}

constexpr double kOldPlanetTargetM = 1.0;  // the shipped-before facet target (kPlanetTargetFacetMeters)
constexpr double kNewPlanetTargetM = 0.25; // the walking-headroom default
} // namespace

// FINDING 1 (fails-before / passes-after) — the shipped 1 m production cap clamps the walking anchor
// above the 0.5 m product bar; the 0.25 m cap clears it. Runs the SAME nadir descent at both the old
// production cap (1 m target, pre-slice metric) and the new one (0.25 m target + editor demand
// shaping) and compares the ~49 m walking-anchor whole-pool median. Prints the grep-taggable
// [prod-cap-*] lines the production-cap table is read from. R=2000 anchor; R20k/R50k below.
TEST_F(CBTSphereDomainTest, ProductionCapWalkingAnchorR2000)
{
    const std::vector<float> checkpoints = {8000.0f, 4000.0f, 2000.0f, 1000.0f, 500.0f, 200.0f,
                                            150.0f,  100.0f,  49.0f,   20.0f,   5.0f};
    const uint32_t oldCap = ProductionCapSubdiv(2000.0, kOldPlanetTargetM);
    const uint32_t newCap = ProductionCapSubdiv(2000.0, kNewPlanetTargetM);
    ProbeConfig oldCfg; oldCfg.CapSubdiv = oldCap; oldCfg.DemandOn = false; // pre-slice default
    const ProbeConfig newCfg = EditorDefaultDemand(newCap);
    const auto oldRows = RunAltitudeDescent(2000.0f, checkpoints, "pool-alt-prodOLD-R2000", oldCfg);
    const auto newRows = RunAltitudeDescent(2000.0f, checkpoints, "pool-alt-prodNEW-R2000", newCfg);
    const DescentRow* oldA = NearestDescentRow(oldRows, 49.0f);
    const DescentRow* newA = NearestDescentRow(newRows, 49.0f);
    ASSERT_NE(oldA, nullptr); ASSERT_NE(newA, nullptr);
    std::printf("[prod-cap-anchor] R=2000 OLD(sd=%u,1m) anchor@%.0fm nearMed=%.3fm wholeMed=%.3fm occ=%.1f%% "
                "maxND=%u | NEW(sd=%u,0.25m,demandOn) anchor@%.0fm nearMed=%.3fm wholeMed=%.3fm occ=%.1f%% maxND=%u\n",
                oldCap, oldA->Alt, oldA->NearMed, oldA->WholeMed, oldA->Occ, oldA->MaxND, newCap, newA->Alt,
                newA->NearMed, newA->WholeMed, newA->Occ, newA->MaxND);
    // Finding 1, deterministic core: the shipped 1 m cap's finest facet sits ABOVE the 0.5 m product
    // bar (unreachable at ANY altitude), the 0.25 m cap's sits below it. Convergence-free.
    EXPECT_GT(CapFloorMeters(2000.0, oldCap), 0.5)
        << "the shipped 1 m production cap floor already clears 0.5 m (finding 1 moot)";
    EXPECT_LT(CapFloorMeters(2000.0, newCap), 0.5)
        << "the 0.25 m production cap floor does not clear the 0.5 m product bar";
    // Empirical confirmation the classifier reaches near the floor: the OLD cap pins the under-feet
    // median above the bar (floor-limited); the NEW cap drives it to the metric equilibrium (~0.5 m at
    // 48 m; the eye-height grazing feet reaches 0.35 m — see WalkingHeadroomGrazingR2000).
    EXPECT_GT(oldA->NearMed, 0.9) << "OLD-cap under-feet median not pinned by the 1 m floor";
    EXPECT_LT(newA->NearMed, 0.6) << "NEW-cap under-feet median did not approach the product bar";
    EXPECT_LE(newA->MaxND, newCap) << "walking anchor exceeded the production decode cap";
}

// FINDING 1 large-planet — production caps land under the int64 decode cap (R=50000 ~37 <= 40) and the
// walking anchor clears the product bar. Prints the table rows; asserts the cap-precision invariant.
TEST_F(CBTSphereDomainTest, ProductionCapWalkingAnchorR20k)
{
    const std::vector<float> checkpoints = {40000.0f, 8000.0f, 4000.0f, 2000.0f, 1000.0f, 500.0f,
                                            200.0f,   150.0f,  100.0f,  49.0f,   20.0f,   5.0f};
    const uint32_t newCap = ProductionCapSubdiv(20000.0, kNewPlanetTargetM);
    const auto rows = RunAltitudeDescent(20000.0f, checkpoints, "pool-alt-prodNEW-R20k",
                                         EditorDefaultDemand(newCap));
    const DescentRow* a = NearestDescentRow(rows, 49.0f);
    ASSERT_NE(a, nullptr);
    std::printf("[prod-cap-anchor] R=20000 NEW(sd=%u,0.25m,demandOn) anchor@%.0fm wholeMed=%.3fm "
                "nearMed=%.3fm occ=%.1f%% maxND=%u\n", newCap, a->Alt, a->WholeMed, a->NearMed, a->Occ, a->MaxND);
    EXPECT_LE(newCap, kMaxDecodeSubdiv) << "R=20000 production cap exceeds the int64 decode cap";
    EXPECT_LT(CapFloorMeters(20000.0, newCap), 0.5) << "R=20000 production cap floor does not clear 0.5 m";
    EXPECT_LT(a->NearMed, 0.6) << "R=20000 under-feet walking median did not approach the product bar";
    EXPECT_LE(a->MaxND, newCap) << "R=20000 walking anchor exceeded the production decode cap";
}

TEST_F(CBTSphereDomainTest, ProductionCapWalkingAnchorR50k)
{
    const std::vector<float> checkpoints = {100000.0f, 20000.0f, 8000.0f, 4000.0f, 2000.0f, 1000.0f,
                                            500.0f,    200.0f,   150.0f,  100.0f,  49.0f,   20.0f, 5.0f};
    const uint32_t newCap = ProductionCapSubdiv(50000.0, kNewPlanetTargetM);
    const auto rows = RunAltitudeDescent(50000.0f, checkpoints, "pool-alt-prodNEW-R50k",
                                         EditorDefaultDemand(newCap));
    const DescentRow* a = NearestDescentRow(rows, 49.0f);
    ASSERT_NE(a, nullptr);
    std::printf("[prod-cap-anchor] R=50000 NEW(sd=%u,0.25m,demandOn) anchor@%.0fm wholeMed=%.3fm "
                "nearMed=%.3fm occ=%.1f%% maxND=%u\n", newCap, a->Alt, a->WholeMed, a->NearMed, a->Occ, a->MaxND);
    EXPECT_LE(newCap, kMaxDecodeSubdiv) << "R=50000 production cap exceeds the int64 decode cap (should be ~37)";
    EXPECT_LE(a->MaxND, newCap) << "R=50000 walking anchor exceeded the production decode cap";
    EXPECT_LT(CapFloorMeters(50000.0, newCap), 0.5) << "R=50000 production cap floor does not clear 0.5 m";
    // Whole-pool median at R=50000 is horizon-disc-inflated (bigger planet shows more surface); the
    // near-under-the-feet median is the honest walking datum. Report both; gate the near one softly.
    EXPECT_LT(a->NearMed, 0.6) << "R=50000 at-nadir walking detail did not approach the product bar";
}

// EARTH-SCALE fails-before (decode-precision arc S1): at R = 6.371e6 the production cap
// derivation SATURATES the int64 decode cap (numSubdiv 40 -> facet floor ~9.54 m), so the
// walking pose is floor-pinned ~19x above the 0.5 m product bar. This is the arc's live
// "before" number; when the deep-decode cure lands (S2) and the cap lifts, the FeetMed
// expectations here are the ones that flip to < 0.5 (update them WITH the cure, with an
// in-test justification — the #617 rule).
//
// Unlike the R<=50k probes, the pose math must run the PRODUCTION Earth-scale path (#524):
// the camera pose is built in double and rebased about the render-origin sector, and
// CBTFrameParams carries RenderOriginSector so Classify's metric projects small camera-
// relative corners (a raw fp32 world VP at 6.371e6 would swamp the look pitch: one fp32
// ULP at Earth magnitude is 0.5 m — larger than the whole grazing pitch offset).
TEST_F(CBTSphereDomainTest, EarthWalkingPoseFacetFloorPinnedByDecodeCap)
{
    using namespace GameEngine::Mathematics;
    constexpr double kEarthR = 6.371e6;
    constexpr double kAlt = 1.7; // walking eye height
    const uint32_t capSub = ProductionCapSubdiv(kEarthR, kNewPlanetTargetM);
    // Precondition: Earth saturates the decode cap (the unclamped 0.25 m derivation wants ~51).
    ASSERT_EQ(capSub, kMaxDecodeSubdiv) << "Earth no longer saturates the decode cap — arc premise gone";
    const double capFloor = CapFloorMeters(kEarthR, capSub);
    EXPECT_NEAR(capFloor, 9.544, 0.02);

    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots(kDomainSpherical));

    const uint32_t cap = kSphereBaseDepth + capSub;
    const float radiusF = static_cast<float>(kEarthR);
    CBTFrameParams p = SphereParams(radiusF, 0.0f, 5.0f); // clean sphere (relief 0), like the grazing probes
    p.PlanetParams[3] = 4.0f;
    p.Screen[0] = 1600.0f;
    p.Screen[1] = 900.0f;
    p.Screen[2] = 8.0f;
    p.Screen[3] = 4.0f;
    p.TerrainOrigin[2] = static_cast<float>(cap);
    ApplyDemandTuning(p, EditorDefaultDemand(capSub));

    // Render origin: the camera's 1024 m world sector (fixed across the descent — the whole
    // approach stays inside it). Sector coords at Earth (~6222) are exact in fp32.
    const int32_t secZ = static_cast<int32_t>(std::lround(-(kEarthR + kAlt) / 1024.0));
    const double originZ = 1024.0 * static_cast<double>(secZ);
    p.RenderOriginSector[0] = 0.0f;
    p.RenderOriginSector[1] = 0.0f;
    p.RenderOriginSector[2] = static_cast<float>(secZ);
    p.RenderOriginSector[3] = 1024.0f;

    const float startAlt = 60.0f;
    const float farPlane = static_cast<float>(3.0 * kEarthR) + 2.0f * startAlt;
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(1.2f, 16.0f / 9.0f, 0.05f, farPlane);
    const Vector3 upLocal(0.0f, 0.0f, -1.0f); // radial outward at the nadir point
    constexpr float kGrazeWalkPitch = 0.03f;  // ~1.7 deg below horizontal, the walker's look
    const Vector3 dirCenter = (Vector3(1.0f, 0.0f, 0.0f) * std::cos(kGrazeWalkPitch) +
                               Vector3(0.0f, 0.0f, 1.0f) * std::sin(kGrazeWalkPitch))
                                  .Normalize();

    auto poseAt = [&](double a) {
        const double eyeZ = -(kEarthR + a);
        const Vector3 eyeRel(0.0f, 0.0f, static_cast<float>(eyeZ - originZ)); // small: fp32-precise
        const Matrix4x4 vp = proj * MakeLookAtLH(eyeRel, eyeRel + dirCenter, upLocal);
        std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
        p.CameraPos[0] = 0.0f; // PLANET-CENTER space (horizon cull wants true radii)
        p.CameraPos[1] = 0.0f;
        p.CameraPos[2] = static_cast<float>(eyeZ);
        p.CameraPos[3] = 1.0f;
    };

    CBTClassifyDesc classify{};
    classify.Mode = kClassifyScreenSpace;
    classify.TargetDepth = cap;

    constexpr uint32_t kApproach = 140u;
    for (uint32_t f = 0; f < kApproach; ++f)
    {
        poseAt(std::max(kAlt, static_cast<double>(startAlt) * std::pow(0.95, static_cast<double>(f))));
        RunFrame(instance, classify, p, f);
    }
    poseAt(kAlt);
    for (uint32_t f = 0; f < 60u; ++f)
        RunFrame(instance, classify, p, kApproach + f);

    // Ground points ahead along the +X walk tangent, in DOUBLE (fp32 normalize at Earth
    // magnitude would wobble the scan centre by ~0.5 m; harmless for 6 m discs, but the
    // double path costs nothing and keeps the scan centres exact).
    auto groundAhead = [&](double dArc) {
        const double v[3] = {dArc, 0.0, -kEarthR};
        const double len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        return std::array<double, 3>{v[0] / len * kEarthR, v[1] / len * kEarthR, v[2] / len * kEarthR};
    };
    const std::array<double, 3> feetC = groundAhead(12.0);
    const std::array<double, 3> aheadC = groundAhead(60.0);
    const PoolScan whole = ScanLivePool(instance);
    const PoolScan feet = ScanNearPatch(instance, feetC, 6.0);
    const PoolScan ahead = ScanNearPatch(instance, aheadC, 24.0);
    const std::vector<CubeTri> tris = ReadLiveCubeTris(instance, kDefaultBisectorPoolSize);
    const uint32_t bad = CountNonConformingEdges(tris);
    const double occ = 100.0 * static_cast<double>(whole.Live) / kDefaultBisectorPoolSize;

    std::printf("[earth-pose] R=%.4g alt=%.1fm cap(sd=%u,floor=%.3fm) live=%u occ=%.1f%% "
                "feetMed=%.3fm feetMin=%.3fm feetMaxND=%u aheadMed=%.3fm wholeMed=%.3fm "
                "maxND=%u nonConf=%u\n",
                kEarthR, kAlt, capSub, capFloor, whole.Live, occ, feet.MedEdge, feet.MinEdge,
                feet.MaxNumSubdiv, ahead.MedEdge, whole.MedEdge, whole.MaxNumSubdiv, bad);

    // The wall, live: the under-feet patch REACHES the decode cap (cap-pinned, not
    // demand-starved) and its facets sit an order of magnitude above the product bar.
    ASSERT_GT(feet.Live, 0u) << "feet disc empty — pose/scan mismatch";
    EXPECT_EQ(feet.MaxNumSubdiv, capSub) << "under-feet detail must be pinned AT the decode cap";
    EXPECT_GT(feet.MedEdge, 2.0) << "Earth walking feet median unexpectedly below the cap floor "
                                    "band — did the decode cap move without updating this pin?";
    EXPECT_LT(feet.MedEdge, 25.0) << "feet median above the cap floor band — demand starvation?";
    EXPECT_GT(feet.MinEdge, 1.0) << "an edge below the cap floor band exists — cap not binding?";
    // The floor is ~19x the walking bar: the product bar is UNREACHABLE at Earth radius today.
    EXPECT_GT(feet.MedEdge, 4.0 * 0.5) << "fails-before: walking-bar detail must NOT be reachable";
    // Integrity at Earth scale: closed conforming manifold, zero validation errors.
    EXPECT_EQ(bad, 0u) << "cross-face conformity broke at Earth radius";
    EXPECT_EQ(instance.ReadValidationErrorCount(), 0u);
}

// S2b PAYOFF GATE (the arc's passes-after): the SAME Earth walking pose with the deep
// (sector, local) storage live AND the flag-coupled cap lift (numSubdiv 50 — what
// TerrainProvisioning SubdivCapFor derives with GE_CBT_DEEP_DECODE on; the fails-before
// twin above pins 12.15 m at the fp32 cap 40). Justification for the flipped pins (the
// #617 rule): the deep cap floors the Earth split edge at (pi*R/2)*2^-25 = 0.298 m, so the
// under-feet median lands in the [floor, ~1.6x floor] band the metric equilibrium allows —
// under the 0.5 m walking bar that was 24x out of reach. Storage carries it: the df64
// (sector, local) decode error at nd 50 is <= 2e-5 m (0.02% of the facet short edge; the
// fp32 world store would be at 275% — the S1 measured table), and the conformity oracle
// runs on the raised 2^50 common grid. Also measures the honest cost datum: occupancy +
// quiescent CBT.Update wall clock at the lifted cap (S1 headroom claim re-checked).
TEST_F(CBTSphereDomainTest, EarthWalkingPoseDeepDecodeClearsWalkingBar)
{
    using namespace GameEngine::Mathematics;
    constexpr double kEarthR = 6.371e6;
    constexpr double kAlt = 1.7; // walking eye height
    const uint32_t capSub =
        ProductionCapSubdiv(kEarthR, kNewPlanetTargetM, DeepDecode::kDeepDecodeSubdiv);
    // Earth saturates the deep cap too (the unclamped 0.25 m derivation wants 51): the lift
    // is clamped by 1, not by 11 — the walk-exactness ceiling (~52) keeps 2 levels of margin.
    ASSERT_EQ(capSub, DeepDecode::kDeepDecodeSubdiv)
        << "Earth no longer saturates the deep decode cap — re-derive these pins";
    const double capFloor = CapFloorMeters(kEarthR, capSub);
    EXPECT_NEAR(capFloor, 0.298, 0.005) << "deep cap facet floor model moved";
    EXPECT_LT(capFloor, 0.5) << "the lifted cap must put the floor under the walking bar";

    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots(kDomainSpherical));

    const uint32_t cap = kSphereBaseDepth + capSub;
    const float radiusF = static_cast<float>(kEarthR);
    CBTFrameParams p = SphereParams(radiusF, 0.0f, 5.0f); // clean sphere (relief 0), as the OFF twin
    p.PlanetParams[3] = 4.0f;
    p.Screen[0] = 1600.0f;
    p.Screen[1] = 900.0f;
    p.Screen[2] = 8.0f;
    p.Screen[3] = 4.0f;
    p.TerrainOrigin[2] = static_cast<float>(cap);
    ApplyDemandTuning(p, EditorDefaultDemand(capSub));

    const int32_t secZ = static_cast<int32_t>(std::lround(-(kEarthR + kAlt) / 1024.0));
    const double originZ = 1024.0 * static_cast<double>(secZ);
    p.RenderOriginSector[0] = 0.0f;
    p.RenderOriginSector[1] = 0.0f;
    p.RenderOriginSector[2] = static_cast<float>(secZ);
    p.RenderOriginSector[3] = 1024.0f;

    const float startAlt = 60.0f;
    const float farPlane = static_cast<float>(3.0 * kEarthR) + 2.0f * startAlt;
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(1.2f, 16.0f / 9.0f, 0.05f, farPlane);
    const Vector3 upLocal(0.0f, 0.0f, -1.0f); // radial outward at the nadir point
    constexpr float kGrazeWalkPitch = 0.03f;  // ~1.7 deg below horizontal, the walker's look
    const Vector3 dirCenter = (Vector3(1.0f, 0.0f, 0.0f) * std::cos(kGrazeWalkPitch) +
                               Vector3(0.0f, 0.0f, 1.0f) * std::sin(kGrazeWalkPitch))
                                  .Normalize();

    auto poseAt = [&](double a) {
        const double eyeZ = -(kEarthR + a);
        const Vector3 eyeRel(0.0f, 0.0f, static_cast<float>(eyeZ - originZ)); // small: fp32-precise
        const Matrix4x4 vp = proj * MakeLookAtLH(eyeRel, eyeRel + dirCenter, upLocal);
        std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
        p.CameraPos[0] = 0.0f; // PLANET-CENTER space (horizon cull wants true radii)
        p.CameraPos[1] = 0.0f;
        p.CameraPos[2] = static_cast<float>(eyeZ);
        p.CameraPos[3] = 1.0f;
    };

    CBTClassifyDesc classify{};
    classify.Mode = kClassifyScreenSpace;
    classify.TargetDepth = cap;
    classify.DeepDecode = 1u; // the flag-ON pipeline (CBTUpdateSystem's coupled read)

    constexpr uint32_t kApproach = 140u;
    for (uint32_t f = 0; f < kApproach; ++f)
    {
        poseAt(std::max(kAlt, static_cast<double>(startAlt) * std::pow(0.95, static_cast<double>(f))));
        RunFrame(instance, classify, p, f);
    }
    poseAt(kAlt);
    for (uint32_t f = 0; f < 60u; ++f)
        RunFrame(instance, classify, p, kApproach + f);

    auto groundAhead = [&](double dArc) {
        const double v[3] = {dArc, 0.0, -kEarthR};
        const double len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        return std::array<double, 3>{v[0] / len * kEarthR, v[1] / len * kEarthR, v[2] / len * kEarthR};
    };
    const std::array<double, 3> feetC = groundAhead(12.0);
    const std::array<double, 3> aheadC = groundAhead(60.0);
    const PoolScan whole = ScanLivePool(instance);
    const PoolScan feet = ScanNearPatch(instance, feetC, 6.0);
    const PoolScan ahead = ScanNearPatch(instance, aheadC, 24.0);
    const std::vector<CubeTri> tris = ReadLiveCubeTris(instance, kDefaultBisectorPoolSize);
    const uint32_t bad = CountNonConformingEdges(tris);
    const double occ = 100.0 * static_cast<double>(whole.Live) / kDefaultBisectorPoolSize;

    // The honest quiescent-cost datum at the lifted cap: production quiescence gate on
    // (GateVertexEval=1 — a converged frame re-evaluates ~0 vertices), min-of-30 wall clock
    // (min under the shared box's noise, the RunCostProbe convention).
    CBTClassifyDesc quiescent = classify;
    quiescent.GateVertexEval = 1u;
    const CBTTessellationStats statsBefore = instance.ReadTessellationStats();
    double quiesMinMs = 1e30, quiesSumMs = 0.0;
    constexpr uint32_t kTimed = 30u;
    for (uint32_t f = 0; f < kTimed; ++f)
    {
        const auto t0 = std::chrono::steady_clock::now();
        RunFrame(instance, quiescent, p, kApproach + 60u + f);
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        quiesMinMs = std::min(quiesMinMs, ms);
        quiesSumMs += ms;
    }
    const CBTTessellationStats stats = instance.ReadTessellationStats();
    // Steady-state dropped splits over the timed window (cumulative overflow includes the
    // approach's convergence churn, which is expected at a 50-level descent; what must be
    // ~0 is the converged pose's rate — the RunCostProbe DroppedOverQuiescent convention).
    const int overflowQuiescent = stats.OverflowTotal - statsBefore.OverflowTotal;

    std::printf("[earth-pose-deep] R=%.4g alt=%.1fm cap(sd=%u,floor=%.3fm) live=%u occ=%.1f%% "
                "feetMed=%.3fm feetMin=%.3fm feetMaxND=%u aheadMed=%.3fm wholeMed=%.3fm "
                "maxND=%u nonConf=%u overflowTotal=%d overflowQuiescent=%d "
                "quiescentMs(min)=%.3f (mean %.3f)\n",
                kEarthR, kAlt, capSub, capFloor, whole.Live, occ, feet.MedEdge, feet.MinEdge,
                feet.MaxNumSubdiv, ahead.MedEdge, whole.MedEdge, whole.MaxNumSubdiv, bad,
                stats.OverflowTotal, overflowQuiescent, quiesMinMs, quiesSumMs / kTimed);
    EXPECT_EQ(overflowQuiescent, 0)
        << "converged Earth walking pose still drops splits — demand not screen-bounded at "
           "the deep cap";

    // THE PRODUCT GATE: sub-half-metre facets under the feet at Earth radius (was 12.15 m).
    ASSERT_GT(feet.Live, 0u) << "feet disc empty — pose/scan mismatch";
    EXPECT_LT(feet.MedEdge, 0.5) << "walking-bar detail must be REACHED with the deep cap";
    EXPECT_GT(feet.MedEdge, 0.15) << "feet median below the deep floor band — scan reading "
                                     "garbage (deep corners misreconstructed?)";
    // The lift actually engaged: under-feet detail refines PAST the old fp32-store wall.
    EXPECT_GT(feet.MaxNumSubdiv, kMaxDecodeSubdiv)
        << "no bisector beyond the old cap — the lift did not engage";
    EXPECT_LE(feet.MaxNumSubdiv, capSub) << "refinement exceeded the deep decode cap";
    // Integrity at the lifted depth: closed conforming manifold (2^50 common grid), zero
    // validation errors, and the demand machinery stays sane (bounded occupancy — S1
    // measured 3.4% headroom at cap 40; the near-field 0.5 m target bounds the deep spend).
    EXPECT_EQ(bad, 0u) << "cross-face conformity broke at the deep cap";
    EXPECT_EQ(instance.ReadValidationErrorCount(), 0u);
    EXPECT_LT(occ, 90.0) << "Earth walking pose saturates the 1M pool at the deep cap — "
                            "report the occupancy/TPE trade-off instead of shipping this";
}

// FINDING 2 (walking-altitude headroom, A/B) — at the walking grazing pose, the demand shaping must
// leave measurable free pool (>10-15%) so findings 3/4 have room, WITHOUT coarsening the at-feet
// detail past the product bar. Grazing walk, production cap, demand OFF vs the editor default ON.
TEST_F(CBTSphereDomainTest, WalkingHeadroomGrazingR2000)
{
    const uint32_t cap = ProductionCapSubdiv(2000.0, kNewPlanetTargetM);
    ProbeConfig off; off.CapSubdiv = cap; off.DemandOn = false; // production cap, pre-slice metric
    const GrazingRow rOff = RunGrazingProbe(2000.0f, 2.0f, "walk-headroom-OFF-R2000", off);
    const GrazingRow rOn = RunGrazingProbe(2000.0f, 2.0f, "walk-headroom-ON-R2000", EditorDefaultDemand(cap));
    std::printf("[walk-headroom] R=2000 cap(sd=%u) OFF occ=%.1f%% feetMed=%.3fm splitDemand=%d dropped=%d | "
                "ON occ=%.1f%% feetMed=%.3fm splitDemand=%d dropped=%d | headroomGain=%.1f%%\n",
                cap, rOff.Occ, rOff.FeetMed, rOff.SplitDemand, rOff.DroppedOverSettle, rOn.Occ, rOn.FeetMed,
                rOn.SplitDemand, rOn.DroppedOverSettle, rOff.Occ - rOn.Occ);
    EXPECT_EQ(rOn.Bad, 0u) << "demand-on grazing broke cross-face conformity";
    EXPECT_LT(rOn.FeetMed, 0.5)
        << "the behind-eye bound coarsened the at-feet facet past the product bar (over-reach)";
    // Finding-2 gate: measurable free headroom at the walking pose with the fixes on.
    EXPECT_LT(rOn.Occ, 90.0) << "walking pose still saturated with demand shaping on (no headroom for 3/4)";
}

// FINDING 3 (rotation churn CHARACTERIZATION, A/B) — the off-frustum keep-band relocates the frustum-
// edge coarsen/re-refine (the user's "massive jumps") to ~1.4 NDC, OFF-SCREEN, by keeping a fixed
// annulus of just-off-frustum geometry at depth. HONEST NEGATIVE captured here: the whole-pool
// per-frame SIMPLIFY count RISES with the fix on (the kept annulus steadily coarsens at its off-screen
// outer edge — MORE total merge work), even though the on-screen pop is gone. The total-count metric
// therefore CANNOT gate this fix; it conflates the relocated off-screen churn with the visible pop. The
// measurable headless win is the BURST: the fix must not spike the per-frame max (the all-at-once
// coarsen that reads as a jump). The VISIBLE-pop gate is the editor micro-rotation screenshot sequence
// (task requirement) — this probe prints the signature the report reads and guards conformity + burst.
TEST_F(CBTSphereDomainTest, RotationChurnGated)
{
    const uint32_t cap2k = ProductionCapSubdiv(2000.0, kNewPlanetTargetM);
    ProbeConfig off2k; off2k.CapSubdiv = cap2k; off2k.DemandOn = false;
    const RotationRow r2Off = RunRotationChurn(2000.0f, 2.0f, "rotchurn-OFF-R2000", off2k);
    const RotationRow r2On = RunRotationChurn(2000.0f, 2.0f, "rotchurn-ON-R2000", EditorDefaultDemand(cap2k));
    std::printf("[rotchurn-gate] R=2000 walking(HEADROOM) OFF occ=%.1f%% yawSimplify(mean/max)=%.0f/%.0f | "
                "ON occ=%.1f%% yawSimplify(mean/max)=%.0f/%.0f\n", r2Off.Occ, r2Off.YawSimplifyMean,
                r2Off.YawSimplifyMax, r2On.Occ, r2On.YawSimplifyMean, r2On.YawSimplifyMax);

    const uint32_t cap20k = ProductionCapSubdiv(20000.0, kNewPlanetTargetM);
    ProbeConfig off20k; off20k.CapSubdiv = cap20k; off20k.DemandOn = false;
    const RotationRow r20Off = RunRotationChurn(20000.0f, 5.0f, "rotchurn-OFF-R20k", off20k);
    const RotationRow r20On = RunRotationChurn(20000.0f, 5.0f, "rotchurn-ON-R20k", EditorDefaultDemand(cap20k));
    std::printf("[rotchurn-gate] R=20000 walking(user repro) OFF occ=%.1f%% yawSimplify(mean/max)=%.0f/%.0f | "
                "ON occ=%.1f%% yawSimplify(mean/max)=%.0f/%.0f meanDelta=%.0f\n", r20Off.Occ,
                r20Off.YawSimplifyMean, r20Off.YawSimplifyMax, r20On.Occ, r20On.YawSimplifyMean,
                r20On.YawSimplifyMax, r20On.YawSimplifyMean - r20Off.YawSimplifyMean);

    EXPECT_EQ(r2On.Bad, 0u) << "demand-on rotation broke conformity (R=2000)";
    EXPECT_EQ(r20On.Bad, 0u) << "demand-on rotation broke conformity (R=20000)";
    // Burst guard at the user's repro: the keep-band must not spike the per-frame merge MAX (the burst
    // that reads as a jump) beyond the pre-slice all-at-once coarsen. (The mean rises by design — the
    // churn is relocated off-screen, not created; the visible-pop verdict is the editor micro-rotation.)
    EXPECT_LE(r20On.YawSimplifyMax, r20Off.YawSimplifyMax * 1.1)
        << "the keep-band spiked the per-frame merge burst (a visible jump) — not just relocated churn";
    // The pool must not run away to a frozen near field (the #599 hoarding failure): occupancy stays
    // below saturation with the annulus retained.
    EXPECT_LT(r20On.Occ, 96.0) << "keep-band hoarded the pool toward saturation (near-field freeze risk)";
}

// FINDING 4 (flat-disc edge-rescue NEUTRALITY on the sphere) — the rescue must not fire on a bare
// sphere (its near ground is ~80deg, the area metric refines it fine), so enabling it must not inflate
// split demand or change the feet detail vs demand-on-without-rescue. The actual flat-disc validation
// (near-90deg incidence) is editor-side (a flatten modifier); the headless sphere only guards that the
// rescue is self-limiting and neutral where it should not engage (limb inflation stays dead).
TEST_F(CBTSphereDomainTest, EdgeRescueNeutralOnSphereR2000)
{
    const uint32_t cap = ProductionCapSubdiv(2000.0, kNewPlanetTargetM);
    ProbeConfig noRescue = EditorDefaultDemand(cap);
    noRescue.RescuePxMul = 0.0f; // demand shaping on, rescue OFF
    const GrazingRow rNo = RunGrazingProbe(2000.0f, 2.0f, "rescue-OFF-R2000", noRescue);
    const GrazingRow rYes = RunGrazingProbe(2000.0f, 2.0f, "rescue-ON-R2000", EditorDefaultDemand(cap));
    std::printf("[edge-rescue] R=2000 rescueOFF occ=%.1f%% feetMed=%.3fm splitDemand=%d | "
                "rescueON occ=%.1f%% feetMed=%.3fm splitDemand=%d | demandDelta=%d\n",
                rNo.Occ, rNo.FeetMed, rNo.SplitDemand, rYes.Occ, rYes.FeetMed, rYes.SplitDemand,
                rYes.SplitDemand - rNo.SplitDemand);
    EXPECT_EQ(rYes.Bad, 0u) << "edge-rescue broke cross-face conformity on the sphere";
    // Neutrality: rescue must not materially inflate split demand on the sphere (limb inflation dead).
    EXPECT_LT(rYes.SplitDemand, rNo.SplitDemand + rNo.SplitDemand / 5 + 2000)
        << "edge-rescue materially inflated split demand on the bare sphere (should be neutral — "
           "raise the rescue px threshold)";
    EXPECT_LE(rYes.FeetMed, rNo.FeetMed + 0.05)
        << "edge-rescue coarsened the sphere feet detail (unexpected)";
}
