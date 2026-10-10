#include <gtest/gtest.h>

#include "CBTTerrain/CBTDeepDecode.h"
#include "CBTTerrain/CBTDemandTuning.h"
#include "CBTTerrain/CBTInstance.h"
#include "CBTTerrain/CBTKernelSet.h"
#include "CBTTerrain/CBTNearBias.h"
#include "CBTTerrain/CBTSphereRoots.h"
#include "CBTTestHarness.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Core/CommandList.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <unordered_map>
#include <vector>

using namespace GameEngine::CBTTerrain;
using namespace GameEngine::CBTTerrain::Test;
using namespace GameEngine::Rendering;

namespace
{
constexpr uint32_t kGridBits = 50;
constexpr uint64_t kGrid = uint64_t{1} << kGridBits;
constexpr uint32_t kVertexWords = sizeof(CBTVertexData) / sizeof(uint32_t);
// The 1024 m world-sector grid of the deep store, as a double for the reference arithmetic.
constexpr double kSectorSizeM = DeepDecode::kSectorSizeMeters;
// A few ulps of fp32 (2^-24 is 6e-8): the rounding a stored corner accrues through the decode,
// applied to a unit direction and, scaled by the radius, to the shell error.
constexpr double kFp32RelativeTolerance = 4e-7;
// The geometric shell bound: twice the authored amplitude for the sum of the fBM octaves, plus
// the fp32 rounding of a radius-scale coordinate, plus an absolute floor.
constexpr double kShellAmplitudeBound = 2.0;
constexpr double kShellAbsoluteToleranceM = 0.001;
// Visible-cull oracle. A corner faces the camera when the camera stands more than the margin
// above the corner's tangent plane; it is interior when its clip position is in front of the eye
// by a positive w, inside the side planes by a margin and away from both depth planes (reverse-Z:
// z/w is 1 at the near plane and 0 at the far plane).
constexpr double kFacingMarginM = 0.5;
constexpr double kMinClipW = 0.0001;
constexpr double kInteriorNdcBound = 0.8;
constexpr double kInteriorFarDepth = 1e-10;
constexpr double kInteriorNearDepth = 0.99;
// The transition trace prints at most this many parent-to-children splits per frame pair.
constexpr uint32_t kMaxTracedTransitions = 16u;
// Relief edits displace by this fraction of the radius, so the relief scales with the planet;
// Params() gives it this fBM frequency and octave count (PlanetParams.z/.w).
constexpr float kReliefAmplitudeFraction = 0.002f;
constexpr float kReliefFrequency = 5.0f;
constexpr float kReliefOctaves = 8.0f;
using CornerKey = std::array<int64_t, 3>;
using Point = std::array<double, 3>;
struct ScaleCase { float Radius; bool Deep; const char* Name; };
struct KeyHash
{
    size_t operator()(const CornerKey& k) const
    {
        size_t h = 0;
        for (const auto v : k) h ^= std::hash<int64_t>{}(v) + 0x9e3779b9u + (h << 6u) + (h >> 2u);
        return h;
    }
};
struct Snapshot
{
    std::vector<uint64_t> Heap;
    std::vector<CBTVertexData> Vertices;
    std::vector<uint32_t> Compact;
    std::vector<uint32_t> Visible;
    std::vector<uint32_t> Bits;
    uint32_t DrawCount = 0;
};
struct Audit
{
    uint32_t Live = 0, BadKeys = 0, Coverage = 0, Edges = 0, Compact = 0, Occupancy = 0;
    uint32_t Shared = 0, SharedFailures = 0, NonFinite = 0, Degenerate = 0, Winding = 0;
    uint32_t Representation = 0, Shell = 0, Direction = 0;
    uint32_t Visible = 0;
    double MaxShellError = 0, MinEdge = 1e100;
};

// Geometric midpoint subdivision on one fixed integer cube grid, independent of
// both the shader's barycentric walk and its persisted floating-point vertices.
std::array<CornerKey, 3> Corners(uint64_t h,
    const std::array<CBTSphereRoot, kSphereRootCount>& roots)
{
    const uint32_t sub = std::bit_width(h) - 1u - kSphereBaseDepth;
    const auto& root = roots[uint32_t(h >> sub) - (1u << kSphereBaseDepth)];
    std::array<CornerKey, 3> c;
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        c[0][axis] = int64_t(root.V0[axis]) * int64_t(kGrid);
        c[1][axis] = int64_t(root.V1[axis]) * int64_t(kGrid);
        c[2][axis] = int64_t(root.V2[axis]) * int64_t(kGrid);
    }
    for (uint32_t bit = sub; bit > 0; --bit)
    {
        CornerKey mid;
        for (uint32_t axis = 0; axis < 3; ++axis) mid[axis] = (c[0][axis] + c[2][axis]) / 2;
        c = ((h >> (bit - 1)) & 1u) ? std::array{c[1], mid, c[0]} : std::array{c[2], mid, c[1]};
    }
    return c;
}
double Volume(const Point& a, const Point& b, const Point& c)
{
    const Point u{b[0]-a[0], b[1]-a[1], b[2]-a[2]};
    const Point v{c[0]-a[0], c[1]-a[1], c[2]-a[2]};
    return a[0]*(u[1]*v[2]-u[2]*v[1]) + a[1]*(u[2]*v[0]-u[0]*v[2]) + a[2]*(u[0]*v[1]-u[1]*v[0]);
}
double Length(const Point& p) { return std::sqrt(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]); }

Audit Inspect(const Snapshot& s, float radius, float amplitude, bool deep)
{
    Audit a;
    const auto roots = BuildSphereRoots();
    std::array<std::vector<std::pair<uint64_t, uint64_t>>, kSphereRootCount> intervals;
    std::vector<std::array<CornerKey, 2>> edges;
    std::vector<uint32_t> liveSlots;
    // The complete stored representation must agree at shared geometric corners,
    // including across cube faces. UVs deliberately differ across face charts.
    std::unordered_map<CornerKey, std::array<uint32_t, 5>, KeyHash> stored;
    for (uint32_t slot = 0; slot < s.Heap.size(); ++slot)
    {
        const uint64_t h = s.Heap[slot];
        if (bool((s.Bits[slot / 32u] >> (slot % 32u)) & 1u) != bool(h)) ++a.Occupancy;
        if (!h) continue;
        ++a.Live;
        liveSlots.push_back(slot);
        const uint32_t depth = std::bit_width(h) - 1u;
        const uint32_t cap = deep ? DeepDecode::kDeepDecodeSubdiv : kMaxDecodeSubdiv;
        if (depth < kSphereBaseDepth || depth > kSphereBaseDepth + cap)
        { ++a.BadKeys; continue; }
        const uint32_t sub = depth - kSphereBaseDepth;
        const uint32_t root = uint32_t(h >> sub) - (1u << kSphereBaseDepth);
        if (root >= kSphereRootCount || slot >= s.Vertices.size()) { ++a.BadKeys; continue; }
        const uint64_t width = uint64_t{1} << (kGridBits-sub);
        intervals[root].emplace_back((h & ((uint64_t{1} << sub)-1u))*width, width);
        const auto keys = Corners(h, roots);
        const auto& v = s.Vertices[slot];
        const float* xyz[] = {v.Corner0, v.Corner1, v.Corner2};
        const uint32_t* sec[] = {v.Sector0, v.Sector1, v.Sector2};
        if (v.DeepTag[0] != uint32_t(deep) || v.DeepTag[1] != 0u) ++a.Representation;
        std::array<Point, 3> world, cube;
        for (uint32_t corner = 0; corner < 3; ++corner)
        {
            const std::array<uint32_t, 5> payload{std::bit_cast<uint32_t>(xyz[corner][0]),
                std::bit_cast<uint32_t>(xyz[corner][1]), std::bit_cast<uint32_t>(xyz[corner][2]),
                sec[corner][0], sec[corner][1]};
            auto [it, inserted] = stored.emplace(keys[corner], payload);
            if (!inserted) { ++a.Shared; if (it->second != payload) ++a.SharedFailures; }
            if ((!deep && (sec[corner][0] || sec[corner][1])) || (sec[corner][1] >> 16u)) ++a.Representation;
            const int32_t sector[] = {UnpackSectorLo(sec[corner][0]), UnpackSectorHi(sec[corner][0]),
                                     UnpackSectorLo(sec[corner][1])};
            for (uint32_t axis = 0; axis < 3; ++axis)
            {
                world[corner][axis] = double(xyz[corner][axis]) + (deep ? kSectorSizeM*sector[axis] : 0.0);
                cube[corner][axis] = double(keys[corner][axis]);
                if (!std::isfinite(world[corner][axis])) ++a.NonFinite;
            }
            const double error = std::abs(Length(world[corner]) - radius);
            const double worldLength = Length(world[corner]);
            const double cubeLength = Length(cube[corner]);
            Point directionError;
            for (uint32_t axis = 0; axis < 3; ++axis)
                directionError[axis] = world[corner][axis]/worldLength - cube[corner][axis]/cubeLength;
            if (Length(directionError) > kFp32RelativeTolerance) ++a.Direction;
            a.MaxShellError = std::max(a.MaxShellError, error);
            // Geometric shell bound includes all fBM octaves and float rounding;
            // shared-corner equality above remains exact, without this tolerance.
            if (error > kShellAmplitudeBound*std::abs(amplitude) + kFp32RelativeTolerance*radius +
                    kShellAbsoluteToleranceM) ++a.Shell;
            auto edge = std::array{keys[corner], keys[(corner+1)%3]};
            if (edge[1] < edge[0]) std::swap(edge[0], edge[1]);
            edges.push_back(edge);
        }
        const double actual = Volume(world[0], world[1], world[2]);
        const double reference = Volume(cube[0], cube[1], cube[2]);
        if (!std::isfinite(actual) || actual == 0) ++a.Degenerate;
        else if ((actual > 0) != (reference > 0)) ++a.Winding;
        for (uint32_t i = 0; i < 3; ++i)
        {
            Point d;
            for (uint32_t axis = 0; axis < 3; ++axis) d[axis] = world[i][axis]-world[(i+1)%3][axis];
            a.MinEdge = std::min(a.MinEdge, Length(d));
        }
    }
    for (auto& leaves : intervals)
    {
        std::sort(leaves.begin(), leaves.end());
        uint64_t cursor = 0;
        for (auto [start, width] : leaves) { if (start != cursor) ++a.Coverage; cursor = start+width; }
        if (cursor != kGrid) ++a.Coverage;
    }
    std::sort(edges.begin(), edges.end());
    for (size_t i = 0; i < edges.size();)
    {
        size_t end = i+1;
        while (end < edges.size() && edges[end] == edges[i]) ++end;
        if (end-i != 2) ++a.Edges;
        i = end;
    }
    auto compact = s.Compact;
    std::sort(compact.begin(), compact.end());
    if (compact != liveSlots || s.DrawCount != 3u*liveSlots.size()) ++a.Compact;
    auto visible = s.Visible;
    std::sort(visible.begin(), visible.end());
    for (size_t i = 0; i < visible.size(); ++i)
        if (visible[i] >= s.Heap.size() || !s.Heap[visible[i]] || (i && visible[i] == visible[i-1])) ++a.Visible;
    return a;
}

// Strict interior subset: a clean-sphere triangle whose three independent key
// directions face the camera, and whose actual corners are well inside every
// clip plane, cannot be hidden by either conservative cull. Boundary triangles
// are excluded so floating-point disagreements at the limb/planes are harmless.
std::vector<uint32_t> RequiredVisible(const Snapshot& s, const CBTFrameParams& p, bool deep)
{
    std::vector<uint32_t> required;
    const auto roots = BuildSphereRoots();
    for (uint32_t slot = 0; slot < s.Vertices.size(); ++slot)
    {
        if (!s.Heap[slot]) continue;
        const auto keys = Corners(s.Heap[slot], roots);
        const auto& v = s.Vertices[slot];
        const float* xyz[] = {v.Corner0,v.Corner1,v.Corner2};
        const uint32_t* sec[] = {v.Sector0,v.Sector1,v.Sector2};
        bool inside = true;
        for (uint32_t corner = 0; corner < 3; ++corner)
        {
            Point key{double(keys[corner][0]),double(keys[corner][1]),double(keys[corner][2])};
            const double norm = Length(key);
            double facing = 0;
            for (uint32_t axis = 0; axis < 3; ++axis) facing += key[axis]/norm*p.CameraPos[axis];
            if (facing <= double(p.PlanetParams[0])+kFacingMarginM) { inside = false; break; }
            const int32_t sector[] = {UnpackSectorLo(sec[corner][0]),UnpackSectorHi(sec[corner][0]),UnpackSectorLo(sec[corner][1])};
            std::array<double,4> relative{0,0,0,1}, clip{};
            for (uint32_t axis = 0; axis < 3; ++axis)
                relative[axis] = double(xyz[corner][axis]) + (deep ? kSectorSizeM*sector[axis] : 0.0)
                    - kSectorSizeM*p.RenderOriginSector[axis];
            for (uint32_t row = 0; row < 4; ++row)
                for (uint32_t col = 0; col < 4; ++col) clip[row] += p.ViewProjRel[col*4+row]*relative[col];
            if (!(clip[3] > kMinClipW) || std::abs(clip[0]) >= kInteriorNdcBound*clip[3] ||
                std::abs(clip[1]) >= kInteriorNdcBound*clip[3] || clip[2] <= kInteriorFarDepth*clip[3] ||
                clip[2] >= kInteriorNearDepth*clip[3])
            { inside = false; break; }
        }
        if (inside) required.push_back(slot);
    }
    return required;
}
uint32_t MissingVisible(const Snapshot& s, const std::vector<uint32_t>& required)
{
    auto visible = s.Visible;
    std::sort(visible.begin(), visible.end());
    uint32_t missing = 0;
    for (const auto slot : required) if (!std::binary_search(visible.begin(), visible.end(), slot)) ++missing;
    return missing;
}

void TraceTransitions(const Snapshot& before, const Snapshot& after, const CBTFrameParams& p, bool deep)
{
    std::unordered_map<uint64_t, uint32_t> oldSlots, newSlots;
    for (const auto slot : before.Compact) oldSlots.emplace(before.Heap[slot],slot);
    for (const auto slot : after.Compact) newSlots.emplace(after.Heap[slot],slot);
    const auto metric = [&](const CBTVertexData& v) {
        const float* xyz[] = {v.Corner0,v.Corner1,v.Corner2};
        const uint32_t* sec[] = {v.Sector0,v.Sector1,v.Sector2};
        std::array<std::array<double,2>,3> pixels{};
        for (uint32_t corner=0; corner<3; ++corner)
        {
            const int32_t sector[] = {UnpackSectorLo(sec[corner][0]),UnpackSectorHi(sec[corner][0]),UnpackSectorLo(sec[corner][1])};
            std::array<double,4> relative{0,0,0,1},clip{};
            for (uint32_t axis=0; axis<3; ++axis)
                relative[axis] = xyz[corner][axis]+(deep?kSectorSizeM*sector[axis]:0)-kSectorSizeM*p.RenderOriginSector[axis];
            for (uint32_t row=0; row<4; ++row)
                for (uint32_t col=0; col<4; ++col) clip[row] += p.ViewProjRel[col*4+row]*relative[col];
            for (uint32_t axis=0; axis<2; ++axis) pixels[corner][axis] = .5*p.Screen[axis]*clip[axis]/clip[3];
        }
        const auto& a=pixels[0]; const auto& b=pixels[1]; const auto& c=pixels[2];
        return std::array{.5*std::abs((b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0])),
            std::hypot(c[0]-a[0],c[1]-a[1])};
    };
    uint32_t printed=0;
    for (const auto& [h,slot] : oldSlots)
    {
        const auto left=newSlots.find(h*2),right=newSlots.find(h*2+1);
        if (newSlots.contains(h) || left==newSlots.end() || right==newSlots.end()) continue;
        const auto m=metric(before.Vertices[slot]),l=metric(after.Vertices[left->second]),r=metric(after.Vertices[right->second]);
        std::printf("[sphere-transition] parent=%llu area=%.9g edge=%.9g child0(area=%.9g edge=%.9g) child1(area=%.9g edge=%.9g)\n",
            static_cast<unsigned long long>(h),m[0],m[1],l[0],l[1],r[0],r[1]);
        if (++printed==kMaxTracedTransitions) break;
    }
}
}

class CBTSphereScaleAudit : public ::testing::TestWithParam<ScaleCase>
{
  protected:
    void SetUp() override
    {
        SetHeadlessEnv();
        DeviceDesc desc{};
        desc.preferredAPI = GraphicsAPI::Vulkan;
        desc.enableDynamicRendering = true;
        const char* validation = std::getenv("GE_CBT_SCALE_AUDIT_VALIDATION");
        desc.enableDebugLayer = validation && std::strcmp(validation, "1") == 0;
        m_Device = DeviceFactory::CreateDevice(desc);
        ASSERT_TRUE(m_Device);
        ASSERT_TRUE(m_Device->Initialize(desc));
        if (!m_Device->GetCapabilities().supportsShaderInt64)
            GTEST_SKIP() << "Vulkan shaderInt64 is required";
        if (desc.enableDebugLayer) ASSERT_TRUE(m_Device->GetValidationStats().Enabled);
        ASSERT_TRUE(m_Kernels.Initialize(*m_Device, ShaderOutputDir()));
    }
    void TearDown() override
    {
        m_Kernels.Shutdown();
        if (m_Device)
        {
            // Device destruction is itself validated: an unreachable backend
            // buffer may only be reported here, after ordinary frame checks.
            m_Device->Shutdown();
            const auto validation = m_Device->GetValidationStats();
            for (const auto& entry : validation.Vuids)
                if (validation.ErrorCount || validation.WarningCount)
                    std::printf("[sphere-scale-validation] %s: %s\n", entry.Vuid.c_str(), entry.FirstMessage.c_str());
            EXPECT_EQ(validation.ErrorCount, 0u);
            EXPECT_EQ(validation.WarningCount, 0u);
            EXPECT_EQ(validation.OverflowCount, 0u);
        }
    }
    void Frame(CBTInstance& inst, const CBTClassifyDesc& c, const CBTFrameParams& p)
    {
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        ASSERT_TRUE(cl);
        cl->Begin(); inst.RecordUpdate(*cl, c, p, m_Frame++); inst.RecordReadback(*cl); cl->End();
        m_Device->ExecuteCommandLists(std::vector<CommandList*>{cl.get()});
        m_Device->WaitForIdle();
        for (uint32_t i = kValidationErrorCounter; i <= kValidationCompactCounter; ++i)
            ASSERT_EQ(inst.ReadValidationCounter(i), 0u) << "frame=" << m_Frame;
        ASSERT_EQ(inst.ReadTessellationStats().DispatchClampTotal, 0);
    }
    void Frames(CBTInstance& inst, const CBTClassifyDesc& c, const CBTFrameParams& p, uint32_t n)
    { for (uint32_t i = 0; i < n; ++i) { Frame(inst, c, p); ASSERT_FALSE(HasFatalFailure()); } }
    Snapshot Read(CBTInstance& inst)
    {
        Snapshot s;
        const uint32_t pool = inst.GetResources().GetPoolSize();
        const auto heap = inst.DebugReadWords(CBTBinding::HeapID, pool*2u);
        EXPECT_EQ(heap.size(), pool*2u);
        if (heap.size() != pool*2u) return s;
        s.Heap.resize(pool);
        std::memcpy(s.Heap.data(), heap.data(), heap.size()*4u);
        uint32_t highest = 0;
        for (uint32_t i = 0; i < pool; ++i) if (s.Heap[i]) highest = i+1;
        const auto vertices = inst.DebugReadWords(CBTBinding::CurrentVertex, highest*kVertexWords);
        EXPECT_EQ(vertices.size(), highest*kVertexWords);
        if (vertices.size() != highest*kVertexWords) return {};
        s.Vertices.resize(highest);
        std::memcpy(s.Vertices.data(), vertices.data(), vertices.size()*4u);
        s.DrawCount = inst.ReadDrawIndexCount(kDrawStreamAll);
        EXPECT_LE(s.DrawCount, pool*3u);
        s.Compact = inst.DebugReadWords(CBTBinding::IndicesAll, std::min(pool, s.DrawCount/3u));
        const auto visibleCount = inst.ReadDrawIndexCount(kDrawStreamVisible);
        EXPECT_LE(visibleCount, pool*3u);
        EXPECT_EQ(visibleCount%3u, 0u);
        s.Visible = inst.DebugReadWords(CBTBinding::IndicesVisible, std::min(pool,visibleCount/3u));
        EXPECT_EQ(s.Visible.size(), visibleCount/3u);
        s.Bits = inst.DebugReadWords(CBTBinding::Bitfield, (pool+31u)/32u);
        EXPECT_EQ(s.Bits.size(), (pool+31u)/32u);
        if (s.Bits.size() != (pool+31u)/32u) return {};
        return s;
    }
    Audit Check(const Snapshot& s, float radius, float amplitude, const char* phase, const CBTFrameParams* camera = nullptr)
    {
        SCOPED_TRACE(phase);
        EXPECT_FALSE(s.Heap.empty());
        const auto a = Inspect(s, radius, amplitude, GetParam().Deep);
        EXPECT_GT(a.Live, 0u); EXPECT_GT(a.Shared, 0u);
        EXPECT_EQ(a.BadKeys, 0u); EXPECT_EQ(a.Coverage, 0u); EXPECT_EQ(a.Edges, 0u);
        EXPECT_EQ(a.Compact, 0u); EXPECT_EQ(a.Occupancy, 0u); EXPECT_EQ(a.SharedFailures, 0u);
        EXPECT_EQ(a.NonFinite, 0u); EXPECT_EQ(a.Degenerate, 0u); EXPECT_EQ(a.Winding, 0u);
        EXPECT_EQ(a.Representation, 0u); EXPECT_EQ(a.Shell, 0u); EXPECT_EQ(a.Direction, 0u);
        EXPECT_EQ(a.Visible, 0u);
        if (camera && !a.BadKeys)
        {
            const auto required = RequiredVisible(s, *camera, GetParam().Deep);
            EXPECT_EQ(MissingVisible(s, required), 0u) << "clearly visible triangle omitted from actual draw stream";
        }
        std::printf("[sphere-scale] %s R=%.0f deep=%u live=%u minEdge=%.6g shellError=%.6g shared=%u mismatches=%u\n",
            phase, radius, unsigned(GetParam().Deep), a.Live, a.MinEdge, a.MaxShellError, a.Shared, a.SharedFailures);
        return a;
    }
    CBTFrameParams Params(float radius, float amplitude = 0) const
    {
        CBTFrameParams p{};
        p.PlanetParams[0] = radius; p.PlanetParams[1] = amplitude;
        p.PlanetParams[2] = kReliefFrequency; p.PlanetParams[3] = kReliefOctaves;
        return p;
    }
    std::unique_ptr<IDevice> m_Device;
    CBTKernelSet m_Kernels;
    uint32_t m_Frame = 0;
};

TEST_P(CBTSphereScaleAudit, StoredGeometryCoversSphereThroughRefinementAndScaleEdits)
{
    // Uniform refinement eight levels under the depth-target classifier (24 roots -> 6144 facets);
    // the live floor only catches an arm that never refined.
    constexpr uint32_t kUniformRefineSubdiv = 8u;
    constexpr uint32_t kUniformRefineFrames = 16u;
    constexpr uint32_t kMinUniformLive = 1000u;
    // A positive-control corner displaced by a hundredth of the radius: outside every tolerance.
    constexpr float kCornerDamageFraction = 0.01f;
    // The radius edit grows the planet by an eighth, with relief switched on in the same refresh.
    constexpr float kRadiusEditScale = 1.125f;
    // Root 0 is then the focus five levels deeper (mixed depths), and finally the whole tree
    // coarsens back to the roots; the frame counts let every split and merge settle.
    constexpr uint32_t kFocusRefineExtraSubdiv = 5u;
    constexpr uint32_t kFocusRefineFrames = 24u;
    constexpr uint32_t kCoarsenFrames = 32u;
    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_Kernels));
    ASSERT_TRUE(inst.InitializeRoots(kDomainSpherical));
    inst.SetValidateEachUpdate(true);
    const auto bytes = inst.GetResources().GetPersistentByteSize();
    const auto vertexBuffer = inst.GetResources().GetBuffer(CBTBinding::CurrentVertex);
    auto p = Params(GetParam().Radius);
    CBTClassifyDesc c{};
    c.Mode = kClassifyDepthTarget; c.FocusRoot = kFocusRootAll;
    c.TargetDepth = kSphereBaseDepth+kUniformRefineSubdiv; c.DeepDecode = GetParam().Deep;
    Frames(inst, c, p, kUniformRefineFrames);
    ASSERT_FALSE(HasFatalFailure());
    auto clean = Read(inst);
    const auto initial = Check(clean, p.PlanetParams[0], 0, "uniform");
    ASSERT_GT(initial.Live, kMinUniformLive);
    ASSERT_FALSE(HasFailure());

    // Positive controls prove that topology-only conformity cannot mask damaged
    // cached positions, and that matching index counts cannot mask a draw hole.
    auto damaged = clean;
    const uint32_t first = damaged.Compact.front();
    // Push the corner along the cube axis least aligned with it, so at least sqrt(2/3) of the
    // push is tangential. A push along the corner's own direction changes only its length,
    // which the Direction check cannot see: +x on a +/-X face centre is exactly radial.
    const auto corner = Corners(damaged.Heap[first], BuildSphereRoots())[0];
    const auto tangentialAxis = std::ranges::min_element(corner, {}, [](int64_t v) { return std::abs(v); });
    damaged.Vertices[first].Corner0[tangentialAxis - corner.begin()] += p.PlanetParams[0]*kCornerDamageFraction;
    EXPECT_GT(Inspect(damaged, p.PlanetParams[0], 0, GetParam().Deep).SharedFailures, 0u);
    EXPECT_GT(Inspect(damaged, p.PlanetParams[0], 0, GetParam().Deep).Direction, 0u);
    damaged = clean;
    damaged.Compact[0] = damaged.Compact[1];
    EXPECT_GT(Inspect(damaged, p.PlanetParams[0], 0, GetParam().Deep).Compact, 0u);

    // The caller explicitly requests the full refresh that production issues for
    // radius/relief edits. This checks GPU geometry, not ECS invalidation routing.
    p.PlanetParams[0] *= kRadiusEditScale;
    p.PlanetParams[1] = p.PlanetParams[0]*kReliefAmplitudeFraction;
    c.GateVertexEval = 0;
    Frame(inst, c, p);
    Check(Read(inst), p.PlanetParams[0], p.PlanetParams[1], "radius-and-relief-edit");
    c.FocusRoot = 0; c.TargetDepth += kFocusRefineExtraSubdiv;
    c.GateVertexEval = 1;
    Frames(inst, c, p, kFocusRefineFrames);
    Check(Read(inst), p.PlanetParams[0], p.PlanetParams[1], "mixed-depth-refine-merge");
    c.FocusRoot = kFocusRootAll; c.TargetDepth = kSphereBaseDepth;
    Frames(inst, c, p, kCoarsenFrames);
    const auto coarse = Check(Read(inst), p.PlanetParams[0], p.PlanetParams[1], "coarsened");
    EXPECT_EQ(coarse.Live, kSphereRootCount);
    EXPECT_EQ(inst.GetResources().GetPersistentByteSize(), bytes);
    EXPECT_EQ(inst.GetResources().GetBuffer(CBTBinding::CurrentVertex), vertexBuffer);
    std::printf("[sphere-scale-memory] R=%.0f deep=%u pool=%u persistentBytes=%llu\n", GetParam().Radius,
        unsigned(GetParam().Deep), inst.GetResources().GetPoolSize(), static_cast<unsigned long long>(bytes));
}

TEST_P(CBTSphereScaleAudit, ScreenSpaceTeleportRebaseAndDistanceLodKeepActualCoverage)
{
    using namespace GameEngine::Mathematics;
    // The depth cap models a root split edge as a quarter great circle (the cube face's 90-degree
    // span) halved every two LEB levels and stops where that model reaches the finest facet edge;
    // at Earth radius it lands on the decode cap, the precision ceiling the arms characterize. The
    // printed floor is that model, not a measured edge.
    constexpr double kModelRootEdgeArc = std::numbers::pi*0.5;
    constexpr double kSubdivisionsPerEdgeHalving = 2.0;
    constexpr double kFinestFacetEdgeM = 0.25;
    // A walking eye height for the two surface poses, an orbit at three radii for the far pose.
    constexpr float kWalkingEyeHeightM = 1.7f;
    constexpr float kOrbitAltitudeRadii = 3.0f;
    // A 1600x900 view, 1.2 rad vertical field, near plane 5 cm, far plane eight radii; splits above
    // 8 px of projected LEB edge and merges below 4 px.
    constexpr float kVerticalFovRadians = 1.2f;
    constexpr float kScreenWidthPixels = 1600.0f;
    constexpr float kScreenHeightPixels = 900.0f;
    constexpr float kNearPlaneM = 0.05f;
    constexpr float kFarPlaneRadii = 8.0f;
    constexpr float kSplitThresholdPixels = 8.0f;
    constexpr float kMergeThresholdPixels = 4.0f;
    // Frames for a pose to converge, the quiet span that must show no split or merge work, the
    // settle after each rescue toggle, the quiet span once the rescue is restored, and the settle
    // after the relief edit.
    constexpr uint32_t kConvergeFrames = 96u;
    constexpr uint32_t kQuietFrames = 16u;
    constexpr uint32_t kRescueToggleFrames = 32u;
    constexpr uint32_t kRestoredRescueQuietFrames = 8u;
    constexpr uint32_t kReliefSettleFrames = 64u;
    // The floor a pose's live set, the retained-key set and the visible-interior set must clear for
    // their oracles to mean anything.
    constexpr uint32_t kMinOracleFacets = 100u;
    // Displaced vertices must move the shell by at least this fraction of the amplitude, or the
    // relief premise of the sibling-read check did not hold.
    constexpr double kMinReliefShellFraction = 0.05;
    const float radius = GetParam().Radius;
    const uint32_t sub = uint32_t(std::clamp<long>(
        std::lround(kSubdivisionsPerEdgeHalving*std::log2(kModelRootEdgeArc*radius/kFinestFacetEdgeM)),
        0, long(GetParam().Deep ? DeepDecode::kDeepDecodeSubdiv : kMaxDecodeSubdiv)));
    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_Kernels));
    ASSERT_TRUE(inst.InitializeRoots(kDomainSpherical));
    inst.SetValidateEachUpdate(true);
    const auto bytes = inst.GetResources().GetPersistentByteSize();
    CBTClassifyDesc c{};
    c.Mode = kClassifyScreenSpace; c.TargetDepth = kSphereBaseDepth+sub;
    c.GateVertexEval = 1; c.DeepDecode = GetParam().Deep;
    auto pose = [&](uint32_t axis, float altitude, int32_t sectorShift = 0)
    {
        auto p = Params(radius);
        const double distance = double(radius)+altitude;
        Point eye{};
        eye[axis] = -distance;
        Vector3 relative, direction(0,0,0);
        float position[3];
        for (uint32_t i = 0; i < 3; ++i)
        {
            const int32_t sector = int32_t(std::lround(eye[i]/kSectorSizeM)) + (i == 0 ? sectorShift : 0);
            p.RenderOriginSector[i] = float(sector);
            position[i] = float(eye[i]-double(sector)*kSectorSizeM);
            p.CameraPos[i] = float(eye[i]);
        }
        relative = Vector3(position[0],position[1],position[2]);
        direction = axis == 0 ? Vector3(1,0,0) : Vector3(0,0,1);
        const auto proj = MakePerspectiveLH_ZO_ReverseZ(kVerticalFovRadians, kScreenWidthPixels/kScreenHeightPixels,
            kNearPlaneM, radius*kFarPlaneRadii);
        const auto vp = proj*MakeLookAtLH(relative, relative+direction, Vector3(0,1,0));
        std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
        p.CameraPos[3] = 1; p.RenderOriginSector[3] = DeepDecode::kSectorSizeMeters;
        p.Screen[0] = kScreenWidthPixels; p.Screen[1] = kScreenHeightPixels;
        p.Screen[2] = kSplitThresholdPixels; p.Screen[3] = kMergeThresholdPixels;
        p.TerrainOrigin[2] = float(c.TargetDepth);
        const auto near = ComputeNearBiasRadii(altitude);
        p.NearBias[0] = 1; p.NearBias[1] = near.NearRadius;
        p.NearBias[2] = near.FarRadius; p.NearBias[3] = kNearBiasMaxCoarsen;
        // The editor's shipped walking-demand defaults (CBTDemandTuning.h), so the arms run the
        // production demand model; the radius-derived cap above intentionally characterizes the
        // default Earth precision ceiling.
        p.DemandTuning[0] = kNearFieldFacetTargetM; p.DemandTuning[1] = kOffFrustumKeepOcc;
        p.DemandTuning[2] = kEdgeRescueTpeMul*p.Screen[2]; p.DemandTuning[3] = kEdgeRescueOcc;
        return p;
    };
    auto p = pose(2, kWalkingEyeHeightM);
    Frames(inst, c, p, kConvergeFrames);
    ASSERT_FALSE(HasFatalFailure());
    const auto near = Check(Read(inst), radius, 0, "near-default-cap", &p);
    ASSERT_GT(near.Live, kMinOracleFacets);
    ASSERT_FALSE(HasFailure());

    // Check the FIRST frame after teleport as well as the converged result. Old
    // cached geometry must still form a complete surface while new LOD arrives.
    p = pose(0, kWalkingEyeHeightM);
    Frame(inst, c, p);
    Check(Read(inst), radius, 0, "first-teleport-frame", &p);
    ASSERT_FALSE(HasFailure());
    Frames(inst, c, p, kConvergeFrames);
    auto beforeRebase = Read(inst);
    Check(beforeRebase, radius, 0, "teleport-converged", &p);
    ASSERT_FALSE(HasFailure());
    p = pose(0, kWalkingEyeHeightM, 1);
    Frame(inst, c, p);
    auto afterRebase = Read(inst);
    Check(afterRebase, radius, 0, "equivalent-camera-new-origin", &p);
    ASSERT_FALSE(HasFailure());
    // Retained keys must keep their exact cached geometry through an origin
    // change; legitimate LOD transitions may allocate different keys/slots.
    std::unordered_map<uint64_t, CBTVertexData> previous;
    for (uint32_t slot = 0; slot < beforeRebase.Vertices.size(); ++slot)
        if (beforeRebase.Heap[slot]) previous.emplace(beforeRebase.Heap[slot], beforeRebase.Vertices[slot]);
    uint32_t retained = 0;
    for (uint32_t slot = 0; slot < afterRebase.Vertices.size(); ++slot)
    {
        const auto it = previous.find(afterRebase.Heap[slot]);
        if (it == previous.end()) continue;
        ++retained;
        EXPECT_EQ(std::memcmp(&it->second, &afterRebase.Vertices[slot], sizeof(CBTVertexData)), 0);
    }
    EXPECT_GT(retained, kMinOracleFacets);

    p = pose(0, radius*kOrbitAltitudeRadii);
    Frame(inst, c, p);
    Check(Read(inst), radius, 0, "first-orbit-frame", &p);
    ASSERT_FALSE(HasFailure());
    Frames(inst, c, p, kConvergeFrames);
    auto far = Read(inst);
    const auto farAudit = Check(far, radius, 0, "orbit-converged", &p);
    const auto required = RequiredVisible(far, p, GetParam().Deep);
    ASSERT_GT(required.size(), kMinOracleFacets) << "visible-cull oracle needs a nonempty interior region";
    auto omitted = far;
    const auto visibleIt = std::find(omitted.Visible.begin(), omitted.Visible.end(), required.front());
    ASSERT_NE(visibleIt, omitted.Visible.end());
    omitted.Visible.erase(visibleIt);
    EXPECT_EQ(MissingVisible(omitted, required), 1u) << "cull oracle must detect one omitted visible triangle";
    // Orbit sees a much larger area of the planet than the close nadir view,
    // so total live count is not monotone with distance. Compare facet size.
    EXPECT_GT(farAudit.MinEdge, near.MinEdge) << "distance LOD kept the finest surface facets";
    const bool trace = std::getenv("GE_CBT_SPHERE_TRANSITION_TRACE") != nullptr;
    Snapshot lastTrace;
    if (trace) lastTrace=far;
    for (uint32_t f = 0; f < kQuietFrames; ++f)
    {
        Frame(inst, c, p);
        const auto stats = inst.ReadTessellationStats();
        if (trace)
            std::printf("[sphere-scale-quiet] R=%.0f deep=%u f=%u live=%u split=%d served=%d merge=%d merged=%d\n",
                radius, unsigned(GetParam().Deep), f, stats.LiveCount, stats.SplitDemand, stats.SplitServed,
                stats.MergeDemand, stats.MergeServed);
        if (trace && f<2)
        {
            auto current=Read(inst);
            TraceTransitions(lastTrace,current,p,GetParam().Deep);
            TraceTransitions(current,lastTrace,p,GetParam().Deep);
            lastTrace=std::move(current);
        }
        EXPECT_EQ(stats.SplitServed, 0) << "stationary orbit split at frame " << f;
        EXPECT_EQ(stats.MergeServed, 0) << "stationary orbit merged at frame " << f;
    }
    const auto quiet = Read(inst);
    Check(quiet, radius, 0, "orbit-quiet", &p);
    ASSERT_EQ(far.Heap, quiet.Heap) << "stationary converged LOD still changes topology";
    ASSERT_EQ(far.Vertices.size(), quiet.Vertices.size());
    for (uint32_t slot = 0; slot < far.Vertices.size(); ++slot)
        if (far.Heap[slot]) EXPECT_EQ(std::memcmp(&far.Vertices[slot], &quiet.Vertices[slot], sizeof(CBTVertexData)), 0);

    // The parent hold applies only while rescue is active. Turning rescue off
    // must release the extra limb detail; restoring it must refine that detail
    // again and settle, rather than permanently freezing the previous tree.
    const float rescue = p.DemandTuning[2];
    p.DemandTuning[2] = 0;
    Frames(inst, c, p, kRescueToggleFrames);
    const auto noRescue = Check(Read(inst), radius, 0, "orbit-rescue-disabled", &p);
    EXPECT_LT(noRescue.Live, farAudit.Live);
    p.DemandTuning[2] = rescue;
    Frames(inst, c, p, kRescueToggleFrames);
    const auto restoredRescue = Check(Read(inst), radius, 0, "orbit-rescue-restored", &p);
    EXPECT_GT(restoredRescue.Live, noRescue.Live);
    for (uint32_t frame = 0; frame < kRestoredRescueQuietFrames; ++frame)
    {
        Frame(inst, c, p);
        const auto stats = inst.ReadTessellationStats();
        EXPECT_EQ(stats.SplitServed, 0);
        EXPECT_EQ(stats.MergeServed, 0);
    }

    // Exercise the same screen-space sibling reads with displaced endpoints.
    // Reflection through a curved/displaced midpoint is not a valid substitute
    // for these actual endpoints. Geometry and coverage must survive a refresh.
    p.PlanetParams[1] = radius*kReliefAmplitudeFraction;
    c.GateVertexEval = 0;
    Frame(inst, c, p);
    c.GateVertexEval = 1;
    Frames(inst, c, p, kReliefSettleFrames);
    const auto relief = Check(Read(inst), radius, p.PlanetParams[1], "orbit-relief-edit");
    EXPECT_GT(relief.MaxShellError, double(p.PlanetParams[1])*kMinReliefShellFraction)
        << "screen-space relief premise requires actual displaced vertices";
    EXPECT_EQ(inst.GetResources().GetPersistentByteSize(), bytes);
    std::printf("[sphere-scale-lod] R=%.0f deep=%u capSubdiv=%u modeledFloor=%.6g nearLive=%u farLive=%u\n",
        radius, unsigned(GetParam().Deep), sub, kModelRootEdgeArc*radius*std::exp2(-double(sub)/kSubdivisionsPerEdgeHalving),
        near.Live, farAudit.Live);
}

INSTANTIATE_TEST_SUITE_P(RadiusMatrix, CBTSphereScaleAudit,
    ::testing::Values(ScaleCase{512,false,"R512"}, ScaleCase{2048,false,"R2048"},
        ScaleCase{8192,false,"R8192"}, ScaleCase{1000000,false,"R1000km"},
        ScaleCase{6371000,false,"Earth"}, ScaleCase{1000000,true,"R1000kmDeep"},
        ScaleCase{6371000,true,"EarthDeep"}),
    [](const ::testing::TestParamInfo<ScaleCase>& info) { return info.param.Name; });
