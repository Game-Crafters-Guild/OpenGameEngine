// World-scale audit. A fixed 513x513 displaced source isolates terrain extent
// from texture-resolution costs; full editor residency is measured separately.
// Two extents, 512 m and 8 km, bracket the range; the instantiation at the
// bottom of the file says why a denser sweep adds no discriminator.
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <vector>
#include "CBTTerrain/CBTInstance.h"
#include "CBTTerrain/CBTKernelSet.h"
#include "CBTReliefHeightSource.h"
#include "CBTTestHarness.h"

using namespace GameEngine::CBTTerrain;
using namespace GameEngine::Rendering;
namespace CBTTest = GameEngine::CBTTerrain::Test;

// Corner UVs are keyed as 24-bit fixed point, so a corner shared by several facets collides
// exactly rather than nearly; 2^24 is the largest scale an fp32 UV in [0,1] resolves losslessly.
constexpr float kCornerKeyScale = 16777216.0f;
// The key of u == 1 or v == 1 — a corner on the patch boundary, where an edge is unpaired.
constexpr uint32_t kCornerKeyMax = 16777216u;
// Summed facet area against the authored footprint, in square metres. The facets tile the patch
// exactly, so this absorbs only the fp32 error of the per-corner decode.
constexpr double kPlanarAreaToleranceM2 = 0.001;

class CBTPlanarScaleAudit : public ::testing::TestWithParam<uint32_t>
{
  protected:
    using CornerKey = std::array<uint32_t, 2>;
    using PositionBits = std::array<uint32_t, 3>;
    struct Snapshot
    {
        std::map<CornerKey, PositionBits> Corners;
        uint32_t Live = 0;
        double MaxSplitEdgePixels = 0;
    };
    void SetUp() override
    {
        CBTTest::SetHeadlessEnv();
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
        ASSERT_TRUE(m_Kernels.Initialize(*m_Device, CBTTest::ShaderOutputDir()));
    }
    void TearDown() override
    {
        m_Kernels.Shutdown();
        if (m_Device)
        {
            // Unreachable backend resources may be reported only at teardown.
            m_Device->Shutdown();
            const auto validation = m_Device->GetValidationStats();
            for (const auto& entry : validation.Vuids)
                if (validation.ErrorCount || validation.WarningCount)
                    std::printf("[planar-scale-validation] %s: %s\n", entry.Vuid.c_str(), entry.FirstMessage.c_str());
            EXPECT_EQ(validation.ErrorCount, 0u);
            EXPECT_EQ(validation.WarningCount, 0u);
            EXPECT_EQ(validation.OverflowCount, 0u);
        }
    }
    void Frame(CBTInstance& instance, const CBTClassifyDesc& classify, const CBTFrameParams& params)
    {
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        ASSERT_TRUE(cl);
        cl->Begin();
        instance.RecordUpdate(*cl, classify, params, m_Frame++);
        instance.RecordReadback(*cl);
        cl->End();
        m_Device->ExecuteCommandLists(std::vector<CommandList*>{cl.get()});
        m_Device->WaitForIdle();
        for (uint32_t counter = 0; counter < 4; ++counter)
            ASSERT_EQ(instance.ReadValidationCounter(counter), 0u) << "frame=" << m_Frame;
        ASSERT_EQ(instance.ReadWorkQueueCounter(kWQDispatchClampCounter), 0);
    }
    void Frames(CBTInstance& instance, const CBTClassifyDesc& classify, const CBTFrameParams& params, uint32_t count)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            Frame(instance, classify, params);
            ASSERT_FALSE(HasFatalFailure());
        }
    }
    void Inspect(CBTInstance& instance, const CBTFrameParams& params, Snapshot& result)
    {
        constexpr uint32_t stride = sizeof(CBTVertexData) / 4u;
        const uint32_t pool = instance.GetResources().GetPoolSize();
        const auto heap = instance.DebugReadWords(CBTBinding::HeapID, pool * 2u);
        ASSERT_EQ(heap.size(), pool * 2u);
        uint32_t highest = 0;
        std::vector<bool> occupied(pool, false);
        std::vector<std::pair<uint64_t, uint64_t>> intervals;
        for (uint32_t i = 0; i < pool; ++i)
        {
            const uint64_t h = uint64_t(heap[i*2u]) | (uint64_t(heap[i*2u+1u]) << 32u);
            if (!h) continue;
            occupied[i] = true;
            highest = i;
            const uint32_t depth = static_cast<uint32_t>(std::bit_width(h) - 1u);
            ASSERT_GE(depth, 1u);
            ASSERT_LE(depth, 63u);
            const uint64_t width = uint64_t{1} << (63u-depth);
            intervals.emplace_back((h-(uint64_t{1} << depth))*width, width);
        }
        ASSERT_FALSE(intervals.empty());
        std::sort(intervals.begin(), intervals.end());
        uint64_t covered = 0;
        for (const auto& [start, width] : intervals)
        {
            ASSERT_EQ(start, covered) << "logical coverage gap, overlap or duplicate leaf";
            covered += width;
        }
        ASSERT_EQ(covered, uint64_t{1} << 63u);
        result.Live = static_cast<uint32_t>(intervals.size());
        const auto compact = instance.DebugReadWords(CBTBinding::IndicesAll, result.Live);
        ASSERT_EQ(compact.size(), result.Live);
        auto remaining = occupied;
        for (const uint32_t slot : compact)
        {
            ASSERT_LT(slot, pool);
            ASSERT_TRUE(remaining[slot]) << "duplicate or dead compact draw slot";
            remaining[slot] = false;
        }
        EXPECT_EQ(std::count(remaining.begin(), remaining.end(), true), 0);
        EXPECT_EQ(instance.ReadDrawIndexCount(kDrawStreamAll), result.Live*3u);
        // This orthographic matrix puts the entire patch inside the clip volume,
        // including after the equivalent terrain/camera translation. The actual
        // visible draw stream must therefore contain every live facet exactly once.
        ASSERT_EQ(instance.ReadDrawIndexCount(kDrawStreamVisible), result.Live*3u);
        const auto visible = instance.DebugReadWords(CBTBinding::IndicesVisible, result.Live);
        ASSERT_EQ(visible.size(), result.Live);
        remaining = occupied;
        for (const uint32_t slot : visible)
        {
            ASSERT_LT(slot, pool);
            ASSERT_TRUE(remaining[slot]) << "duplicate or dead visible draw slot";
            remaining[slot] = false;
        }
        EXPECT_EQ(std::count(remaining.begin(), remaining.end(), true), 0)
            << "an in-frustum facet is missing from the actual terrain draw";
        const auto words = instance.DebugReadWords(CBTBinding::CurrentVertex, (highest+1u)*stride);
        ASSERT_EQ(words.size(), (highest+1u)*stride);
        using Edge = std::pair<CornerKey, CornerKey>;
        std::map<Edge, uint32_t> edges;
        double area = 0;
        for (const uint32_t slot : compact)
        {
            CBTVertexData vertex{};
            std::memcpy(&vertex, words.data()+slot*stride, sizeof(vertex));
            ASSERT_EQ(vertex.DeepTag[0], 0u);
            const float* corners[] = {vertex.Corner0, vertex.Corner1, vertex.Corner2};
            CornerKey keys[3];
            for (uint32_t i = 0; i < 3; ++i)
            {
                const float* corner = corners[i];
                for (uint32_t axis = 0; axis < 3; ++axis) ASSERT_TRUE(std::isfinite(corner[axis]));
                const float u = corner[3], v = vertex.Meta[i];
                ASSERT_GE(u, 0.0f); ASSERT_LE(u, 1.0f);
                ASSERT_GE(v, 0.0f); ASSERT_LE(v, 1.0f);
                keys[i] = {static_cast<uint32_t>(std::lround(u*kCornerKeyScale)),
                           static_cast<uint32_t>(std::lround(v*kCornerKeyScale))};
                PositionBits bits{};
                std::memcpy(bits.data(), corner, sizeof(bits));
                const auto [it, inserted] = result.Corners.emplace(keys[i], bits);
                ASSERT_TRUE(inserted || it->second == bits) << "shared displaced corner differs in the actual GPU buffer";
                EXPECT_FLOAT_EQ(corner[0], params.TerrainOrigin[0]+u*params.TerrainSize[0]);
                EXPECT_FLOAT_EQ(corner[2], params.TerrainOrigin[1]+v*params.TerrainSize[1]);
            }
            for (uint32_t i = 0; i < 3; ++i)
            {
                Edge edge{keys[i], keys[(i+1u)%3u]};
                if (edge.second < edge.first) std::swap(edge.first, edge.second);
                ++edges[edge];
            }
            const double cross = (double(corners[1][0])-corners[0][0])*(double(corners[2][2])-corners[0][2])
                               -(double(corners[1][2])-corners[0][2])*(double(corners[2][0])-corners[0][0]);
            ASSERT_LT(cross, 0.0) << "collapsed or reversed planar triangle";
            area -= cross*0.5;
            const double dx = (double(corners[0][0])-corners[2][0])/params.TerrainSize[0]*params.Screen[0];
            const double dz = (double(corners[0][2])-corners[2][2])/params.TerrainSize[1]*params.Screen[1];
            result.MaxSplitEdgePixels = std::max(result.MaxSplitEdgePixels, std::hypot(dx, dz));
        }
        for (const auto& [edge, count] : edges)
        {
            const auto& a = edge.first; const auto& b = edge.second;
            const bool boundary = (a[0] == b[0] && (a[0] == 0u || a[0] == kCornerKeyMax)) ||
                                  (a[1] == b[1] && (a[1] == 0u || a[1] == kCornerKeyMax));
            ASSERT_EQ(count, boundary ? 1u : 2u) << "unpaired interior edge or duplicate geometry";
        }
        EXPECT_NEAR(area, double(params.TerrainSize[0])*params.TerrainSize[1], kPlanarAreaToleranceM2);
    }
    std::unique_ptr<IDevice> m_Device;
    CBTKernelSet m_Kernels;
    uint32_t m_Frame = 0;
};

TEST_P(CBTPlanarScaleAudit, CoverageLodAndMemorySurviveRelocationEditsAndReseed)
{
    // The relief texture's edge, fixed across the arms so terrain extent is the only variable.
    constexpr uint32_t kReliefTextureDim = 513u;
    // heightScale = extent / 8: relief spans an eighth of the horizontal extent at every arm.
    constexpr float kHeightScaleDivisor = 8.0f;
    // A 1536x1024 view that splits above 8 px of projected LEB edge and merges below 4 px.
    constexpr float kScreenWidthPixels = 1536.0f;
    constexpr float kScreenHeightPixels = 1024.0f;
    constexpr float kSplitThresholdPixels = 8.0f;
    constexpr float kMergeThresholdPixels = 4.0f;
    // The converged metric may overshoot the split threshold by the classifier's fp32 slack,
    // but not by a pixel.
    constexpr double kMaxSplitEdgePixels = 8.01;
    // Subdivision depth cap. It reaches the shader as TerrainOrigin.z (maxDepth) and the
    // classifier as TargetDepth; the arms hold both at the same cap.
    constexpr uint32_t kMaxSubdivisionDepth = 25u;
    // Frames to converge the screen-space metric from the base tessellation, and the quiescent
    // span the steady-state allocation check runs over once converged.
    constexpr uint32_t kConvergeFrames = 36u;
    constexpr uint32_t kSteadyStateFrames = 16u;
    // A whole-terrain view at the authored threshold converges far past this; the floor only
    // catches an arm that collapsed back to its base triangles.
    constexpr uint32_t kMinLiveFacets = 32u;
    // Relief has to span more than extent/128 before the cached-edit arms mean anything.
    constexpr float kMinReliefDivisor = 128.0f;
    // The cached edit raises the terrain by extent/64 through TerrainSize.w (originY).
    constexpr float kOriginYEditDivisor = 64.0f;
    // Teleport distance for the relocation arm: 2^20 m, far enough that a stale cached corner
    // cannot survive as a plausible value.
    constexpr float kRelocationShift = 1048576.0f;
    // Relative fp32 tolerance for a height differenced against a distance of `extent`.
    constexpr float kHeightEditTolerance = 0.0000002f;

    const float extent = static_cast<float>(GetParam());
    const float heightScale = extent/kHeightScaleDivisor;
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_Kernels));
    ASSERT_TRUE(instance.InitializeRoots(kDomainPlanar));
    // The relief upload's staging buffer is released inside the constructor, so an armed height
    // source costs image bytes and no buffer bytes. DestroyBuffer retires on the idle drain, so
    // both reads are taken with the GPU idle.
    m_Device->WaitForIdle();
    const size_t bufferBytesBeforeRelief = m_Device->DebugGetBufferBytes();
    CBTTest::ReliefHeightSource relief(*m_Device, instance, heightScale, kReliefTextureDim);
    ASSERT_TRUE(relief.IsArmed());
    m_Device->WaitForIdle();
    EXPECT_EQ(m_Device->DebugGetBufferBytes(), bufferBytesBeforeRelief)
        << "the relief upload's staging buffer outlived the height-source constructor";
    CBTFrameParams params{};
    params.ViewProjRel[0] = params.ViewProjRel[9] = 2.0f/extent;
    params.ViewProjRel[14] = 0.5f; params.ViewProjRel[15] = 1.0f;
    params.Screen[0] = kScreenWidthPixels; params.Screen[1] = kScreenHeightPixels;
    params.Screen[2] = kSplitThresholdPixels; params.Screen[3] = kMergeThresholdPixels;
    params.TerrainSize[0] = params.TerrainSize[1] = extent;
    params.TerrainSize[2] = heightScale;
    params.TerrainOrigin[0] = params.TerrainOrigin[1] = -extent/2.0f;
    params.TerrainOrigin[2] = static_cast<float>(kMaxSubdivisionDepth);
    params.CameraPos[1] = extent;
    CBTClassifyDesc classify{};
    classify.Mode = kClassifyScreenSpace; classify.TargetDepth = kMaxSubdivisionDepth;
    classify.GateVertexEval = 1;
    const uint64_t persistentBytes = instance.GetResources().GetPersistentByteSize();
    std::array<BufferHandle, kCBTBindingCount> handles{};
    for (uint32_t i = 0; i < kCBTBindingCount; ++i)
        handles[i] = instance.GetResources().GetBuffer(static_cast<CBTBinding>(i));
    Frames(instance, classify, params, kConvergeFrames);
    ASSERT_FALSE(HasFatalFailure());
    Snapshot initial;
    Inspect(instance, params, initial);
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_GT(initial.Live, kMinLiveFacets);
    float lowestHeight = std::numeric_limits<float>::max();
    float highestHeight = std::numeric_limits<float>::lowest();
    for (const auto& [key, bits] : initial.Corners)
    {
        const float height = std::bit_cast<float>(bits[1]);
        lowestHeight = std::min(lowestHeight, height);
        highestHeight = std::max(highestHeight, height);
    }
    ASSERT_GT(highestHeight-lowestHeight, extent/kMinReliefDivisor)
        << "displaced geometry must contain nontrivial relief before testing cached edits";
    EXPECT_LE(initial.MaxSplitEdgePixels, kMaxSplitEdgePixels) << "authored screen target was not reached in a comfortable whole-terrain view";
    const auto stats = instance.ReadTessellationStats();
    EXPECT_EQ(stats.SplitDemand, 0);
    m_Device->WaitForIdle();
    const size_t buffersBefore = m_Device->DebugGetBufferRegistryCount();
    const size_t bytesBefore = m_Device->DebugGetBufferBytes();
    Frames(instance, classify, params, kSteadyStateFrames);
    ASSERT_FALSE(HasFatalFailure());
    m_Device->WaitForIdle();
    EXPECT_EQ(m_Device->DebugGetBufferRegistryCount(), buffersBefore);
    EXPECT_EQ(m_Device->DebugGetBufferBytes(), bytesBefore);

    // Teleport the terrain and its camera together; a forced first-frame refresh
    // must replace every cached world corner before culling/classification.
    params.TerrainOrigin[0] += kRelocationShift; params.CameraPos[0] += kRelocationShift;
    params.ViewProjRel[12] = -2.0f*kRelocationShift/extent;
    classify.GateVertexEval = 0;
    Frame(instance, classify, params);
    Snapshot moved;
    Inspect(instance, params, moved);
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(moved.Live, initial.Live);
    params.TerrainSize[3] = extent/kOriginYEditDivisor;
    Frame(instance, classify, params);
    Snapshot edited;
    Inspect(instance, params, edited);
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_EQ(edited.Corners.size(), moved.Corners.size());
    for (const auto& [key, bits] : edited.Corners)
    {
        const auto old = moved.Corners.find(key);
        ASSERT_NE(old, moved.Corners.end());
        EXPECT_NEAR(std::bit_cast<float>(bits[1])-std::bit_cast<float>(old->second[1]),
                    params.TerrainSize[3], extent*kHeightEditTolerance);
    }
    ASSERT_TRUE(instance.InitializeRoots(kDomainPlanar));
    Frame(instance, classify, params);
    Snapshot reseeded;
    Inspect(instance, params, reseeded);
    ASSERT_FALSE(HasFatalFailure());
    classify.GateVertexEval = 1;
    Frames(instance, classify, params, kConvergeFrames);
    Snapshot settled;
    Inspect(instance, params, settled);
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_LE(settled.MaxSplitEdgePixels, kMaxSplitEdgePixels);
    EXPECT_EQ(instance.GetResources().GetPersistentByteSize(), persistentBytes);
    for (uint32_t i = 0; i < kCBTBindingCount; ++i)
        EXPECT_EQ(instance.GetResources().GetBuffer(static_cast<CBTBinding>(i)), handles[i]);
    const auto validation = m_Device->GetValidationStats();
    EXPECT_EQ(validation.ErrorCount, 0u); EXPECT_EQ(validation.WarningCount, 0u); EXPECT_EQ(validation.OverflowCount, 0u);
    std::printf("[planar-scale] extent=%u live=%u maxEdgePx=%.4f persistentBytes=%llu steadyBufferBytes=%zu validation=%d\n",
        GetParam(), settled.Live, settled.MaxSplitEdgePixels, static_cast<unsigned long long>(persistentBytes),
        bytesBefore, validation.Enabled ? 1 : 0);
    instance.Shutdown();
}

// Two arms rather than a sweep of the range. The planar decode is scale-invariant in UV — a
// corner is TerrainOrigin + uv * TerrainSize, and the screen-space metric divides by
// TerrainSize again — so an intermediate extent reproduces the same live count and the same
// projected edge as these two and only adds runtime. 512 m and 8 km bracket the range and
// still discriminate on what genuinely varies with extent: the fp32 headroom the relocation
// and cached-edit arms measure.
INSTANTIATE_TEST_SUITE_P(WorldExtent, CBTPlanarScaleAudit, ::testing::Values(512u, 8192u));
