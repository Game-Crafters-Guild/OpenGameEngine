// C4 metrics on a real GPU: the screen-space Classify metric (projected edge length
// vs a target pixel error, with a split/merge hysteresis band) and the VertexEval
// height displacement (evaluated corner Y == the height source scaled/offset). The
// depth-target machinery tests live in CBTInstanceTests; these drive the camera- and
// heightmap-dependent C4 paths against hand-computed expectations.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "CBTTerrain/CBTInstance.h"
#include "CBTTerrain/CBTKernelSet.h"
#include "CBTTerrain/CBTLayout.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Core/CommandList.h"
#include "Terrain/Heightfield.h"
#include "CBTTestHarness.h"

#include <algorithm> // std::sort/min/max (near-patch scan)

using namespace GameEngine::CBTTerrain;
using namespace GameEngine::CBTTerrain::Test;
using namespace GameEngine::Rendering;

namespace
{
constexpr uint32_t kRootIndexCount = kRootHalfedgeCount * 3u; // 6
// S2a 96B CBTVertexData (sector tail appended; corner/meta word offsets unchanged).
constexpr uint32_t kVertexWordsPerSlot = sizeof(CBTVertexData) / 4u; // 24

// Orthographic top-down viewProj: clip.x = a*worldX, clip.y = a*worldZ, w = 1. World
// XZ maps linearly to NDC (ignoring height), so a projected edge's pixel length is
// hand-computable. Column-major float[16] (GLSL mat4*vec4 is column-major).
void MakeOrthoTopDown(float a, float outViewProj[16])
{
    for (int i = 0; i < 16; ++i)
        outViewProj[i] = 0.0f;
    outViewProj[0] = a;   // col0.x -> clip.x picks up a*worldX
    outViewProj[9] = a;   // col2.y -> clip.y picks up a*worldZ
    outViewProj[15] = 1.0f; // clip.w = 1
}

// Pixel coordinate of a world XZ point under MakeOrthoTopDown(a) + a WxH viewport.
std::array<float, 2> ProjectPixels(float a, float worldX, float worldZ, float w, float h)
{
    const float ndcX = a * worldX;
    const float ndcZ = a * worldZ;
    return {(ndcX * 0.5f + 0.5f) * w, (ndcZ * 0.5f + 0.5f) * h};
}

CBTFrameParams MakeParams(const float viewProj[16], float sizeX, float sizeZ, float originX,
                          float originZ, float heightScale, float originY, float viewW,
                          float viewH, float splitPx, float mergePx)
{
    CBTFrameParams p{};
    // Origin inactive (RenderOriginSector default 0): viewProjRel == the world viewProj, so
    // the screen-space + frustum metrics run the byte-identical pre-slice path (dark-ship).
    std::memcpy(p.ViewProjRel, viewProj, sizeof(p.ViewProjRel));
    p.Screen[0] = viewW;
    p.Screen[1] = viewH;
    p.Screen[2] = splitPx;
    p.Screen[3] = mergePx;
    p.TerrainSize[0] = sizeX;
    p.TerrainSize[1] = sizeZ;
    p.TerrainSize[2] = heightScale;
    p.TerrainSize[3] = originY;
    p.TerrainOrigin[0] = originX;
    p.TerrainOrigin[1] = originZ;
    p.TerrainOrigin[2] = 0.0f; // maxDepth filled by RunScreenSpace
    p.TerrainOrigin[3] = 0.0f;
    return p;
}

uint32_t RunScreenSpace(IDevice& device, CBTInstance& instance, CBTFrameParams params,
                        uint32_t maxDepth, uint32_t frameIndex)
{
    params.TerrainOrigin[2] = static_cast<float>(maxDepth);
    CBTClassifyDesc classify{};
    classify.Mode = kClassifyScreenSpace;
    classify.TargetDepth = maxDepth; // cap
    auto cl = device.CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    instance.RecordUpdate(*cl, classify, params, frameIndex);
    instance.RecordReadback(*cl);
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    device.ExecuteCommandLists(lists);
    device.WaitForIdle();
    return instance.ReadDrawIndexCount(kDrawStreamAll);
}

// Screen-space update with a caller-supplied classify (so the C5 tests can drive the
// dirty-region rect); the maxDepth cap rides in classify.TargetDepth (mirrored into
// terrainOrigin.z, matching RunScreenSpace). Returns the ALL-stream draw count.
uint32_t RunClassify(IDevice& device, CBTInstance& instance, CBTFrameParams params,
                     const CBTClassifyDesc& classify, uint32_t frameIndex)
{
    params.TerrainOrigin[2] = static_cast<float>(classify.TargetDepth);
    auto cl = device.CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    instance.RecordUpdate(*cl, classify, params, frameIndex);
    instance.RecordReadback(*cl);
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    device.ExecuteCommandLists(lists);
    device.WaitForIdle();
    return instance.ReadDrawIndexCount(kDrawStreamAll);
}

// NxN R32_FLOAT height texture filled with a constant baseline, staged through an
// upload buffer (so a later band poke can rewrite a sub-region) and settled in
// ShaderResource. Caller destroys.
TextureHandle MakeHeightTextureN(IDevice& device, uint32_t n, float baseline)
{
    TextureDesc td{};
    td.width = n;
    td.height = n;
    td.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) |
               static_cast<uint32_t>(TextureUsage::TransferDst);
    td.persistent = true;
    td.debugName = "CBT.Test.HeightN";
    TextureHandle tex = device.CreateTexture(td);
    if (!tex.IsValid())
        return tex;

    const std::vector<float> data(static_cast<size_t>(n) * n, baseline);
    const size_t bytes = data.size() * sizeof(float);
    const size_t rowPitch = static_cast<size_t>(n) * sizeof(float);
    BufferHandle staging = device.CreateUploadBuffer(bytes, "CBT.Test.HeightStaging");
    device.UpdateBuffer(staging, 0, bytes, data.data());

    auto cl = device.CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    // Fresh texture: whole-subresource copy from UNDEFINED (discard-tolerant).
    cl->CopyBufferToTextureSubresource(staging, tex, 0, 0, n, n, 0, rowPitch, 1, 0, 0, 0,
                                       ResourceState::Undefined);
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(tex, ResourceState::CopyDest,
                                                      ResourceState::ShaderResource));
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    device.ExecuteCommandLists(lists);
    device.WaitForIdle();
    device.DestroyBuffer(staging);
    return tex;
}

// Rewrite rows [minZ, maxZ) of a ShaderResource-resting R32_FLOAT texture to `value`
// — the E2 band-upload shape (content-preserving from the resting layout). The rest
// of the texture keeps its contents.
void PokeHeightBand(IDevice& device, TextureHandle tex, uint32_t n, uint32_t minZ, uint32_t maxZ,
                    float value)
{
    const uint32_t rows = maxZ - minZ;
    const std::vector<float> band(static_cast<size_t>(rows) * n, value);
    const size_t bytes = band.size() * sizeof(float);
    const size_t rowPitch = static_cast<size_t>(n) * sizeof(float);
    BufferHandle staging = device.CreateUploadBuffer(bytes, "CBT.Test.PokeStaging");
    device.UpdateBuffer(staging, 0, bytes, band.data());

    auto cl = device.CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->CopyBufferToTextureSubresource(staging, tex, 0, 0, n, rows, 0, rowPitch, 1, 0, 0, minZ,
                                       ResourceState::ShaderResource);
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(tex, ResourceState::CopyDest,
                                                      ResourceState::ShaderResource));
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    device.ExecuteCommandLists(lists);
    device.WaitForIdle();
    device.DestroyBuffer(staging);
}

// CPU mirror of CBT_TriangleInFrustum (cbt_layout.glsl): a triangle is culled only
// when all three corners fall strictly outside one Gribb-Hartmann plane extracted from
// the column-major world->clip matrix. Must match the shader so the oracle below tests
// the SAME predicate the GPU binned on.
bool TriangleInFrustum(const float vp[16], const std::array<float, 3>& a,
                       const std::array<float, 3>& b, const std::array<float, 3>& c)
{
    // row_i = (vp[0*4+i], vp[1*4+i], vp[2*4+i], vp[3*4+i]) — column-major float[16].
    auto row = [&](int i) {
        return std::array<float, 4>{vp[0 * 4 + i], vp[1 * 4 + i], vp[2 * 4 + i], vp[3 * 4 + i]};
    };
    const auto r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    std::array<std::array<float, 4>, 6> planes = {{
        {r3[0] + r0[0], r3[1] + r0[1], r3[2] + r0[2], r3[3] + r0[3]}, // left
        {r3[0] - r0[0], r3[1] - r0[1], r3[2] - r0[2], r3[3] - r0[3]}, // right
        {r3[0] + r1[0], r3[1] + r1[1], r3[2] + r1[2], r3[3] + r1[3]}, // bottom
        {r3[0] - r1[0], r3[1] - r1[1], r3[2] - r1[2], r3[3] - r1[3]}, // top
        {r3[0] + r2[0], r3[1] + r2[1], r3[2] + r2[2], r3[3] + r2[3]}, // near
        {r3[0] - r2[0], r3[1] - r2[1], r3[2] - r2[2], r3[3] - r2[3]}, // far
    }};
    auto side = [](const std::array<float, 4>& pl, const std::array<float, 3>& p) {
        return pl[0] * p[0] + pl[1] * p[1] + pl[2] * p[2] + pl[3];
    };
    for (const auto& pl : planes)
    {
        if (side(pl, a) < 0.0f && side(pl, b) < 0.0f && side(pl, c) < 0.0f)
            return false;
    }
    return true;
}

// 1x1 (or NxN) R32_FLOAT texture cleared to a constant R value, settled in
// ShaderResource — a deterministic height source for the displacement test.
TextureHandle MakeConstantHeightTexture(IDevice& device, float value)
{
    TextureDesc td{};
    td.width = 4;
    td.height = 4;
    td.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) |
               static_cast<uint32_t>(TextureUsage::TransferDst);
    td.persistent = true;
    td.debugName = "CBT.Test.Height";
    TextureHandle tex = device.CreateTexture(td);
    if (!tex.IsValid())
        return tex;

    auto cl = device.CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(tex, ResourceState::Undefined,
                                                      ResourceState::CopyDest));
    const float clearValue[4] = {value, 0.0f, 0.0f, 0.0f};
    cl->ClearColorImageSubresource(tex, 0, 0, clearValue);
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(tex, ResourceState::CopyDest,
                                                      ResourceState::ShaderResource));
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    device.ExecuteCommandLists(lists);
    device.WaitForIdle();
    return tex;
}

// dim x dim R32_FLOAT texture uploaded from `data` (row-major), settled in ShaderResource.
// Used to stage a hand-packed resident-window atlas for the Phase E GPU-sample oracles.
TextureHandle MakeR32TextureFromData(IDevice& device, uint32_t dim, const std::vector<float>& data)
{
    TextureDesc td{};
    td.width = dim;
    td.height = dim;
    td.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) |
               static_cast<uint32_t>(TextureUsage::TransferDst);
    td.persistent = true;
    td.debugName = "CBT.Test.Atlas";
    TextureHandle tex = device.CreateTexture(td);
    if (!tex.IsValid())
        return tex;
    const size_t bytes = data.size() * sizeof(float);
    const size_t rowPitch = static_cast<size_t>(dim) * sizeof(float);
    BufferHandle staging = device.CreateUploadBuffer(bytes, "CBT.Test.AtlasStaging");
    device.UpdateBuffer(staging, 0, bytes, data.data());
    auto cl = device.CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->CopyBufferToTextureSubresource(staging, tex, 0, 0, dim, dim, 0, rowPitch, 1, 0, 0, 0,
                                       ResourceState::Undefined);
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(tex, ResourceState::CopyDest,
                                                      ResourceState::ShaderResource));
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    device.ExecuteCommandLists(lists);
    device.WaitForIdle();
    device.DestroyBuffer(staging);
    return tex;
}

// Set the Phase E atlas geometry in the frame params (mirror CBTRenderFeature::BuildFrameParams).
// AtlasParams1.w carries the coarse field's texels/axis (binding 19), not a flat height (the R1 fix).
void SetAtlasParams(CBTFrameParams& p, uint32_t atlasDim, uint32_t slotStride, uint32_t slotsPerRow,
                    uint32_t tileRes, uint32_t tilesX, uint32_t tilesZ, uint32_t coarseDim)
{
    p.AtlasParams0[0] = static_cast<float>(atlasDim);
    p.AtlasParams0[1] = static_cast<float>(slotStride);
    p.AtlasParams0[2] = static_cast<float>(slotsPerRow);
    p.AtlasParams0[3] = static_cast<float>(tileRes);
    p.AtlasParams1[0] = static_cast<float>(tilesX);
    p.AtlasParams1[1] = static_cast<float>(tilesZ);
    p.AtlasParams1[2] = 1.0f; // enabled
    p.AtlasParams1[3] = static_cast<float>(coarseDim);
}

// One live corner's evaluated height (corner.y) + its topological uv (meta.[k], corner.w), read
// from the ALL index stream + CurrentVertex SSBO. `allCount` is the ALL-stream triangle
// count (ReadDrawIndexCount(kDrawStreamAll)/3).
struct CornerYUv
{
    float Y;
    float UvY;
    float UvX;
};
std::vector<CornerYUv> ReadLiveCorners(CBTInstance& instance, uint32_t allCount)
{
    std::vector<CornerYUv> out;
    if (allCount == 0u)
        return out;
    const std::vector<uint32_t> indices =
        instance.DebugReadWords(CBTBinding::IndicesAll, allCount);
    uint32_t maxSlot = 0;
    for (uint32_t s : indices)
        maxSlot = std::max(maxSlot, s);
    const std::vector<uint32_t> verts =
        instance.DebugReadWords(CBTBinding::CurrentVertex, (maxSlot + 1u) * kVertexWordsPerSlot);
    auto asFloat = [](uint32_t bits) { float f; std::memcpy(&f, &bits, sizeof(f)); return f; };
    for (uint32_t slot : indices)
    {
        const uint32_t base = slot * kVertexWordsPerSlot;
        if (base + 15u >= verts.size())
            continue;
        for (uint32_t k = 0; k < 3u; ++k)
            out.push_back({asFloat(verts[base + k * 4u + 1u]), asFloat(verts[base + 12u + k]),
                           asFloat(verts[base + k * 4u + 3u])});
    }
    return out;
}

// Whole-pool + near-disc split-edge scan for the planar view-priority oracle. Reads every live
// bisector's split edge (corner0..corner2) and reports the whole-pool median plus the median
// restricted to bisectors whose edge midpoint is within `discRadius` of `center` (world XZ; the
// planar patch is flat so Y is ignored). Mirrors the sphere probe's ScanNearPatch.
struct PlanarScan
{
    uint32_t Live = 0;
    uint32_t NearLive = 0;
    double MedEdge = 0.0;
    double NearMedEdge = 0.0;
};
PlanarScan ScanPlanar(CBTInstance& instance, float centerX, float centerZ, double discRadius)
{
    const auto heap = instance.DebugReadWords(CBTBinding::HeapID, kDefaultBisectorPoolSize * 2u);
    const auto verts = instance.DebugReadWords(CBTBinding::CurrentVertex,
                                                     kDefaultBisectorPoolSize * kVertexWordsPerSlot);
    std::vector<double> all, near;
    all.reserve(8192);
    const double r2 = discRadius * discRadius;
    auto asFloat = [](uint32_t bits) { float f; std::memcpy(&f, &bits, sizeof(f)); return f; };
    for (uint32_t slot = 0; slot < kDefaultBisectorPoolSize; ++slot)
    {
        if (heap[slot * 2u] == 0u && heap[slot * 2u + 1u] == 0u)
            continue;
        const uint32_t base = slot * kVertexWordsPerSlot;
        const float c0x = asFloat(verts[base + 0u]), c0y = asFloat(verts[base + 1u]),
                    c0z = asFloat(verts[base + 2u]);
        const float c2x = asFloat(verts[base + 8u]), c2y = asFloat(verts[base + 9u]),
                    c2z = asFloat(verts[base + 10u]);
        const double dx = c0x - c2x, dy = c0y - c2y, dz = c0z - c2z;
        const double edge = std::sqrt(dx * dx + dy * dy + dz * dz);
        all.push_back(edge);
        const double mx = 0.5 * (c0x + c2x) - centerX, mz = 0.5 * (c0z + c2z) - centerZ;
        if (mx * mx + mz * mz <= r2)
            near.push_back(edge);
    }
    std::sort(all.begin(), all.end());
    std::sort(near.begin(), near.end());
    PlanarScan s{};
    s.Live = static_cast<uint32_t>(all.size());
    s.NearLive = static_cast<uint32_t>(near.size());
    s.MedEdge = all.empty() ? 0.0 : all[all.size() / 2u];
    s.NearMedEdge = near.empty() ? 0.0 : near[near.size() / 2u];
    return s;
}
} // namespace

class CBTScreenSpaceTest : public ::testing::Test
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
    std::unique_ptr<IDevice> m_Device;
    CBTKernelSet m_KernelSet;
};

TEST_F(CBTScreenSpaceTest, FirstGatedFrameInitializesCornersBeforeCulling)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());
    float ortho[16];
    MakeOrthoTopDown(1.0f, ortho);
    const auto params = MakeParams(ortho, 1.0f, 1.0f, 4096.0f, 4096.0f, 0.0f, 0.0f,
                                   1024.0f, 1024.0f, 10000.0f, 5000.0f);
    CBTClassifyDesc classify{};
    classify.Mode = kClassifyScreenSpace;
    classify.TargetDepth = kDefaultBaseDepth;
    classify.GateVertexEval = 1u;
    EXPECT_EQ(RunClassify(*m_Device, instance, params, classify, 0u), kRootIndexCount);
    EXPECT_EQ(instance.ReadDrawIndexCount(kDrawStreamVisible), 0u)
        << "first-frame culling used the zero-filled corners at the origin";
    const auto vertices = instance.DebugReadWords(CBTBinding::CurrentVertex, kVertexWordsPerSlot);
    ASSERT_EQ(vertices.size(), kVertexWordsPerSlot);
    float x = 0.0f;
    std::memcpy(&x, vertices.data(), sizeof(x));
    EXPECT_GE(x, 4096.0f) << "gated first use left the root geometry uninitialized";
    EXPECT_EQ(instance.ReadValidationErrorCount(), 0u);
}

TEST_F(CBTScreenSpaceTest, ForcedTerrainMoveRefreshesVisibilityInSameFrame)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());
    float ortho[16];
    MakeOrthoTopDown(1.0f, ortho);
    auto params = MakeParams(ortho, 1.0f, 1.0f, 4096.0f, 4096.0f, 0.0f, 0.0f,
                              1024.0f, 1024.0f, 10000.0f, 5000.0f);
    CBTClassifyDesc classify{};
    classify.Mode = kClassifyScreenSpace;
    classify.TargetDepth = kDefaultBaseDepth;
    RunClassify(*m_Device, instance, params, classify, 0u);
    RunClassify(*m_Device, instance, params, classify, 1u);
    ASSERT_EQ(instance.ReadDrawIndexCount(kDrawStreamVisible), 0u);

    // Move both roots into view without changing their topology. A force refresh must update
    // the culling inputs as well as the final vertex positions; otherwise this frame has a hole.
    params.TerrainOrigin[0] = -0.5f;
    params.TerrainOrigin[1] = -0.5f;
    EXPECT_EQ(RunClassify(*m_Device, instance, params, classify, 2u), kRootIndexCount);
    EXPECT_EQ(instance.ReadDrawIndexCount(kDrawStreamVisible), kRootIndexCount);
    EXPECT_EQ(instance.ReadValidationErrorCount(), 0u);

    params.TerrainOrigin[0] = 4096.0f;
    params.TerrainOrigin[1] = 4096.0f;
    RunClassify(*m_Device, instance, params, classify, 3u);
    EXPECT_EQ(instance.ReadDrawIndexCount(kDrawStreamVisible), 0u);
}

// Crack lock (plan §8 C4 seam fix): every corner instance that is the SAME logical
// LEB vertex must evaluate to a BIT-IDENTICAL position across all bisectors that
// reference it. Neighbours reach a shared vertex via different bit-paths; if the
// decode accumulated float rounding differently per path, the ulp-different UVs would
// sample ulp-different heights and open sky-coloured cracks under displacement (C3's
// flat Y=0 masked this). The integer decode makes shared corners bit-exact. Identity
// terrain params make each corner's world XZ equal its LEB UV, so this asserts the
// decode exactness directly (flat height -> the crack root cause is the UV).
TEST_F(CBTScreenSpaceTest, SharedCornersAreBitIdentical)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    float ortho[16];
    MakeOrthoTopDown(1.0f, ortho); // camera unused (depth-target), any valid matrix
    CBTFrameParams idp = MakeParams(ortho, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1024.0f, 1024.0f,
                                    8.0f, 4.0f);

    // Deterministic depth-target refinement of root 0 to a moderate depth (+ the
    // conformance gradient into root 1) — plenty of interior shared vertices reached
    // via distinct paths, none too deep for fp32 exactness.
    CBTClassifyDesc refine{};
    refine.Mode = kClassifyDepthTarget;
    refine.FocusRoot = 0u;
    refine.TargetDepth = kDefaultBaseDepth + 5u;
    for (uint32_t f = 0; f < 8u; ++f)
    {
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        instance.RecordUpdate(*cl, refine, idp, f);
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();
    }

    constexpr uint32_t kSlots = 64u;
    // Live slots (HeapID != 0). HeapID is u64 = 2 words/slot; read in batches.
    std::vector<uint64_t> heap(kSlots, 0);
    for (uint32_t base = 0; base < kSlots; base += 15u)
    {
        const uint32_t n = std::min(15u, kSlots - base);
        const auto w = instance.DebugReadWords(CBTBinding::HeapID, n * 2u, base * 2u);
        for (uint32_t i = 0; i < n; ++i)
            std::memcpy(&heap[base + i], &w[i * 2u], sizeof(uint64_t));
    }

    // Quantize world XZ (== UV here) to a grid far finer than the tessellation to key
    // "the same logical vertex", then assert every instance's raw float bits match.
    struct Bits
    {
        uint32_t X, Y, Z;
    };
    std::map<std::pair<int32_t, int32_t>, Bits> seen;
    uint32_t liveCorners = 0;
    uint32_t sharedHits = 0;
    for (uint32_t slot = 0; slot < kSlots; ++slot)
    {
        if (heap[slot] == 0ull)
            continue;
        const auto w = instance.DebugReadWords(CBTBinding::CurrentVertex, 12u, slot * kVertexWordsPerSlot);
        for (uint32_t k = 0; k < 3u; ++k)
        {
            Bits b{w[k * 4u + 0u], w[k * 4u + 1u], w[k * 4u + 2u]};
            float cx, cz;
            std::memcpy(&cx, &b.X, sizeof(float));
            std::memcpy(&cz, &b.Z, sizeof(float));
            const std::pair<int32_t, int32_t> key{static_cast<int32_t>(std::lround(cx * 65536.0f)),
                                                  static_cast<int32_t>(std::lround(cz * 65536.0f))};
            ++liveCorners;
            auto it = seen.find(key);
            if (it == seen.end())
            {
                seen.emplace(key, b);
            }
            else
            {
                ++sharedHits;
                EXPECT_EQ(it->second.X, b.X) << "shared corner X not bit-identical (crack) at slot " << slot;
                EXPECT_EQ(it->second.Y, b.Y) << "shared corner Y not bit-identical (crack) at slot " << slot;
                EXPECT_EQ(it->second.Z, b.Z) << "shared corner Z not bit-identical (crack) at slot " << slot;
            }
        }
    }
    EXPECT_GT(liveCorners, kRootHalfedgeCount * 3u) << "refinement never grew past the roots";
    EXPECT_GT(sharedHits, 0u) << "no shared vertices found — the test exercised nothing";
}

// VertexEval displaces each corner to the sampled terrain height: corner.y ==
// texel * heightScale + originY, and world XZ == origin + uv * size. Constant height
// texture (0.5) + known params -> hand-computed corner Y.
TEST_F(CBTScreenSpaceTest, HeightDisplacementMatchesHeightSource)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    constexpr float kTexel = 0.5f;
    constexpr float kHeightScale = 100.0f;
    constexpr float kOriginY = 10.0f;
    constexpr float kSizeX = 200.0f;
    constexpr float kSizeZ = 200.0f;
    constexpr float kOriginX = -100.0f;
    constexpr float kOriginZ = -100.0f;
    const float kExpectedY = kTexel * kHeightScale + kOriginY; // 60

    TextureHandle height = MakeConstantHeightTexture(*m_Device, kTexel);
    ASSERT_TRUE(height.IsValid());
    instance.SetHeightSource(0u, height); // frame 0 -> ring slot 0 (RecordUpdate below uses frameIndex 0)

    float ortho[16];
    MakeOrthoTopDown(1.0f, ortho); // camera unused (depth-target inert), any valid matrix
    CBTFrameParams params = MakeParams(ortho, kSizeX, kSizeZ, kOriginX, kOriginZ, kHeightScale,
                                       kOriginY, 1024.0f, 1024.0f, 8.0f, 4.0f);

    // Depth-target inert: roots only, so exactly the 2 root triangles are displaced.
    auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    instance.RecordUpdate(*cl, CBTInertClassify(), params, 0u);
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    m_Device->ExecuteCommandLists(lists);
    m_Device->WaitForIdle();

    // Root 0 (slot 0): 16 words/slot; corner k = (worldX, height, worldZ, uv.x).
    const auto w = instance.DebugReadWords(CBTBinding::CurrentVertex, 12u, 0u);
    for (uint32_t k = 0; k < 3u; ++k)
    {
        float cy = 0.0f, cx = 0.0f, cz = 0.0f;
        std::memcpy(&cx, &w[k * 4u + 0u], sizeof(float));
        std::memcpy(&cy, &w[k * 4u + 1u], sizeof(float));
        std::memcpy(&cz, &w[k * 4u + 2u], sizeof(float));
        EXPECT_NEAR(cy, kExpectedY, 1e-2f) << "corner " << k << " height not displaced";
        // World XZ stays inside the terrain bounds [origin, origin+size].
        EXPECT_GE(cx, kOriginX - 1e-2f);
        EXPECT_LE(cx, kOriginX + kSizeX + 1e-2f);
        EXPECT_GE(cz, kOriginZ - 1e-2f);
        EXPECT_LE(cz, kOriginZ + kSizeZ + 1e-2f);
    }

    m_Device->DestroyTexture(height);
}

// ---------------------------------------------------------------------------
// Phase E resident-window atlas — GPU sample oracles (design §3.3). VertexEval's
// height sample resolves terrain UV -> tile -> slot -> atlas texel through the
// indirection SSBO (binding 17) + atlas texture (binding 18) when atlas-enabled.
// ---------------------------------------------------------------------------
namespace
{
// Pack a tileRes^2 heightfield into a single (tileRes+2)^2 slot: interior at (1,1),
// 1-texel apron = edge duplication (so hardware bilinear at the slot edge is clamped).
std::vector<float> PackSingleSlot(uint32_t tileRes, const std::vector<float>& tile)
{
    const uint32_t dim = tileRes + 2u;
    std::vector<float> atlas(static_cast<size_t>(dim) * dim, 0.0f);
    auto at = [&](uint32_t ax, uint32_t az) -> float& { return atlas[az * dim + ax]; };
    for (uint32_t z = 0; z < tileRes; ++z)
        for (uint32_t x = 0; x < tileRes; ++x)
            at(1u + x, 1u + z) = tile[z * tileRes + x];
    for (uint32_t i = 0; i < tileRes; ++i)
    {
        at(0u, 1u + i) = tile[i * tileRes + 0u];              // left apron
        at(tileRes + 1u, 1u + i) = tile[i * tileRes + (tileRes - 1u)]; // right apron
        at(1u + i, 0u) = tile[0u * tileRes + i];              // top apron
        at(1u + i, tileRes + 1u) = tile[(tileRes - 1u) * tileRes + i]; // bottom apron
    }
    return atlas;
}

// A resident (slot 0) / non-resident (kAtlasNoSlot) 16-B indirection row as raw bytes.
std::vector<uint8_t> MakeRow(uint32_t slot, uint32_t generation)
{
    std::vector<uint8_t> row(16, 0);
    std::memcpy(row.data() + 0, &slot, 4);
    std::memcpy(row.data() + 4, &generation, 4);
    // LodBias (float, +8) and Pad (+12) stay 0.
    return row;
}
} // namespace

// The atlas path is TAKEN and correct: with the unified height source bound to a WRONG
// constant and the atlas holding a varying pattern, the evaluated root-corner heights must
// reflect the ATLAS (per-corner tile texel), never the unified constant. This both proves the
// UV->tile->slot->texel resolve at grid points and DISCRIMINATES the swap (disable it —
// AtlasParams1.z = 0 — and every corner collapses to the unified constant).
TEST_F(CBTScreenSpaceTest, AtlasResolveSamplesAtlasNotUnifiedHeight)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    constexpr uint32_t kTileRes = 9;
    constexpr uint32_t kApron = 1;
    constexpr uint32_t kDim = kTileRes + 2u * kApron; // 11
    constexpr float kHeightScale = 100.0f;
    constexpr float kOriginY = 10.0f;
    constexpr float kUnifiedConst = 0.9f; // the WRONG answer (0.9*100+10 = 100)

    // A pattern that differs at every corner so a per-corner match is meaningful.
    std::vector<float> tile(kTileRes * kTileRes);
    for (uint32_t z = 0; z < kTileRes; ++z)
        for (uint32_t x = 0; x < kTileRes; ++x)
            tile[z * kTileRes + x] = 0.10f + 0.08f * static_cast<float>(x) + 0.02f * static_cast<float>(z);

    const std::vector<float> atlasData = PackSingleSlot(kTileRes, tile);
    TextureHandle atlasTex = MakeR32TextureFromData(*m_Device, kDim, atlasData);
    TextureHandle unifiedTex = MakeConstantHeightTexture(*m_Device, kUnifiedConst);
    ASSERT_TRUE(atlasTex.IsValid() && unifiedTex.IsValid());

    instance.SetHeightSource(0u, unifiedTex);   // the unified path would give kUnifiedConst
    instance.SetAtlasSource(0u, atlasTex);      // the atlas path gives the pattern
    const std::vector<uint8_t> row = MakeRow(/*slot*/ 0u, /*gen*/ 1u);
    instance.UploadAtlasRows(0u, row.data(), 1u);

    float ortho[16];
    MakeOrthoTopDown(1.0f, ortho);
    CBTFrameParams params = MakeParams(ortho, 200.0f, 200.0f, -100.0f, -100.0f, kHeightScale,
                                       kOriginY, 1024.0f, 1024.0f, 8.0f, 4.0f);
    SetAtlasParams(params, kDim, kDim, /*slotsPerRow*/ 1u, kTileRes, /*tilesX*/ 1u, /*tilesZ*/ 1u,
                   /*coarseDim*/ 0u); // whole tile resident -> the coarse fallback is never taken

    auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    instance.RecordUpdate(*cl, CBTInertClassify(), params, 0u); // roots only
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    m_Device->ExecuteCommandLists(lists);
    m_Device->WaitForIdle();

    // Roots occupy slots 0 and 1; read both full slots. corner k: (x,y,z,uv.x) then meta.
    const auto w = instance.DebugReadWords(CBTBinding::CurrentVertex, 2u * kVertexWordsPerSlot);
    auto asFloat = [](uint32_t bits) { float f; std::memcpy(&f, &bits, sizeof(f)); return f; };
    uint32_t checked = 0, differsFromUnified = 0;
    for (uint32_t slot = 0; slot < 2u; ++slot)
    {
        const uint32_t base = slot * kVertexWordsPerSlot;
        for (uint32_t k = 0; k < 3u; ++k)
        {
            const float cy = asFloat(w[base + k * 4u + 1u]);
            const float uvx = asFloat(w[base + k * 4u + 3u]);
            const float uvy = asFloat(w[base + 12u + k]);
            // Grid-point corner UVs (0 or 1) map to tile texel 0 or tileRes-1.
            const uint32_t tx = uvx > 0.5f ? kTileRes - 1u : 0u;
            const uint32_t tz = uvy > 0.5f ? kTileRes - 1u : 0u;
            const float expected = tile[tz * kTileRes + tx] * kHeightScale + kOriginY;
            EXPECT_NEAR(cy, expected, 1e-2f)
                << "atlas corner (uv " << uvx << "," << uvy << ") height mismatch";
            if (std::abs(cy - (kUnifiedConst * kHeightScale + kOriginY)) > 1.0f)
                ++differsFromUnified;
            ++checked;
        }
    }
    EXPECT_GT(checked, 0u);
    EXPECT_GT(differsFromUnified, 0u)
        << "every corner equalled the unified constant — the atlas path was not taken (swap broken)";

    m_Device->DestroyTexture(atlasTex);
    m_Device->DestroyTexture(unifiedTex);
}

// Risk 3 / R1 (PR #506 fix) on the GPU: a kAtlasNoSlot indirection row resolves through the COARSE
// FIELD (binding 19), height-continuous with resident relief — not the flat representative height
// the dark-ship sampled (which cracked the frontier). The whole tile is non-resident, so every
// corner reads the coarse field's endpoint-exact texel at its UV. DISCRIMINATOR: the coarse field
// is a ramp, so corners at u=0 and u=1 read DIFFERENT heights — a flat fallback would collapse them
// all to one value (the shipped bug). Disabling the coarse bind (default 1x1 -> 0) reads 0 instead.
TEST_F(CBTScreenSpaceTest, AtlasNonResidentTileReadsCoarseField)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    constexpr uint32_t kTileRes = 9;
    constexpr uint32_t kDim = kTileRes + 2u;
    constexpr uint32_t kCoarseDim = 8u;
    constexpr float kHeightScale = 100.0f;
    constexpr float kOriginY = 10.0f;

    // A non-zero atlas so "read the coarse field" is distinguishable from "read a resident slot".
    std::vector<float> tile(kTileRes * kTileRes, 0.95f);
    const std::vector<float> atlasData = PackSingleSlot(kTileRes, tile);
    TextureHandle atlasTex = MakeR32TextureFromData(*m_Device, kDim, atlasData);

    // Coarse field: a ramp in u. Corner UVs are grid points (0 or 1) -> coarse texel 0 or dim-1
    // (the sample is endpoint-exact), so corners at u=0 read 0.2 and at u=1 read 0.7 — distinct.
    std::vector<float> coarse(static_cast<size_t>(kCoarseDim) * kCoarseDim);
    auto coarseVal = [](uint32_t x) { return 0.2f + 0.5f * static_cast<float>(x) / static_cast<float>(kCoarseDim - 1u); };
    for (uint32_t z = 0; z < kCoarseDim; ++z)
        for (uint32_t x = 0; x < kCoarseDim; ++x)
            coarse[static_cast<size_t>(z) * kCoarseDim + x] = coarseVal(x);
    TextureHandle coarseTex = MakeR32TextureFromData(*m_Device, kCoarseDim, coarse);
    ASSERT_TRUE(atlasTex.IsValid() && coarseTex.IsValid());

    instance.SetAtlasSource(0u, atlasTex);
    instance.SetCoarseSource(0u, coarseTex);
    // kAtlasNoSlot row -> the whole tile is out of the resident window -> coarse fallback.
    const std::vector<uint8_t> row = MakeRow(/*slot*/ 0xFFFFFFFFu, /*gen*/ 0u);
    instance.UploadAtlasRows(0u, row.data(), 1u);

    float ortho[16];
    MakeOrthoTopDown(1.0f, ortho);
    CBTFrameParams params = MakeParams(ortho, 200.0f, 200.0f, -100.0f, -100.0f, kHeightScale,
                                       kOriginY, 1024.0f, 1024.0f, 8.0f, 4.0f);
    SetAtlasParams(params, kDim, kDim, 1u, kTileRes, 1u, 1u, /*coarseDim*/ kCoarseDim);

    auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    instance.RecordUpdate(*cl, CBTInertClassify(), params, 0u);
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    m_Device->ExecuteCommandLists(lists);
    m_Device->WaitForIdle();

    const auto w = instance.DebugReadWords(CBTBinding::CurrentVertex, 2u * kVertexWordsPerSlot);
    auto asFloat = [](uint32_t bits) { float f; std::memcpy(&f, &bits, sizeof(f)); return f; };
    uint32_t sawLow = 0, sawHigh = 0; // corners at u=0 vs u=1 -> the ramp's ends (discrimination)
    for (uint32_t slot = 0; slot < 2u; ++slot)
        for (uint32_t k = 0; k < 3u; ++k)
        {
            const float cy = asFloat(w[slot * kVertexWordsPerSlot + k * 4u + 1u]);
            const float uvx = asFloat(w[slot * kVertexWordsPerSlot + k * 4u + 3u]);
            const uint32_t cx = uvx > 0.5f ? kCoarseDim - 1u : 0u;
            const float expected = coarseVal(cx) * kHeightScale + kOriginY;
            EXPECT_NEAR(cy, expected, 1e-2f) << "non-resident corner did not read the coarse field (u=" << uvx << ")";
            if (uvx < 0.5f)
                ++sawLow;
            else
                ++sawHigh;
        }
    EXPECT_GT(sawLow, 0u);
    EXPECT_GT(sawHigh, 0u) << "no u=1 corner sampled — the ramp discrimination did not exercise";
    // A flat fallback would read one constant at every corner; the ramp read above proves the field
    // varies with UV (the shipped flat-fallback bug the R1 fix removes).
    m_Device->DestroyTexture(atlasTex);
    m_Device->DestroyTexture(coarseTex);
}

namespace
{
// Pack tile `slot`'s heightfield into slot `slot` of a slotsPerRow x N atlas (interior at the slot
// origin + apron, 1-texel edge-duplication) — the multi-slot generalization of PackSingleSlot,
// matching CBT_AtlasSlotUV's slot origin (col*slotStride, row*slotStride).
void PackSlotInto(std::vector<float>& atlas, uint32_t atlasDim, uint32_t slotStride,
                  uint32_t slotsPerRow, uint32_t slot, uint32_t tileRes, const std::vector<float>& tile)
{
    const uint32_t ox = (slot % slotsPerRow) * slotStride;
    const uint32_t oy = (slot / slotsPerRow) * slotStride;
    auto at = [&](uint32_t ax, uint32_t az) -> float& { return atlas[static_cast<size_t>(az) * atlasDim + ax]; };
    for (uint32_t z = 0; z < tileRes; ++z)
        for (uint32_t x = 0; x < tileRes; ++x)
            at(ox + 1u + x, oy + 1u + z) = tile[z * tileRes + x];
    for (uint32_t i = 0; i < tileRes; ++i)
    {
        at(ox + 0u, oy + 1u + i) = tile[i * tileRes + 0u];
        at(ox + tileRes + 1u, oy + 1u + i) = tile[i * tileRes + (tileRes - 1u)];
        at(ox + 1u + i, oy + 0u) = tile[0u * tileRes + i];
        at(ox + 1u + i, oy + tileRes + 1u) = tile[(tileRes - 1u) * tileRes + i];
    }
}
} // namespace

// F6: a >=2x2-slot atlas — the coverage the single-slot oracles (slotsPerRow=1, slot 0) structurally
// cannot give. Slot 0 sits at col==row==0, where a col/row swap in CBT_AtlasSlotUV is a no-op. Here 4
// tiles map to 4 slots (slot = tz*2 + tx) so slots 1 and 2 have col!=row; each holds a DISTINCT base
// height. Every terrain corner must read ITS tile's slot. Under a col/row swap slots 1<->2 read each
// other's base -> the per-corner check fails (DISCRIMINATOR: swap interiorX/interiorY in
// CBT_AtlasSlotUV to falsify). Slot 3's row carries a COARSE LodBias to prove LodBias is a hint,
// never a height-sample input.
TEST_F(CBTScreenSpaceTest, AtlasResolve2x2DistinctSlotsCatchColRowSwap)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    constexpr uint32_t kTileRes = 9;
    constexpr uint32_t kApron = 1;
    constexpr uint32_t kSlotStride = kTileRes + 2u * kApron; // 11
    constexpr uint32_t kSlotsPerRow = 2;
    constexpr uint32_t kAtlasDim = kSlotsPerRow * kSlotStride; // 22
    constexpr float kHeightScale = 100.0f;
    constexpr float kOriginY = 10.0f;

    // Distinct base per slot; a small +x ramp so the corner lands on the slot's interior edge (not a
    // shared origin texel) — a within-slot addressing error diverges too.
    const float base[4] = {0.10f, 0.30f, 0.60f, 0.85f};
    auto tileFor = [&](uint32_t slot) {
        std::vector<float> t(kTileRes * kTileRes);
        for (uint32_t z = 0; z < kTileRes; ++z)
            for (uint32_t x = 0; x < kTileRes; ++x)
                t[z * kTileRes + x] =
                    base[slot] + 0.02f * static_cast<float>(x) / static_cast<float>(kTileRes - 1u);
        return t;
    };
    std::vector<float> atlas(static_cast<size_t>(kAtlasDim) * kAtlasDim, 0.0f);
    for (uint32_t s = 0; s < 4u; ++s)
        PackSlotInto(atlas, kAtlasDim, kSlotStride, kSlotsPerRow, s, kTileRes, tileFor(s));
    TextureHandle atlasTex = MakeR32TextureFromData(*m_Device, kAtlasDim, atlas);
    ASSERT_TRUE(atlasTex.IsValid());
    instance.SetAtlasSource(0u, atlasTex);

    // Row (tileIndex = tz*tilesX + tx = s) -> slot s. Slot 3 carries a coarse LodBias hint.
    std::vector<uint8_t> rows(4u * 16u, 0);
    for (uint32_t s = 0; s < 4u; ++s)
    {
        std::memcpy(rows.data() + s * 16u + 0u, &s, 4);
        const uint32_t gen = 1u;
        std::memcpy(rows.data() + s * 16u + 4u, &gen, 4);
        if (s == 3u)
        {
            const float lodCoarse = 1.0f;
            std::memcpy(rows.data() + s * 16u + 8u, &lodCoarse, 4);
        }
    }
    instance.UploadAtlasRows(0u, rows.data(), 4u);

    float ortho[16];
    MakeOrthoTopDown(1.0f, ortho);
    CBTFrameParams params = MakeParams(ortho, 200.0f, 200.0f, -100.0f, -100.0f, kHeightScale,
                                       kOriginY, 1024.0f, 1024.0f, 8.0f, 4.0f);
    SetAtlasParams(params, kAtlasDim, kSlotStride, kSlotsPerRow, kTileRes, /*tilesX*/ 2u, /*tilesZ*/ 2u,
                   /*coarseDim*/ 0u);

    auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    instance.RecordUpdate(*cl, CBTInertClassify(), params, 0u); // roots only -> the 4 terrain corners
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    m_Device->ExecuteCommandLists(lists);
    m_Device->WaitForIdle();

    const auto w = instance.DebugReadWords(CBTBinding::CurrentVertex, 2u * kVertexWordsPerSlot);
    auto asFloat = [](uint32_t bits) { float f; std::memcpy(&f, &bits, sizeof(f)); return f; };
    uint32_t checkedCorners = 0;
    bool sawSlot1 = false, sawSlot2 = false;
    for (uint32_t slot = 0; slot < 2u; ++slot)
        for (uint32_t k = 0; k < 3u; ++k)
        {
            const float cy = asFloat(w[slot * kVertexWordsPerSlot + k * 4u + 1u]);
            const float uvx = asFloat(w[slot * kVertexWordsPerSlot + k * 4u + 3u]);
            const float uvy = asFloat(w[slot * kVertexWordsPerSlot + 12u + k]);
            const uint32_t tx = uvx > 0.5f ? 1u : 0u;
            const uint32_t tz = uvy > 0.5f ? 1u : 0u;
            const uint32_t s = tz * 2u + tx;
            const float rampX = uvx > 0.5f ? 0.02f : 0.0f; // localU 1 -> ramp end, localU 0 -> ramp start
            const float expected = (base[s] + rampX) * kHeightScale + kOriginY;
            EXPECT_NEAR(cy, expected, 5e-2f) << "2x2 corner (uv " << uvx << "," << uvy << ") read slot "
                                             << s << " wrong (col/row swap or slot addressing)";
            if (s == 1u)
                sawSlot1 = true;
            if (s == 2u)
                sawSlot2 = true;
            ++checkedCorners;
        }
    EXPECT_GT(checkedCorners, 0u);
    EXPECT_TRUE(sawSlot1 && sawSlot2)
        << "the swap-sensitive slots (col!=row) were not both sampled — test not discriminating";
    m_Device->DestroyTexture(atlasTex);
}

namespace
{
// A cliff and a slope on a 33 x 33 lattice: every sample after column kCliffColumn stands
// kCliffRise higher, and the whole field rises linearly along z. Between two lattice samples the
// CPU height field reads their bilinear blend, so a sampler that blends at any other position
// lands up to the cliff's height away from it.
constexpr uint32_t kLatticeDim = 33u;
constexpr uint32_t kCliffColumn = 20u;
constexpr float kCliffRise = 0.6f;
constexpr float kSlopeRise = 0.3f;
constexpr float kLatticeTerrainSize = 256.0f;
constexpr float kLatticeHeightScale = 10.0f;
constexpr float kLatticeOriginY = 5.0f;

GameEngine::Terrain::HeightfieldData MakeCliffAndSlopeHeightfield()
{
    GameEngine::Terrain::HeightfieldData field(kLatticeDim, kLatticeDim);
    for (uint32_t z = 0; z < kLatticeDim; ++z)
        for (uint32_t x = 0; x < kLatticeDim; ++x)
            field.SetSample(x, z,
                            (x > kCliffColumn ? kCliffRise : 0.0f) +
                                kSlopeRise * static_cast<float>(z) / static_cast<float>(kLatticeDim - 1u));
    return field;
}

CBTFrameParams MakeLatticeTerrainParams()
{
    constexpr float kView = 1024.0f;
    float ortho[16];
    MakeOrthoTopDown(2.0f / kLatticeTerrainSize, ortho);
    return MakeParams(ortho, kLatticeTerrainSize, kLatticeTerrainSize, 0.0f, 0.0f, kLatticeHeightScale,
                      kLatticeOriginY, kView, kView, 8.0f, 4.0f);
}

struct HeightfieldAgreement
{
    uint32_t Corners = 0;
    uint32_t CliffCellCorners = 0; // corners strictly between the cliff's two lattice columns
    float MaxError = 0.0f;         // largest |rendered - CPU| over every live corner, metres
};

// Refines the terrain to the depth cap (4 m legs on 256 m, so corners fall on every lattice point
// and every cell centre), evaluating every corner each frame, then compares each live corner's
// height with the CPU height field at the corner's own UV.
HeightfieldAgreement RefineAndCompareWithHeightfield(IDevice& device, CBTInstance& instance,
                                                     const CBTFrameParams& params,
                                                     const GameEngine::Terrain::HeightfieldData& field)
{
    CBTClassifyDesc refine{};
    refine.Mode = kClassifyScreenSpace;
    refine.TargetDepth = 13u;
    refine.GateVertexEval = 0u;
    uint32_t previous = 0xFFFFFFFFu;
    uint32_t stable = 0;
    for (uint32_t frame = 0; frame < 48u && stable < 2u; ++frame)
    {
        const uint32_t count = RunClassify(device, instance, params, refine, frame);
        stable = (count == previous) ? stable + 1u : 0u;
        previous = count;
    }

    const float cellU = 1.0f / static_cast<float>(kLatticeDim - 1u);
    const float cliffU0 = static_cast<float>(kCliffColumn) * cellU;
    const float cliffU1 = cliffU0 + cellU;
    HeightfieldAgreement result;
    const uint32_t triangles = instance.ReadDrawIndexCount(kDrawStreamAll) / 3u;
    for (const CornerYUv& corner : ReadLiveCorners(instance, triangles))
    {
        const float expected =
            field.SampleBilinear(corner.UvX, corner.UvY) * kLatticeHeightScale + kLatticeOriginY;
        result.MaxError = std::max(result.MaxError, std::abs(corner.Y - expected));
        ++result.Corners;
        if (corner.UvX > cliffU0 && corner.UvX < cliffU1)
            ++result.CliffCellCorners;
    }
    return result;
}
} // namespace

// The rendered surface is the CPU height field on a single or unified height texture: every
// refined corner, on a lattice point or between two, sits at HeightfieldData::SampleBilinear's
// height for its UV, across a cliff and along a slope.
TEST_F(CBTScreenSpaceTest, RefinedCornersMatchTheCpuHeightfieldOnTheHeightTexture)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    const GameEngine::Terrain::HeightfieldData field = MakeCliffAndSlopeHeightfield();
    const std::vector<float> samples(field.GetRawSamples(), field.GetRawSamples() + field.GetSampleCount());
    TextureHandle height = MakeR32TextureFromData(*m_Device, kLatticeDim, samples);
    ASSERT_TRUE(height.IsValid());
    for (uint32_t s = 0; s < kCBTFrameParamsRing; ++s)
        instance.SetHeightSource(s, height);

    const HeightfieldAgreement agreement =
        RefineAndCompareWithHeightfield(*m_Device, instance, MakeLatticeTerrainParams(), field);
    EXPECT_GT(agreement.CliffCellCorners, 0u) << "no corner landed inside the cliff's cell";
    EXPECT_GT(agreement.Corners, 8192u) << "the tree did not refine to the depth cap";
    EXPECT_LT(agreement.MaxError, 1e-2f) << "a rendered corner is off the CPU height field";
    EXPECT_EQ(instance.ReadValidationCounter(kValidationErrorCounter), 0u);

    m_Device->DestroyTexture(height);
}

// The same agreement through the resident-window atlas: the field packed as one resident tile.
TEST_F(CBTScreenSpaceTest, RefinedCornersMatchTheCpuHeightfieldOnTheAtlas)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    const GameEngine::Terrain::HeightfieldData field = MakeCliffAndSlopeHeightfield();
    const std::vector<float> samples(field.GetRawSamples(), field.GetRawSamples() + field.GetSampleCount());
    constexpr uint32_t kSlotDim = kLatticeDim + 2u; // one-texel apron on each side
    TextureHandle atlasTex = MakeR32TextureFromData(*m_Device, kSlotDim, PackSingleSlot(kLatticeDim, samples));
    ASSERT_TRUE(atlasTex.IsValid());
    const std::vector<uint8_t> row = MakeRow(/*slot*/ 0u, /*gen*/ 1u);
    for (uint32_t s = 0; s < kCBTFrameParamsRing; ++s)
    {
        instance.SetAtlasSource(s, atlasTex);
        instance.UploadAtlasRows(s, row.data(), 1u);
    }

    CBTFrameParams params = MakeLatticeTerrainParams();
    SetAtlasParams(params, kSlotDim, kSlotDim, /*slotsPerRow*/ 1u, kLatticeDim, /*tilesX*/ 1u,
                   /*tilesZ*/ 1u, /*coarseDim*/ 0u);
    const HeightfieldAgreement agreement = RefineAndCompareWithHeightfield(*m_Device, instance, params, field);
    EXPECT_GT(agreement.CliffCellCorners, 0u) << "no corner landed inside the cliff's cell";
    EXPECT_GT(agreement.Corners, 8192u) << "the tree did not refine to the depth cap";
    EXPECT_LT(agreement.MaxError, 1e-2f) << "a rendered corner is off the CPU height field";
    EXPECT_EQ(instance.ReadValidationCounter(kValidationErrorCounter), 0u);

    m_Device->DestroyTexture(atlasTex);
}

// ---------------------------------------------------------------------------
// The paged height resolve (bindings 22 and 23, CBT_SampleHeight's AtlasParams1.z == 2 branch):
// a 129 x 129 field of two levels, one page each, in a cache of 2 x 2 slots of 130 x 130 texels.
// ---------------------------------------------------------------------------
namespace
{
constexpr uint32_t kPagedSamples = 129u;    // level 0: one page of 128 owned samples + its last
constexpr uint32_t kPageStride = 130u;      // mirror PageStreaming::kPageStrideSamples
constexpr uint32_t kPagedSlotsPerRow = 2u;
constexpr uint32_t kPagedSlotCount = 4u;
constexpr uint32_t kPagedCacheDim = kPagedSlotsPerRow * kPageStride;
constexpr uint32_t kPageEntryLevelShift = 24u; // mirror PageStreaming::kPageLevelShift

// The normalized height slot `slot` holds at its stride texel (x, z): a ramp distinct per slot.
float PagedSlotRamp(uint32_t slot, uint32_t x, uint32_t z)
{
    return 0.1f * static_cast<float>(slot) + 0.001f * static_cast<float>(x) + 0.002f * static_cast<float>(z);
}

std::vector<float> MakePagedCache()
{
    std::vector<float> cache(static_cast<size_t>(kPagedCacheDim) * kPagedCacheDim, 0.0f);
    for (uint32_t slot = 0; slot < kPagedSlotCount; ++slot)
    {
        const uint32_t originX = (slot % kPagedSlotsPerRow) * kPageStride;
        const uint32_t originZ = (slot / kPagedSlotsPerRow) * kPageStride;
        for (uint32_t z = 0; z < kPageStride; ++z)
            for (uint32_t x = 0; x < kPageStride; ++x)
                cache[(originZ + z) * kPagedCacheDim + originX + x] = PagedSlotRamp(slot, x, z);
    }
    return cache;
}

// The binding-22 words of the two-level field (CBTLayout.h's layout, what PackPageTableWords
// writes): level 0's one page in `level0Slot` with arrival fade `level0Fade`, level 1's in
// `level1Slot`, settled; every other slot settled.
std::vector<uint32_t> PackTwoLevelPageTable(uint32_t level0Slot, float level0Fade, uint32_t level1Slot)
{
    std::vector<uint32_t> words(kCBTPageFadeOffsetWords + kPagedSlotCount + 2u, 0u);
    words[0] = kPagedSamples;
    words[1] = kPagedSamples;
    words[2] = 2u; // level count
    words[3] = kPagedSlotsPerRow;
    words[4] = kPagedSlotCount;
    for (uint32_t level = 0; level < 2u; ++level)
    {
        uint32_t* shape = words.data() + kCBTPageHeaderWords + 4u * level;
        shape[0] = level; // first entry
        shape[1] = 1u;    // pages X
        shape[2] = 1u;    // pages Z
    }
    for (uint32_t slot = 0; slot < kPagedSlotCount; ++slot)
    {
        const float fade = slot == level0Slot ? level0Fade : 1.0f;
        std::memcpy(&words[kCBTPageFadeOffsetWords + slot], &fade, sizeof(fade));
    }
    words[kCBTPageFadeOffsetWords + kPagedSlotCount + 0u] = level0Slot | (0u << kPageEntryLevelShift);
    words[kCBTPageFadeOffsetWords + kPagedSlotCount + 1u] = level1Slot | (1u << kPageEntryLevelShift);
    return words;
}

// The normalized height the two-level table resolves at a root corner (terrain UV 0 or 1 on each
// axis): level 0's lattice is 0 or 128, level 1's 0 or 64, each offset by the one-texel apron.
float ExpectedPagedCorner(float u, float v, uint32_t level0Slot, float level0Fade, uint32_t level1Slot)
{
    const uint32_t x0 = u > 0.5f ? kPagedSamples : 1u;
    const uint32_t z0 = v > 0.5f ? kPagedSamples : 1u;
    const uint32_t x1 = u > 0.5f ? (kPagedSamples - 1u) / 2u + 1u : 1u;
    const uint32_t z1 = v > 0.5f ? (kPagedSamples - 1u) / 2u + 1u : 1u;
    return level0Fade * PagedSlotRamp(level0Slot, x0, z0) + (1.0f - level0Fade) * PagedSlotRamp(level1Slot, x1, z1);
}

// Uploads two different page tables into frame slots 0 and 1 of the instance's page-table ring,
// runs VertexEval in each frame and expects every root corner on its own frame's table: frame 0
// resolves level 0 settled in slot 1; frame 1 level 0 half-faded in slot 3 over level 1 in slot 2.
void ExpectEachFrameResolvesItsOwnPageTable(IDevice& device, CBTInstance& instance, TextureHandle cacheTex)
{
    struct FrameTable
    {
        uint32_t Level0Slot;
        float Level0Fade;
        uint32_t Level1Slot;
    };
    constexpr std::array<FrameTable, 2> kTables{{{1u, 1.0f, 2u}, {3u, 0.5f, 2u}}};
    for (uint32_t frame = 0; frame < kTables.size(); ++frame)
    {
        const FrameTable& t = kTables[frame];
        instance.UploadPageTable(frame, PackTwoLevelPageTable(t.Level0Slot, t.Level0Fade, t.Level1Slot));
        instance.SetPageCacheSource(frame, cacheTex);
    }

    constexpr float kHeightScale = 100.0f;
    constexpr float kOriginY = 10.0f;
    float ortho[16];
    MakeOrthoTopDown(1.0f, ortho);
    CBTFrameParams params = MakeParams(ortho, 200.0f, 200.0f, -100.0f, -100.0f, kHeightScale, kOriginY,
                                       1024.0f, 1024.0f, 8.0f, 4.0f);
    params.AtlasParams1[2] = 2.0f; // the paged resolve (CBTRenderFeature::BuildFrameParams)

    auto asFloat = [](uint32_t bits) { float f; std::memcpy(&f, &bits, sizeof(f)); return f; };
    for (uint32_t frame = 0; frame < kTables.size(); ++frame)
    {
        auto cl = device.CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        instance.RecordUpdate(*cl, CBTInertClassify(), params, frame); // roots only
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        device.ExecuteCommandLists(lists);
        device.WaitForIdle();

        const FrameTable& t = kTables[frame];
        const auto w = instance.DebugReadWords(CBTBinding::CurrentVertex, 2u * kVertexWordsPerSlot);
        for (uint32_t slot = 0; slot < 2u; ++slot)
        {
            const uint32_t base = slot * kVertexWordsPerSlot;
            for (uint32_t k = 0; k < 3u; ++k)
            {
                const float cy = asFloat(w[base + k * 4u + 1u]);
                const float u = asFloat(w[base + k * 4u + 3u]);
                const float v = asFloat(w[base + 12u + k]);
                const float expected =
                    ExpectedPagedCorner(u, v, t.Level0Slot, t.Level0Fade, t.Level1Slot) * kHeightScale + kOriginY;
                EXPECT_NEAR(cy, expected, 1e-2f) << "frame " << frame << " corner (uv " << u << "," << v << ")";
            }
        }
    }
}

} // namespace

// The production paged path: page tables uploaded through CBTInstance::UploadPageTable into two
// frame slots of the ring with DIFFERENT contents, the cache bound through SetPageCacheSource, and
// VertexEval's root corners read back, in the ring's first slots and again after the ring grew for a
// larger terrain's table (the shader finds a frame's slot from the words per slot its frame params
// carry).
// Every slot holds its own ramp, so a shader reading another frame's ring slot resolves another
// table's slots and lands on another height.
TEST_F(CBTScreenSpaceTest, PagedResolveReadsThisFramesPageTableAndCache)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    TextureHandle cacheTex = MakeR32TextureFromData(*m_Device, kPagedCacheDim, MakePagedCache());
    ASSERT_TRUE(cacheTex.IsValid());

    ExpectEachFrameResolvesItsOwnPageTable(*m_Device, instance, cacheTex);
    ASSERT_TRUE(instance.ProvisionPageTableWords(kCBTPageTableMinRingWords + 1u));
    ExpectEachFrameResolvesItsOwnPageTable(*m_Device, instance, cacheTex);
    EXPECT_EQ(instance.ReadValidationCounter(kValidationErrorCounter), 0u);

    m_Device->DestroyTexture(cacheTex);
}

// Coarse on screen -> split. The two root triangles' split-edge projects to a known
// pixel length; a split threshold BELOW it must make Classify enqueue both roots.
TEST_F(CBTScreenSpaceTest, SplitsWhenEdgeExceedsThreshold)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    constexpr float kSize = 256.0f;
    constexpr float kView = 1024.0f;
    const float a = 2.0f / kSize;
    float ortho[16];
    MakeOrthoTopDown(a, ortho);

    // Root split edge is (corner0=(size,0), corner2=(0,size)) in world XZ.
    const auto p0 = ProjectPixels(a, kSize, 0.0f, kView, kView);
    const auto p2 = ProjectPixels(a, 0.0f, kSize, kView, kView);
    const float expectedPx = std::hypot(p0[0] - p2[0], p0[1] - p2[1]);
    ASSERT_GT(expectedPx, 1.0f);

    // Threshold well below the projected edge -> too coarse -> split.
    CBTFrameParams params = MakeParams(ortho, kSize, kSize, 0.0f, 0.0f, 0.0f, 0.0f, kView, kView,
                                       expectedPx * 0.5f, expectedPx * 0.25f);

    // Initial geometry is refreshed before Classify, so the first frame already sees both roots.
    const uint32_t draw = RunScreenSpace(*m_Device, instance, params, /*maxDepth=*/12u, 0u);

    EXPECT_EQ(instance.ReadWorkQueueCounter(kWQSplitCounter), 2)
        << "both roots should split when the projected edge exceeds the threshold";
    EXPECT_GT(draw, kRootIndexCount) << "the tree did not refine under the screen-space metric";
    EXPECT_EQ(instance.ReadValidationCounter(kValidationErrorCounter), 0u);
}

// Fine on screen -> hold. A split threshold ABOVE the projected root edge must leave
// the roots unsplit (and they cannot merge below the base), so the tree is inert.
TEST_F(CBTScreenSpaceTest, HoldsWhenEdgeBelowThreshold)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    constexpr float kSize = 256.0f;
    constexpr float kView = 1024.0f;
    const float a = 2.0f / kSize;
    float ortho[16];
    MakeOrthoTopDown(a, ortho);

    const auto p0 = ProjectPixels(a, kSize, 0.0f, kView, kView);
    const auto p2 = ProjectPixels(a, 0.0f, kSize, kView, kView);
    const float expectedPx = std::hypot(p0[0] - p2[0], p0[1] - p2[1]);

    // Threshold well above the projected edge -> already fine enough -> no split.
    CBTFrameParams params = MakeParams(ortho, kSize, kSize, 0.0f, 0.0f, 0.0f, 0.0f, kView, kView,
                                       expectedPx * 2.0f, expectedPx * 1.0f);

    RunScreenSpace(*m_Device, instance, params, 12u, 0u);
    const uint32_t draw = RunScreenSpace(*m_Device, instance, params, 12u, 1u);

    EXPECT_EQ(instance.ReadWorkQueueCounter(kWQSplitCounter), 0)
        << "roots should not split when the projected edge is under the threshold";
    EXPECT_EQ(draw, kRootIndexCount) << "the tree drifted off the roots with nothing to do";
    EXPECT_EQ(instance.ReadValidationCounter(kValidationErrorCounter), 0u);
}

// The hysteresis band gives work quiescence: a static camera + terrain must reach a
// stable density and then NOT churn (the #405 pattern). Then pulling the target error
// way up must merge the tree back toward the roots. Split, merge, and hysteresis in
// one deterministic sequence.
TEST_F(CBTScreenSpaceTest, ConvergesQuiescentThenMergesOnTargetChange)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    constexpr float kSize = 256.0f;
    constexpr float kView = 1024.0f;
    const float a = 2.0f / kSize;
    float ortho[16];
    MakeOrthoTopDown(a, ortho);

    // Refine: split at 32 px, merge below 16 px (a 2x hysteresis band). Uniform
    // ortho projection refines one level per frame, so run until the count settles
    // (two identical frames) rather than assuming a fixed frame budget.
    CBTFrameParams refine = MakeParams(ortho, kSize, kSize, 0.0f, 0.0f, 0.0f, 0.0f, kView, kView,
                                       32.0f, 16.0f);

    uint32_t frame = 0;
    uint32_t converged = 0;
    uint32_t prev = 0xFFFFFFFFu;
    uint32_t stable = 0;
    for (uint32_t i = 0; i < 32u && stable < 2u; ++i, ++frame)
    {
        converged = RunScreenSpace(*m_Device, instance, refine, 12u, frame);
        EXPECT_EQ(instance.ReadValidationCounter(kValidationErrorCounter), 0u) << "frame " << frame;
        stable = (converged == prev) ? (stable + 1u) : 0u;
        prev = converged;
    }
    EXPECT_GT(converged, kRootIndexCount) << "screen-space refinement never grew past the roots";
    EXPECT_GE(stable, 2u) << "screen-space refinement never settled to a fixed density";

    // Quiescence: identical params -> the count must NOT oscillate (hysteresis holds).
    for (uint32_t i = 0; i < 3u; ++i, ++frame)
    {
        const uint32_t draw = RunScreenSpace(*m_Device, instance, refine, 12u, frame);
        EXPECT_EQ(draw, converged) << "static camera churned the tree at frame " << frame
                                   << " — hysteresis band failed";
        EXPECT_EQ(instance.ReadValidationCounter(kValidationErrorCounter), 0u);
    }

    // Raise the target error far above every live edge -> the whole tree is too fine
    // -> it merges back toward the roots (one level per frame; run until it settles).
    CBTFrameParams coarse = MakeParams(ortho, kSize, kSize, 0.0f, 0.0f, 0.0f, 0.0f, kView, kView,
                                       1.0e6f, 1.0e6f);
    uint32_t merged = converged;
    for (uint32_t i = 0; i < 32u && merged != kRootIndexCount; ++i, ++frame)
    {
        merged = RunScreenSpace(*m_Device, instance, coarse, 12u, frame);
        EXPECT_EQ(instance.ReadValidationCounter(kValidationErrorCounter), 0u) << "merge frame " << frame;
    }
    EXPECT_EQ(merged, kRootIndexCount) << "too-fine tree did not merge back to the roots";
}

// C5 work-quiescence gate (plan §8 C5, the #405 pattern): once converged, a static
// camera with NO dirty region must produce zero split/merge/allocate work and an
// empty MODIFIED stream every frame. The reclassification path is gated on a
// non-empty dirty rect, so its absence guarantees quiescence — this asserts the
// counters directly (stronger than the draw-count-stable check above), which is the
// invariant an idle terrain must hold.
TEST_F(CBTScreenSpaceTest, NoDirtyRegionIsQuiescent)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    constexpr float kSize = 256.0f;
    constexpr float kView = 1024.0f;
    const float a = 2.0f / kSize;
    float ortho[16];
    MakeOrthoTopDown(a, ortho);
    CBTFrameParams params = MakeParams(ortho, kSize, kSize, 0.0f, 0.0f, 0.0f, 0.0f, kView, kView,
                                       32.0f, 16.0f);

    // Converge (empty dirty rect throughout — the default CBTClassifyDesc rect).
    CBTClassifyDesc converge{};
    converge.Mode = kClassifyScreenSpace;
    converge.TargetDepth = 12u;
    uint32_t frame = 0;
    uint32_t prev = 0xFFFFFFFFu;
    uint32_t stable = 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < 32u && stable < 2u; ++i, ++frame)
    {
        count = RunClassify(*m_Device, instance, params, converge, frame);
        stable = (count == prev) ? (stable + 1u) : 0u;
        prev = count;
    }
    ASSERT_GE(stable, 2u) << "screen-space refinement never settled";
    ASSERT_GT(count, kRootIndexCount) << "refinement never grew past the roots";

    // Quiescence window: no edit (empty rect) + static camera => zero work, empty
    // MODIFIED stream, stable draw count. The MODIFIED stream is the C5 tell — the
    // reclassification path must contribute nothing when nothing was edited.
    for (uint32_t i = 0; i < 4u; ++i, ++frame)
    {
        const uint32_t draw = RunClassify(*m_Device, instance, params, converge, frame);
        EXPECT_EQ(draw, count) << "static camera churned the tree at frame " << frame;
        EXPECT_EQ(instance.ReadWorkQueueCounter(kWQSplitCounter), 0) << "frame " << frame;
        EXPECT_EQ(instance.ReadWorkQueueCounter(kWQAllocateCounter), 0) << "frame " << frame;
        EXPECT_EQ(instance.ReadWorkQueueCounter(kWQSimplifyClassCounter), 0) << "frame " << frame;
        EXPECT_EQ(instance.ReadDrawIndexCount(kDrawStreamModified), 0u)
            << "an idle terrain flagged bisectors MODIFIED at frame " << frame;
        EXPECT_EQ(instance.ReadValidationCounter(kValidationErrorCounter), 0u) << "frame " << frame;
    }
}

// C5 live-edit proof (plan §8 C5, task items 1 + 4): refine over a known heightmap,
// poke a sub-region (the E2 band-upload shape) + mark it dirty, run ONE update, and
// assert (a) the overlapping bisectors were flagged MODIFIED (reclassification) and
// (b) VertexEval re-sampled the region — corners inside the poked band carry the new
// height, corners outside keep the old one. Ortho top-down ignores height, so the
// screen-space density is unchanged by the edit: this isolates the region path from
// camera-driven refinement.
TEST_F(CBTScreenSpaceTest, RegionEditReclassifiesAndRefreshesHeights)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    constexpr uint32_t kN = 8u;
    constexpr float kBaseline = 0.2f;
    constexpr float kPoke = 0.8f;
    constexpr float kHeightScale = 100.0f;
    constexpr float kOriginY = 0.0f;
    constexpr float kSize = 256.0f;
    constexpr float kView = 1024.0f;
    const float kBaseY = kBaseline * kHeightScale + kOriginY; // 20
    const float kPokeY = kPoke * kHeightScale + kOriginY;     // 80

    TextureHandle height = MakeHeightTextureN(*m_Device, kN, kBaseline);
    ASSERT_TRUE(height.IsValid());
    for (uint32_t s = 0; s < kCBTFrameParamsRing; ++s)
        instance.SetHeightSource(s, height); // any frame index samples the real texture

    const float a = 2.0f / kSize;
    float ortho[16];
    MakeOrthoTopDown(a, ortho);
    const auto p0 = ProjectPixels(a, kSize, 0.0f, kView, kView);
    const auto p2 = ProjectPixels(a, 0.0f, kSize, kView, kView);
    const float edgePx = std::hypot(p0[0] - p2[0], p0[1] - p2[1]);
    // Split well below the root edge -> a few levels of refinement -> plenty of live
    // corners spread across the UV square.
    CBTFrameParams params = MakeParams(ortho, kSize, kSize, 0.0f, 0.0f, kHeightScale, kOriginY,
                                       kView, kView, edgePx * 0.2f, edgePx * 0.1f);

    CBTClassifyDesc converge{};
    converge.Mode = kClassifyScreenSpace;
    converge.TargetDepth = 12u;
    uint32_t frame = 0;
    uint32_t prev = 0xFFFFFFFFu;
    uint32_t stable = 0;
    for (uint32_t i = 0; i < 24u && stable < 2u; ++i, ++frame)
    {
        const uint32_t c = RunClassify(*m_Device, instance, params, converge, frame);
        stable = (c == prev) ? (stable + 1u) : 0u;
        prev = c;
    }
    ASSERT_GE(stable, 2u) << "flat-height refinement never settled";

    // Poke the upper band (rows [4,8) -> uv.v in [0.5,1)) to kPoke and reclassify that
    // UV rect. The texture handle is unchanged (in-place band copy), so the bound
    // descriptor still resolves — VertexEval samples the new texels this update.
    PokeHeightBand(*m_Device, height, kN, kN / 2u, kN, kPoke);
    CBTClassifyDesc dirty = converge;
    dirty.DirtyMinU = 0.0f;
    dirty.DirtyMaxU = 1.0f;
    dirty.DirtyMinV = 0.5f;
    dirty.DirtyMaxV = 1.0f;
    RunClassify(*m_Device, instance, params, dirty, frame);
    ++frame;

    EXPECT_GT(instance.ReadDrawIndexCount(kDrawStreamModified), 0u)
        << "the edited region flagged no bisector MODIFIED — reclassification did not fire";
    EXPECT_EQ(instance.ReadValidationCounter(kValidationErrorCounter), 0u);

    // Read every live bisector's corners via the ALL index stream (authoritative live
    // list; slot reuse scatters live slots so a fixed prefix would miss some).
    const uint32_t liveCount = instance.ReadDrawIndexCount(kDrawStreamAll) / 3u;
    ASSERT_GT(liveCount, 0u);
    const std::vector<uint32_t> indices =
        instance.DebugReadWords(CBTBinding::IndicesAll, liveCount);
    uint32_t maxSlot = 0;
    for (uint32_t s : indices)
        maxSlot = std::max(maxSlot, s);
    const std::vector<uint32_t> verts =
        instance.DebugReadWords(CBTBinding::CurrentVertex, (maxSlot + 1u) * kVertexWordsPerSlot);

    auto asFloat = [](uint32_t bits) { float f; std::memcpy(&f, &bits, sizeof(f)); return f; };
    uint32_t pokedSeen = 0;
    uint32_t baseSeen = 0;
    for (uint32_t slot : indices)
    {
        const uint32_t base = slot * kVertexWordsPerSlot;
        if (base + 15u >= verts.size())
            continue;
        for (uint32_t k = 0; k < 3u; ++k)
        {
            const float uvY = asFloat(verts[base + 12u + k]); // meta.[xyz] = uv.y per corner
            const float cy = asFloat(verts[base + k * 4u + 1u]); // corner.y = sampled height
            if (uvY > 0.7f)
            {
                ++pokedSeen;
                EXPECT_NEAR(cy, kPokeY, 1e-2f) << "corner in the poked band kept the old height";
            }
            else if (uvY < 0.3f)
            {
                ++baseSeen;
                EXPECT_NEAR(cy, kBaseY, 1e-2f) << "corner outside the poke changed height";
            }
        }
    }
    EXPECT_GT(pokedSeen, 0u) << "no corners sampled inside the poked band";
    EXPECT_GT(baseSeen, 0u) << "no corners sampled outside the poked band";

    m_Device->DestroyTexture(height);
}

// E6 streaming re-eval gate — the SHADER-side machinery the fix relies on (this hand-feeds
// classify.Dirty* directly; it does NOT exercise the CPU-side publish plumbing — the
// accumulator + ConsumeUnifiedTiledDirtyRegion routing, covered by the CBTRegionConsume
// tests). The production steady state runs VertexEval GATED (GateVertexEval=1 — only
// bisectors flagged MODIFIED re-evaluate); this asserts that under that gate the dirty-rect
// channel is exactly what refreshes streamed / re-baked heights, in BOTH directions on one
// converged tree:
//   * CONTROL: gate on + NO dirty rect -> a height-texture change is invisible; the poked
//     band's corners stay frozen at the baseline and VERTEX_EVAL counts 0. This is the
//     corruption the fix cures — an empty published rect (what the pre-fix tiled path fed).
//   * FIX: publishing the matching UV rect flags the overlapping bisectors MODIFIED,
//     VertexEval re-samples EXACTLY them (counter == MODIFIED bisector count), their corners
//     heal to the poked height, and corners outside the band keep the baseline.
// This passes on origin/main (the gated re-sample already existed) — it locks the mechanism
// the CPU fix drives, not the CPU fix itself; that is what the CBTRegionConsume routing +
// discipline tests cover.
TEST_F(CBTScreenSpaceTest, GatedRegionEditRefreshesOnlyWithDirtyRect)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    constexpr uint32_t kN = 8u;
    constexpr float kBaseline = 0.2f;
    constexpr float kPoke = 0.8f;
    constexpr float kHeightScale = 100.0f;
    constexpr float kOriginY = 0.0f;
    constexpr float kSize = 256.0f;
    constexpr float kView = 1024.0f;
    const float kBaseY = kBaseline * kHeightScale + kOriginY; // 20
    const float kPokeY = kPoke * kHeightScale + kOriginY;     // 80

    TextureHandle height = MakeHeightTextureN(*m_Device, kN, kBaseline);
    ASSERT_TRUE(height.IsValid());
    for (uint32_t s = 0; s < kCBTFrameParamsRing; ++s)
        instance.SetHeightSource(s, height);

    const float a = 2.0f / kSize;
    float ortho[16];
    MakeOrthoTopDown(a, ortho);
    const auto p0 = ProjectPixels(a, kSize, 0.0f, kView, kView);
    const auto p2 = ProjectPixels(a, 0.0f, kSize, kView, kView);
    const float edgePx = std::hypot(p0[0] - p2[0], p0[1] - p2[1]);
    CBTFrameParams params = MakeParams(ortho, kSize, kSize, 0.0f, 0.0f, kHeightScale, kOriginY,
                                       kView, kView, edgePx * 0.2f, edgePx * 0.1f);

    // Converge with a FULL-pool eval (GateVertexEval=0 — the post-init force pass), so
    // every corner holds the baseline height before we switch to the gated steady state.
    CBTClassifyDesc converge{};
    converge.Mode = kClassifyScreenSpace;
    converge.TargetDepth = 12u;
    converge.GateVertexEval = 0u;
    uint32_t frame = 0;
    uint32_t prev = 0xFFFFFFFFu;
    uint32_t stable = 0;
    for (uint32_t i = 0; i < 24u && stable < 2u; ++i, ++frame)
    {
        const uint32_t c = RunClassify(*m_Device, instance, params, converge, frame);
        stable = (c == prev) ? (stable + 1u) : 0u;
        prev = c;
    }
    ASSERT_GE(stable, 2u) << "flat-height refinement never settled";

    // Steady state: GATED. One quiescent gated frame re-evaluates ZERO vertices (nothing
    // MODIFIED) — the idle-zero-work law under the production gate (the quiescence oracle).
    CBTClassifyDesc gated = converge;
    gated.GateVertexEval = 1u;
    RunClassify(*m_Device, instance, params, gated, frame++);
    EXPECT_EQ(instance.ReadVertexEvalCount(), 0)
        << "a quiescent gated frame re-evaluated vertices — the gate is not holding";

    // ---- CONTROL: poke the upper band (rows [4,8) -> uv.v in [0.5,1)) but publish NO
    // dirty rect. With the gate on nothing is flagged MODIFIED, so the poked band's corners
    // stay FROZEN at the baseline — the streamed-tile staleness the fix must cure.
    PokeHeightBand(*m_Device, height, kN, kN / 2u, kN, kPoke);
    RunClassify(*m_Device, instance, params, gated, frame++); // empty dirty rect + gate on
    {
        const uint32_t modified = instance.ReadDrawIndexCount(kDrawStreamModified);
        const uint32_t allCount = instance.ReadDrawIndexCount(kDrawStreamAll) / 3u;
        const std::vector<CornerYUv> corners = ReadLiveCorners(instance, allCount);
        const int32_t evalCount = instance.ReadVertexEvalCount();
        EXPECT_EQ(modified, 0u) << "no dirty rect must flag nothing MODIFIED";
        EXPECT_EQ(evalCount, 0) << "gated frame with no dirty rect re-evaluated vertices";
        uint32_t stalePoked = 0;
        for (const CornerYUv& c : corners)
            if (c.UvY > 0.7f)
            {
                ++stalePoked;
                EXPECT_NEAR(c.Y, kBaseY, 1e-2f) << "control: a poked corner healed without a dirty rect";
            }
        EXPECT_GT(stalePoked, 0u) << "no corners sampled inside the poked band";
    }

    // ---- FIX: publish the matching UV rect. The overlapping bisectors flag MODIFIED,
    // VertexEval re-samples EXACTLY them, and their corners heal to the poked height.
    CBTClassifyDesc dirty = gated;
    dirty.DirtyMinU = 0.0f;
    dirty.DirtyMaxU = 1.0f;
    dirty.DirtyMinV = 0.5f;
    dirty.DirtyMaxV = 1.0f;
    RunClassify(*m_Device, instance, params, dirty, frame++);
    {
        const uint32_t modifiedBisectors = instance.ReadDrawIndexCount(kDrawStreamModified) / 3u;
        const uint32_t allCount = instance.ReadDrawIndexCount(kDrawStreamAll) / 3u;
        const uint32_t validationErrors = instance.ReadValidationCounter(kValidationErrorCounter);
        const std::vector<CornerYUv> corners = ReadLiveCorners(instance, allCount);
        const int32_t evalCount = instance.ReadVertexEvalCount();
        EXPECT_GT(modifiedBisectors, 0u) << "the published rect flagged no bisector MODIFIED";
        EXPECT_EQ(static_cast<uint32_t>(evalCount), modifiedBisectors)
            << "VertexEval counter must equal the MODIFIED bisector count under the gate";
        EXPECT_EQ(validationErrors, 0u);
        uint32_t healed = 0, kept = 0;
        for (const CornerYUv& c : corners)
        {
            if (c.UvY > 0.7f)
            {
                ++healed;
                EXPECT_NEAR(c.Y, kPokeY, 1e-2f) << "poked corner not re-sampled under the gate";
            }
            else if (c.UvY < 0.3f)
            {
                ++kept;
                EXPECT_NEAR(c.Y, kBaseY, 1e-2f) << "an outside-rect corner changed height";
            }
        }
        EXPECT_GT(healed, 0u) << "no corners sampled inside the poked band";
        EXPECT_GT(kept, 0u) << "no corners sampled outside the poked band";
    }

    m_Device->DestroyTexture(height);
}

// DISCRIMINATOR (PR #506 R3, device-side): an EDIT into a NON-resident (fallback) tile changes the
// COARSE FIELD (binding 19) content — the modifier bake writes every loaded tile's heightfield and
// the coarse rebuild folds it in. Under the production GateVertexEval that change is INVISIBLE to
// already-cached fallback bisector corners until a dirty rect re-flags them — so if extraction
// rebuilds the coarse field but does not publish the edited region's rect, the fallback bisectors
// keep pre-edit heights while the resident side drops: the seam at the residency boundary inside the
// edit footprint. This locks the binding-19 variant of the gated re-eval the R3 publish drives
// (whole tile non-resident, NoSlot row held constant so the resolve stays on the coarse field):
//   * CONTROL: poke the coarse texture with NO dirty rect -> corners stay FROZEN at pre-edit, EVAL 0.
//   * FIX: poke + publish the matching UV rect -> the overlapping bisectors re-sample the edited
//     coarse EXACTLY, corners outside the rect keep the pre-edit height.
TEST_F(CBTScreenSpaceTest, AtlasCoarseFieldEditRefreshesOnlyWithDirtyRect)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    constexpr uint32_t kCoarseDim = 8u;
    constexpr uint32_t kTileRes = 9u;
    constexpr uint32_t kAtlasDim = kTileRes + 2u; // dummy resident geometry (NoSlot -> never sampled)
    constexpr float kBaseline = 0.3f;
    constexpr float kPoke = 0.8f;
    constexpr float kHeightScale = 100.0f;
    constexpr float kOriginY = 0.0f;
    constexpr float kSize = 256.0f;
    constexpr float kView = 1024.0f;
    const float kBaseY = kBaseline * kHeightScale + kOriginY; // 30
    const float kPokeY = kPoke * kHeightScale + kOriginY;     // 80

    TextureHandle coarseTex = MakeHeightTextureN(*m_Device, kCoarseDim, kBaseline);
    ASSERT_TRUE(coarseTex.IsValid());
    const std::vector<uint8_t> noSlotRow = MakeRow(/*slot*/ 0xFFFFFFFFu, /*gen*/ 0u); // whole tile fallback
    for (uint32_t s = 0; s < kCBTFrameParamsRing; ++s)
    {
        instance.SetCoarseSource(s, coarseTex);
        instance.UploadAtlasRows(s, noSlotRow.data(), 1u);
    }

    const float a = 2.0f / kSize;
    float ortho[16];
    MakeOrthoTopDown(a, ortho);
    const auto p0 = ProjectPixels(a, kSize, 0.0f, kView, kView);
    const auto p2 = ProjectPixels(a, 0.0f, kSize, kView, kView);
    const float edgePx = std::hypot(p0[0] - p2[0], p0[1] - p2[1]);
    CBTFrameParams params = MakeParams(ortho, kSize, kSize, 0.0f, 0.0f, kHeightScale, kOriginY,
                                       kView, kView, edgePx * 0.2f, edgePx * 0.1f);
    SetAtlasParams(params, kAtlasDim, kAtlasDim, /*slotsPerRow*/ 1u, kTileRes, 1u, 1u, kCoarseDim);

    // Converge (full eval) so every corner holds the coarse baseline.
    CBTClassifyDesc converge{};
    converge.Mode = kClassifyScreenSpace;
    converge.TargetDepth = 12u;
    converge.GateVertexEval = 0u;
    uint32_t frame = 0, prev = 0xFFFFFFFFu, stable = 0;
    for (uint32_t i = 0; i < 24u && stable < 2u; ++i, ++frame)
    {
        const uint32_t c = RunClassify(*m_Device, instance, params, converge, frame);
        stable = (c == prev) ? (stable + 1u) : 0u;
        prev = c;
    }
    ASSERT_GE(stable, 2u) << "coarse-fallback refinement never settled";

    CBTClassifyDesc gated = converge;
    gated.GateVertexEval = 1u;
    RunClassify(*m_Device, instance, params, gated, frame++);
    {
        const uint32_t allCount = instance.ReadDrawIndexCount(kDrawStreamAll) / 3u; // draw counts first
        const std::vector<CornerYUv> corners = ReadLiveCorners(instance, allCount);
        ASSERT_FALSE(corners.empty());
        ASSERT_NEAR(corners.front().Y, kBaseY, 1e-2f) << "converged fallback corners did not hold the coarse baseline";
        EXPECT_EQ(instance.ReadVertexEvalCount(), 0) << "gated quiescent frame re-evaluated vertices"; // WQ last
    }

    // EDIT: poke the coarse field's upper band (the modifier bake -> coarse rebuild lands here).
    PokeHeightBand(*m_Device, coarseTex, kCoarseDim, kCoarseDim / 2u, kCoarseDim, kPoke);

    // CONTROL: gate on + NO dirty rect -> the coarse change is invisible; the poked band's corners
    // stay FROZEN at the baseline (the seam the fix cures).
    RunClassify(*m_Device, instance, params, gated, frame++);
    {
        const uint32_t allCount = instance.ReadDrawIndexCount(kDrawStreamAll) / 3u;
        const std::vector<CornerYUv> corners = ReadLiveCorners(instance, allCount);
        const int32_t evalCount = instance.ReadVertexEvalCount();
        EXPECT_EQ(evalCount, 0) << "gated frame with no dirty rect re-evaluated vertices";
        uint32_t stale = 0;
        for (const CornerYUv& c : corners)
            if (c.UvY > 0.7f)
            {
                ++stale;
                EXPECT_NEAR(c.Y, kBaseY, 1e-2f) << "a coarse-edited corner healed without a dirty rect";
            }
        EXPECT_GT(stale, 0u) << "no corners sampled in the poked band";
    }

    // FIX: publish the edited region's UV rect (upper band). The overlapping bisectors re-sample the
    // edited coarse; corners outside the rect keep the baseline.
    CBTClassifyDesc dirty = gated;
    dirty.DirtyMinU = 0.0f;
    dirty.DirtyMaxU = 1.0f;
    dirty.DirtyMinV = 0.5f;
    dirty.DirtyMaxV = 1.0f;
    RunClassify(*m_Device, instance, params, dirty, frame++);
    {
        const uint32_t modifiedBisectors = instance.ReadDrawIndexCount(kDrawStreamModified) / 3u;
        const uint32_t allCount = instance.ReadDrawIndexCount(kDrawStreamAll) / 3u;
        const std::vector<CornerYUv> corners = ReadLiveCorners(instance, allCount);
        const int32_t evalCount = instance.ReadVertexEvalCount();
        EXPECT_GT(modifiedBisectors, 0u) << "the published rect flagged no bisector MODIFIED";
        EXPECT_EQ(static_cast<uint32_t>(evalCount), modifiedBisectors)
            << "VertexEval must re-sample exactly the MODIFIED bisectors under the gate";
        uint32_t healed = 0, kept = 0;
        for (const CornerYUv& c : corners)
        {
            if (c.UvY > 0.7f)
            {
                ++healed;
                EXPECT_NEAR(c.Y, kPokeY, 1e-2f) << "coarse-edited corner not re-sampled under the gate";
            }
            else if (c.UvY < 0.3f)
            {
                ++kept;
                EXPECT_NEAR(c.Y, kBaseY, 1e-2f) << "an outside-rect corner changed height";
            }
        }
        EXPECT_GT(healed, 0u) << "no corners sampled inside the published band";
        EXPECT_GT(kept, 0u) << "no corners sampled outside the published band";
    }

    m_Device->DestroyTexture(coarseTex);
}

// M2 (adversarial-review fix): a SUSTAINED edit must not oscillate. Marking a
// CONVERGED region dirty every frame flags it MODIFIED but must NOT churn its
// density — the merge threshold stays at the lower hysteresis bound (screen.w) for
// dirty bisectors, so converged bisectors in the dead-band [screen.w, screen.z]
// neither split nor merge. The pre-fix code raised the dirty merge threshold to
// screen.z, so every converged bisector merged, then re-split next frame: the draw
// count oscillated period-2 and the split/merge counters never reached zero. Hold the
// same full-terrain dirty rect for several frames and assert the density is frozen and
// per-frame work is zero (the MODIFIED stream stays populated — the rect is flagged).
TEST_F(CBTScreenSpaceTest, SustainedDirtyRegionDoesNotOscillate)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    constexpr float kSize = 256.0f;
    constexpr float kView = 1024.0f;
    const float a = 2.0f / kSize;
    float ortho[16];
    MakeOrthoTopDown(a, ortho);
    // Split at 40 px / merge at 20 px. The root split edge projects to ~1448 px and a
    // LEB bisection scales the longest edge by 1/sqrt(2) per level (1448 -> 45.3 ->
    // 32.0 -> 22.6 ...), so metric convergence lands the finest bisectors at ~32 px:
    // STRICTLY inside the dead-band (20 < 32 < 40). That is the population the pre-fix
    // dirty code (merge threshold raised to screen.z=40) collapses — 32 < 40 => merge
    // => coarser => 45.3 > 40 => split back => period-2 oscillation. The fix keeps the
    // merge threshold at screen.w=20 (32 > 20 => hold), so the density stays frozen.
    CBTFrameParams params = MakeParams(ortho, kSize, kSize, 0.0f, 0.0f, 0.0f, 0.0f, kView, kView,
                                       40.0f, 20.0f);

    // Converge under the plain screen-space metric (empty dirty rect). The depth cap
    // is deliberately generous (16) so convergence is METRIC-limited, not cap-pinned.
    CBTClassifyDesc converge{};
    converge.Mode = kClassifyScreenSpace;
    converge.TargetDepth = 16u;
    uint32_t frame = 0;
    uint32_t prev = 0xFFFFFFFFu;
    uint32_t stable = 0;
    uint32_t converged = 0;
    for (uint32_t i = 0; i < 32u && stable < 2u; ++i, ++frame)
    {
        converged = RunClassify(*m_Device, instance, params, converge, frame);
        stable = (converged == prev) ? (stable + 1u) : 0u;
        prev = converged;
    }
    ASSERT_GE(stable, 2u) << "screen-space refinement never settled";
    ASSERT_GT(converged, kRootIndexCount) << "refinement never grew past the roots";

    // Hold the WHOLE terrain dirty every frame. Density must stay frozen and per-frame
    // split/merge/allocate work must be zero (no period-2 churn); the region is still
    // binned MODIFIED. An oscillation shows up as a draw-count toggle + nonzero counters.
    CBTClassifyDesc dirty = converge;
    dirty.DirtyMinU = 0.0f;
    dirty.DirtyMaxU = 1.0f;
    dirty.DirtyMinV = 0.0f;
    dirty.DirtyMaxV = 1.0f;
    for (uint32_t i = 0; i < 6u; ++i, ++frame)
    {
        const uint32_t draw = RunClassify(*m_Device, instance, params, dirty, frame);
        EXPECT_EQ(draw, converged)
            << "sustained dirty region oscillated the density at frame " << frame;
        EXPECT_EQ(instance.ReadWorkQueueCounter(kWQSplitCounter), 0) << "frame " << frame;
        EXPECT_EQ(instance.ReadWorkQueueCounter(kWQSimplifyClassCounter), 0) << "frame " << frame;
        EXPECT_EQ(instance.ReadWorkQueueCounter(kWQAllocateCounter), 0) << "frame " << frame;
        EXPECT_GT(instance.ReadDrawIndexCount(kDrawStreamModified), 0u)
            << "the held dirty rect flagged no bisector MODIFIED at frame " << frame;
        EXPECT_EQ(instance.ReadValidationCounter(kValidationErrorCounter), 0u) << "frame " << frame;
    }
}

// C6 draw-input oracle (device-lost regression, coordinator's ORACLE-FIRST ask): the
// camera forward draw issues DrawIndexedIndirectCount over the VISIBLE record at byte
// offset 20, with the identity index buffer and a one-element count buffer. That is only
// safe if the VISIBLE record is a well-formed VkDrawIndexedIndirectCommand every frame —
// indexCount a multiple of 3 and <= 3*POOL (identity IB length), instanceCount == 1,
// firstIndex/vertexOffset/firstInstance == 0 — INCLUDING frames where the frustum binned
// zero (an uninitialized/garbage record at offset 20 would make the draw walk billions of
// vertices -> TDR). This proves the compute side of the draw contract holds across camera
// motion and a fully-off-screen (zero-visible) camera, and that every IndicesVisible entry
// the draw would index is a LIVE bisector slot (< POOL, HeapID != 0) so gVertex[slot] is
// always an in-bounds live read. It cannot reproduce a descriptor-buffer consumption fault
// (needs the full RenderServices draw path), but it isolates whether the fault is in the
// records the draw reads (this) or downstream in binding/consumption.
TEST_F(CBTScreenSpaceTest, VisibleRecordDrawInputWellFormed)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    constexpr float kSize = 256.0f;
    constexpr float kView = 1024.0f;
    const float a = 2.0f / kSize;
    float ortho[16];
    MakeOrthoTopDown(a, ortho);
    const auto p0 = ProjectPixels(a, kSize, 0.0f, kView, kView);
    const auto p2 = ProjectPixels(a, 0.0f, kSize, kView, kView);
    const float edgePx = std::hypot(p0[0] - p2[0], p0[1] - p2[1]);

    // Every VkDrawIndexedIndirectCommand field of a record must be within the draw
    // contract (the offset-20 draw + identity IB depend on exactly this).
    auto assertRecordWellFormed = [&](uint32_t stream, const char* name) -> uint32_t {
        const uint32_t indexCount = instance.ReadDrawIndexCount(stream);
        EXPECT_EQ(indexCount % 3u, 0u) << name << ": indexCount not a multiple of 3";
        EXPECT_LE(indexCount, 3u * kDefaultBisectorPoolSize) << name << ": indexCount > 3*POOL (identity IB overrun)";
        EXPECT_EQ(instance.ReadDrawRecordField(stream, kDrawInstanceCountField), 1u) << name << ": instanceCount != 1";
        EXPECT_EQ(instance.ReadDrawRecordField(stream, 2u), 0u) << name << ": firstIndex != 0";
        EXPECT_EQ(instance.ReadDrawRecordField(stream, 3u), 0u) << name << ": vertexOffset != 0";
        EXPECT_EQ(instance.ReadDrawRecordField(stream, 4u), 0u) << name << ": firstInstance != 0";
        return indexCount;
    };

    // Phase 1: terrain in view, refine to convergence.
    CBTFrameParams inView = MakeParams(ortho, kSize, kSize, 0.0f, 0.0f, 0.0f, 0.0f, kView, kView,
                                       edgePx * 0.15f, edgePx * 0.075f);
    CBTClassifyDesc converge{};
    converge.Mode = kClassifyScreenSpace;
    converge.TargetDepth = 12u;
    uint32_t frame = 0;
    uint32_t prev = 0xFFFFFFFFu;
    uint32_t stable = 0;
    for (uint32_t i = 0; i < 32u && stable < 2u; ++i, ++frame)
    {
        const uint32_t c = RunClassify(*m_Device, instance, inView, converge, frame);
        stable = (c == prev) ? (stable + 1u) : 0u;
        prev = c;
    }
    ASSERT_GE(stable, 2u) << "refinement never settled";

    const uint32_t allIndex = assertRecordWellFormed(kDrawStreamAll, "ALL");
    const uint32_t visIndex = assertRecordWellFormed(kDrawStreamVisible, "VISIBLE");
    EXPECT_GT(visIndex, 0u) << "in-view camera produced an empty visible stream";
    EXPECT_LE(visIndex, allIndex) << "visible triangles exceed the live tree";

    // Every visible index is a live slot (in-bounds, HeapID != 0) — so the draw's
    // gVertex[IndicesVisible[triangle]] fetch is always an in-bounds live read.
    const uint32_t visCount = visIndex / 3u;
    const std::vector<uint32_t> visIdx =
        instance.DebugReadWords(CBTBinding::IndicesVisible, visCount);
    uint32_t maxSlot = 0;
    for (uint32_t s : visIdx)
        maxSlot = std::max(maxSlot, s);
    const std::vector<uint32_t> heap =
        instance.DebugReadWords(CBTBinding::HeapID, (maxSlot + 1u) * 2u);
    for (uint32_t s : visIdx)
    {
        ASSERT_LT(s, kDefaultBisectorPoolSize) << "visible index out of pool bounds";
        uint64_t h = 0;
        std::memcpy(&h, &heap[s * 2u], sizeof(uint64_t));
        EXPECT_NE(h, 0ull) << "visible stream references a free slot " << s;
    }

    // Phase 2: shift the terrain fully out of the frustum (worldX in [10*size,11*size]
    // -> NDC x in [20,22], right of the frustum). Ortho projection is translation-
    // invariant in edge length, so the tree stays refined (ALL > 0) while every bisector
    // is culled -> the VISIBLE record must degrade to a safe no-op, not garbage.
    CBTFrameParams offScreen = MakeParams(ortho, kSize, kSize, 10.0f * kSize, 10.0f * kSize, 0.0f,
                                          0.0f, kView, kView, edgePx * 0.15f, edgePx * 0.075f);
    for (uint32_t i = 0; i < 3u; ++i, ++frame)
        RunClassify(*m_Device, instance, offScreen, converge, frame);

    EXPECT_EQ(instance.ReadDrawIndexCount(kDrawStreamVisible), 0u)
        << "fully off-screen terrain did not cull to an empty visible stream";
    assertRecordWellFormed(kDrawStreamVisible, "VISIBLE(zero)"); // instanceCount 1, offsets 0 at count 0
    EXPECT_GT(instance.ReadDrawIndexCount(kDrawStreamAll), 0u)
        << "ALL stream lost the live tree when the camera looked away (shadows would break)";
    EXPECT_EQ(instance.ReadValidationCounter(kValidationErrorCounter), 0u);
}

// C6 frustum-cull oracle (plan §8 C6): Classify bins the camera-visible bisectors into
// the VISIBLE draw stream (the forward camera draw rides it; shadows ride ALL). A camera
// whose frustum excludes half the terrain must produce visibleCount < allCount AND every
// triangle in the visible stream must intersect the frustum. The OLD behavior (flags =
// CBT_FLAG_VISIBLE unconditionally, visible == all) fails BOTH assertions — a signal the
// pre-C6 renderer could not produce.
//
// The ortho-top-down maps worldX in [0, size] to NDC x in [0, 2]; NDC's valid x is
// [-1, 1], so worldX in (size/2, size] is right-of-frustum. With the terrain at origin 0
// exactly half the domain is culled — deterministic, no perspective needed.
TEST_F(CBTScreenSpaceTest, FrustumCullingBinsVisibleSubset)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    constexpr float kSize = 256.0f;
    constexpr float kView = 1024.0f;
    const float a = 2.0f / kSize; // worldX in [0,size] -> NDC [0,2]; right half culled
    float ortho[16];
    MakeOrthoTopDown(a, ortho);

    const auto p0 = ProjectPixels(a, kSize, 0.0f, kView, kView);
    const auto p2 = ProjectPixels(a, 0.0f, kSize, kView, kView);
    const float edgePx = std::hypot(p0[0] - p2[0], p0[1] - p2[1]);
    // Refine several levels so the terrain spreads across the whole UV square and both
    // frustum halves carry many bisectors.
    CBTFrameParams params = MakeParams(ortho, kSize, kSize, 0.0f, 0.0f, 0.0f, 0.0f, kView, kView,
                                       edgePx * 0.15f, edgePx * 0.075f);

    CBTClassifyDesc converge{};
    converge.Mode = kClassifyScreenSpace;
    converge.TargetDepth = 12u;
    uint32_t frame = 0;
    uint32_t prev = 0xFFFFFFFFu;
    uint32_t stable = 0;
    uint32_t allIdx = 0;
    for (uint32_t i = 0; i < 32u && stable < 2u; ++i, ++frame)
    {
        allIdx = RunClassify(*m_Device, instance, params, converge, frame);
        stable = (allIdx == prev) ? (stable + 1u) : 0u;
        prev = allIdx;
    }
    ASSERT_GE(stable, 2u) << "screen-space refinement never settled";
    // Two extra quiescent frames so the final VISIBLE flags reflect the frustum test on
    // converged corners (no in-flight split/merge overwriting flags to VIS_MOD).
    for (uint32_t i = 0; i < 2u; ++i, ++frame)
        allIdx = RunClassify(*m_Device, instance, params, converge, frame);

    const uint32_t allCount = instance.ReadDrawIndexCount(kDrawStreamAll) / 3u;
    const uint32_t visCount = instance.ReadDrawIndexCount(kDrawStreamVisible) / 3u;
    const uint32_t validationErrors = instance.ReadValidationCounter(kValidationErrorCounter);

    // Quiescence guarantees the final VISIBLE flags are the frustum test on converged
    // corners (no split/merge force-flagged VIS_MOD this frame). Reuses the shared
    // readback, so it runs after the draw/validation reads above.
    ASSERT_EQ(instance.ReadWorkQueueCounter(kWQSplitCounter), 0) << "not quiescent — split churn";
    ASSERT_EQ(instance.ReadWorkQueueCounter(kWQSimplifyClassCounter), 0)
        << "not quiescent — merge churn";

    ASSERT_GT(allCount, kRootHalfedgeCount) << "refinement never grew past the roots";
    EXPECT_GT(visCount, 0u) << "the whole terrain was culled — visible stream is empty";
    EXPECT_LT(visCount, allCount)
        << "visibleCount == allCount — Classify did not frustum-cull (pre-C6 behavior)";

    // Pull the ALL + VISIBLE index streams and every live bisector's corners.
    const std::vector<uint32_t> allIndices =
        instance.DebugReadWords(CBTBinding::IndicesAll, allCount);
    const std::vector<uint32_t> visIndices =
        instance.DebugReadWords(CBTBinding::IndicesVisible, visCount);
    uint32_t maxSlot = 0;
    for (uint32_t s : allIndices)
        maxSlot = std::max(maxSlot, s);
    const std::vector<uint32_t> verts =
        instance.DebugReadWords(CBTBinding::CurrentVertex, (maxSlot + 1u) * kVertexWordsPerSlot);

    auto asFloat = [](uint32_t bits) { float f; std::memcpy(&f, &bits, sizeof(f)); return f; };
    auto corner = [&](uint32_t slot, uint32_t k) {
        const uint32_t base = slot * kVertexWordsPerSlot + k * 4u;
        return std::array<float, 3>{asFloat(verts[base + 0u]), asFloat(verts[base + 1u]),
                                    asFloat(verts[base + 2u])};
    };

    std::set<uint32_t> visibleSet(visIndices.begin(), visIndices.end());

    // Every VISIBLE triangle must intersect the frustum (the core oracle — the pre-C6
    // unconditional-VISIBLE binning would put culled triangles here and fail this).
    for (uint32_t slot : visIndices)
    {
        ASSERT_LE((slot + 1u) * kVertexWordsPerSlot, verts.size());
        EXPECT_TRUE(TriangleInFrustum(ortho, corner(slot, 0), corner(slot, 1), corner(slot, 2)))
            << "a fully-outside triangle (slot " << slot << ") leaked into the visible stream";
    }

    // The camera really excludes geometry: some ALL bisector is fully outside AND absent
    // from the visible stream.
    uint32_t culledSeen = 0;
    for (uint32_t slot : allIndices)
    {
        if ((slot + 1u) * kVertexWordsPerSlot > verts.size())
            continue;
        if (!TriangleInFrustum(ortho, corner(slot, 0), corner(slot, 1), corner(slot, 2)))
        {
            ++culledSeen;
            EXPECT_EQ(visibleSet.count(slot), 0u)
                << "a frustum-culled triangle (slot " << slot << ") was still marked visible";
        }
    }
    EXPECT_GT(culledSeen, 0u) << "no ALL triangle was outside the frustum — test exercised nothing";
    EXPECT_EQ(validationErrors, 0u);
}

// ===========================================================================
// Earth-scale slice 1b — render-origin-relative Classify oracles.
//
// gVertex stores WORLD-space corners (VertexEval is origin-INDEPENDENT and untouched
// by slice 1b). Classify's frustum + screen-space metric subtract the render origin
// (originWorld = renderOriginSector * CBT_SECTOR_SIZE) from each corner and project the
// small camera-relative result through the rebased viewProjRel — a precision
// reassociation of viewProjRel*(corner-origin) == worldVP*corner. These oracles pin:
//   (1) reconstruction exactness at Earth radius (CPU, the fp32-exact-bound claim);
//   (2) the active path == the world path where both are fp32-accurate (GPU equivalence);
//   (3) an origin sector STEP is transparent — no re-eval, no churn (GPU, the design's
//       open decision: neither full-pool re-seed nor incremental, because world corners
//       are camera-independent).
// The dark-ship (origin inactive) path is covered by every other test in this file (all
// run RenderOriginSector == 0, so viewProjRel == the world viewProj and every subtraction
// is an IEEE-exact no-op — byte-identical to the pre-slice build).
// ===========================================================================
namespace
{
constexpr float kEarthRadiusM = 6371000.0f;
constexpr float kCbtSectorSizeF = 1024.0f; // mirror kWorldSectorSize / CBT_SECTOR_SIZE

// Rebase a column-major viewProj so it consumes render-origin-relative positions:
// out = vp * translate(originWorld). Mirror of RenderOrigin.h::RebaseTranslationColumn —
// the runtime builds viewProjRel this way from the camera at the ResolveCameraData
// chokepoint. viewProjRel*(corner-originWorld) then equals vp*corner (a precision
// reassociation), so the metric is identical to the world path where fp32 is exact.
void RebaseViewProj(const float vp[16], float ox, float oy, float oz, float out[16])
{
    for (int i = 0; i < 16; ++i)
        out[i] = vp[i];
    out[12] = vp[0] * ox + vp[4] * oy + vp[8] * oz + vp[12];
    out[13] = vp[1] * ox + vp[5] * oy + vp[9] * oz + vp[13];
    out[14] = vp[2] * ox + vp[6] * oy + vp[10] * oz + vp[14];
    out[15] = vp[3] * ox + vp[7] * oy + vp[11] * oz + vp[15];
}

// Activate a render origin at sector (sx,sy,sz): rebase the params' viewProjRel (which
// still holds the world matrix MakeParams stored) by originWorld and record the sector —
// exactly what CBTRenderFeature::BuildFrameParams does from CameraData at runtime.
void SetRenderOrigin(CBTFrameParams& p, int32_t sx, int32_t sy, int32_t sz)
{
    float worldVP[16];
    std::memcpy(worldVP, p.ViewProjRel, sizeof(worldVP));
    RebaseViewProj(worldVP, static_cast<float>(sx) * kCbtSectorSizeF,
                   static_cast<float>(sy) * kCbtSectorSizeF,
                   static_cast<float>(sz) * kCbtSectorSizeF, p.ViewProjRel);
    p.RenderOriginSector[0] = static_cast<float>(sx);
    p.RenderOriginSector[1] = static_cast<float>(sy);
    p.RenderOriginSector[2] = static_cast<float>(sz);
    p.RenderOriginSector[3] = kCbtSectorSizeF;
}

// Converge a screen-space instance until the ALL-stream draw count settles (two identical
// frames) or the budget runs out; returns the settled count and advances `frame`.
uint32_t ConvergeAll(IDevice& device, CBTInstance& instance, const CBTFrameParams& params,
                     uint32_t maxDepth, uint32_t& frame)
{
    uint32_t prev = 0xFFFFFFFFu, stable = 0, count = 0;
    for (uint32_t i = 0; i < 32u && stable < 2u; ++i, ++frame)
    {
        count = RunScreenSpace(device, instance, params, maxDepth, frame);
        stable = (count == prev) ? (stable + 1u) : 0u;
        prev = count;
    }
    return count;
}
} // namespace

// (1) Reconstruction exactness at Earth radius (CPU). The shader computes
// relC = worldCorner - originWorld with originWorld = sector * CBT_SECTOR_SIZE (an exact
// integer < 2^24). At 6.4e6 both operands sit on the same fp32 grid (ULP ~0.5 m), so the
// subtraction is EXACT (its result is representable) and round-trips: relC + originWorld ==
// worldCorner bit-for-bit. The reconstruction therefore adds NO error beyond the corner's
// own fp32 granularity (the "fp32-exact bound where slice-1 math promises it") while
// collapsing the projected magnitude from ~6.4e6 to a small camera-relative coordinate.
TEST(CBTOriginReconstruction, RelativeCornerExactAtEarthRadius)
{
    // Camera on the +X shell; its nearest sector is the render origin.
    const float camWorld[3] = {kEarthRadiusM, 0.0f, 0.0f};
    const int32_t sector[3] = {
        static_cast<int32_t>(std::lround(camWorld[0] / kCbtSectorSizeF)),
        static_cast<int32_t>(std::lround(camWorld[1] / kCbtSectorSizeF)),
        static_cast<int32_t>(std::lround(camWorld[2] / kCbtSectorSizeF))};
    const float originWorld[3] = {static_cast<float>(sector[0]) * kCbtSectorSizeF,
                                  static_cast<float>(sector[1]) * kCbtSectorSizeF,
                                  static_cast<float>(sector[2]) * kCbtSectorSizeF};
    ASSERT_LT(std::fabs(originWorld[0]), 16777216.0f) << "originWorld must stay < 2^24 for exactness";

    // A spread of surface corners ~0.1..3 km from the camera on the shell (the near geometry
    // a surface camera renders), each displaced by a few km of relief like VertexEval does.
    const float dirs[5][3] = {{1.0f, 0.0f, 0.0f},        {1.0f, 0.0004f, 0.0f},
                              {1.0f, 0.0f, 0.00047f},    {1.0f, -0.0003f, 0.00025f},
                              {1.0f, 0.00018f, -0.0004f}};
    const float reliefs[5] = {0.0f, 1200.0f, -800.0f, 3000.0f, -2500.0f};
    for (int c = 0; c < 5; ++c)
    {
        const float len = std::sqrt(dirs[c][0] * dirs[c][0] + dirs[c][1] * dirs[c][1] +
                                    dirs[c][2] * dirs[c][2]);
        const float r = kEarthRadiusM + reliefs[c];
        const float corner[3] = {dirs[c][0] / len * r, dirs[c][1] / len * r, dirs[c][2] / len * r};
        for (int k = 0; k < 3; ++k)
        {
            const float relC = corner[k] - originWorld[k];         // the shader's subtraction (fp32)
            const float roundTrip = relC + originWorld[k];          // reconstruct the world coord
            EXPECT_EQ(std::memcmp(&roundTrip, &corner[k], sizeof(float)), 0)
                << "corner " << c << " axis " << k
                << ": relC + originWorld != worldCorner (subtraction was not exact)";
        }
        // The reconstructed corner is small (camera-relative), not ~6.4e6 — the whole point.
        const float relLen = std::sqrt((corner[0] - originWorld[0]) * (corner[0] - originWorld[0]) +
                                       (corner[1] - originWorld[1]) * (corner[1] - originWorld[1]) +
                                       (corner[2] - originWorld[2]) * (corner[2] - originWorld[2]));
        EXPECT_LT(relLen, 10000.0f) << "corner " << c << " relative magnitude not small — "
                                        "reconstruction did not cancel the planetary radius";
    }
}

// (2) Active-origin Classify == world-space Classify where both are fp32-accurate. Two runs
// of the SAME planar scene: sector 0 (world), and an ACTIVE origin (sector 48 ~ 49 km, past
// the 32,768 m activation radius yet fp32-crisp) whose viewProjRel is the same ortho rebased
// by that origin. viewProjRel*(corner-origin) == ortho*corner exactly at this scale, so the
// screen-space split metric and the ALL-stream tessellation must match — proving the rebased
// path reconstructs the world decision (and that the active branch does not perturb the decode).
TEST_F(CBTScreenSpaceTest, OriginRelativeClassifyMatchesWorldSpace)
{
    constexpr float kSize = 256.0f;
    constexpr float kView = 1024.0f;
    const float a = 2.0f / kSize;
    float ortho[16];
    MakeOrthoTopDown(a, ortho);
    const auto p0 = ProjectPixels(a, kSize, 0.0f, kView, kView);
    const auto p2 = ProjectPixels(a, 0.0f, kSize, kView, kView);
    const float edgePx = std::hypot(p0[0] - p2[0], p0[1] - p2[1]);

    CBTFrameParams worldParams = MakeParams(ortho, kSize, kSize, 0.0f, 0.0f, 0.0f, 0.0f, kView,
                                            kView, edgePx * 0.2f, edgePx * 0.1f);
    CBTFrameParams activeParams = worldParams; // same scene…
    SetRenderOrigin(activeParams, 48, 0, 48);  // …but at an active render origin (~49 km)
    ASSERT_NE(std::memcmp(worldParams.ViewProjRel, activeParams.ViewProjRel, sizeof(ortho)), 0)
        << "SetRenderOrigin did not rebase the matrix — the active path would be untested";

    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));

    ASSERT_TRUE(instance.InitializeRoots());
    uint32_t wf = 0;
    const uint32_t worldCount = ConvergeAll(*m_Device, instance, worldParams, 12u, wf);
    ASSERT_GT(worldCount, kRootIndexCount) << "world run never refined past the roots";
    EXPECT_EQ(instance.ReadValidationCounter(kValidationErrorCounter), 0u);

    ASSERT_TRUE(instance.InitializeRoots()); // re-seed to roots for an independent active run
    uint32_t af = 0;
    const uint32_t activeCount = ConvergeAll(*m_Device, instance, activeParams, 12u, af);
    EXPECT_EQ(instance.ReadValidationCounter(kValidationErrorCounter), 0u);

    EXPECT_EQ(activeCount, worldCount)
        << "active-origin tessellation diverged from the world path where fp32 is exact — "
           "the render-origin reconstruction is wrong";
}

// (3) An origin sector STEP is transparent — no re-eval, no churn. Converge with an ACTIVE
// origin (full eval), switch to the production GATED steady state and confirm a parked frame
// re-evaluates ZERO vertices, then STEP the render origin by one sector (rebasing viewProjRel
// to match — the camera representation changed, the geometry did not). Because gVertex stores
// camera-INDEPENDENT world corners, the step flags nothing MODIFIED: VertexEval stays 0, the
// ALL-stream draw count is unchanged, and validation stays clean. This is the design's open
// decision resolved — an origin step is NEITHER a full-pool re-seed NOR incremental re-eval;
// it is free, and quiescence (#490) is preserved across the boundary crossing.
TEST_F(CBTScreenSpaceTest, OriginSectorStepIsTransparentNoReseed)
{
    constexpr float kSize = 256.0f;
    constexpr float kView = 1024.0f;
    const float a = 2.0f / kSize;
    float ortho[16];
    MakeOrthoTopDown(a, ortho);
    const auto p0 = ProjectPixels(a, kSize, 0.0f, kView, kView);
    const auto p2 = ProjectPixels(a, 0.0f, kSize, kView, kView);
    const float edgePx = std::hypot(p0[0] - p2[0], p0[1] - p2[1]);

    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    CBTFrameParams params = MakeParams(ortho, kSize, kSize, 0.0f, 0.0f, 0.0f, 0.0f, kView, kView,
                                       edgePx * 0.2f, edgePx * 0.1f);
    SetRenderOrigin(params, 48, 0, 48); // active render origin (~49 km)

    // Converge with a full-pool eval (GateVertexEval = 0) so every corner is settled.
    CBTClassifyDesc converge{};
    converge.Mode = kClassifyScreenSpace;
    converge.TargetDepth = 12u;
    converge.GateVertexEval = 0u;
    uint32_t frame = 0, prev = 0xFFFFFFFFu, stable = 0, settledCount = 0;
    for (uint32_t i = 0; i < 32u && stable < 2u; ++i, ++frame)
    {
        settledCount = RunClassify(*m_Device, instance, params, converge, frame);
        stable = (settledCount == prev) ? (stable + 1u) : 0u;
        prev = settledCount;
    }
    ASSERT_GE(stable, 2u) << "active-origin refinement never settled";
    ASSERT_GT(settledCount, kRootIndexCount) << "active-origin refinement never grew past the roots";

    // Gated steady state: a parked frame at the active origin re-evaluates zero vertices.
    CBTClassifyDesc gated = converge;
    gated.GateVertexEval = 1u;
    RunClassify(*m_Device, instance, params, gated, frame++);
    EXPECT_EQ(instance.ReadDrawIndexCount(kDrawStreamAll), settledCount)
        << "a parked active-origin frame changed the tessellation";
    EXPECT_EQ(instance.ReadVertexEvalCount(), 0)
        << "a parked active-origin frame re-evaluated vertices — quiescence broke under the origin";

    // STEP the render origin by one sector (X and Z), rebasing viewProjRel so the camera-
    // relative projection is continuous across the crossing. World corners are unchanged.
    CBTFrameParams stepped = MakeParams(ortho, kSize, kSize, 0.0f, 0.0f, 0.0f, 0.0f, kView, kView,
                                        edgePx * 0.2f, edgePx * 0.1f);
    SetRenderOrigin(stepped, 49, 0, 49);
    RunClassify(*m_Device, instance, stepped, gated, frame++);
    const uint32_t afterStepDraw = instance.ReadDrawIndexCount(kDrawStreamAll);
    const uint32_t afterStepModified = instance.ReadDrawIndexCount(kDrawStreamModified);
    const uint32_t stepValidation = instance.ReadValidationCounter(kValidationErrorCounter);
    const int32_t stepEval = instance.ReadVertexEvalCount();
    EXPECT_EQ(stepEval, 0)
        << "the origin sector step re-evaluated vertices — a re-seed the world-corner design avoids";
    EXPECT_EQ(afterStepModified, 0u) << "the origin step flagged bisectors MODIFIED (spurious churn)";
    EXPECT_EQ(afterStepDraw, settledCount) << "the origin step changed the tessellation (not transparent)";
    EXPECT_EQ(stepValidation, 0u) << "the origin step corrupted neighbor links";
}

// Arc slice S2 — view-prioritised split metric on the PLANAR domain (design §6 gate 5: the pool is
// global, so the redistribution must be validated in both domains, not just the sphere). A large
// flat terrain viewed at a grazing angle saturates the shared 131072-slot pool; the near patch (a
// disc around the camera's look-at point) is frozen at the pool median with the bias OFF and gets
// markedly finer with it ON, while the pool stays saturated and validation stays green. Directional
// (planar detail is set by terrain size, not a fixed metre target, so the oracle is ON < OFF under
// saturation, not the sphere's absolute < 5 m). Perspective camera => a near/far gradient the bias
// can act on (a top-down ortho pose, as the other planar tests use, has no gradient by design).
TEST_F(CBTScreenSpaceTest, ViewPriorityNearPatchPlanar)
{
    using namespace GameEngine::Mathematics;
    // A BOUNDED plane (~1.5 km): an unbounded plane refines its whole recession to the far clip and
    // spreads the pool so thin the near patch never refines (whole median stuck ~330 m); bounding the
    // far extent lets the near/mid field freeze at a fine uniform size (like the sphere's horizon cap),
    // so the near disc holds enough facets to measure and the far band the bias coarsens is finite.
    constexpr float kTerrain = 1500.0f;
    constexpr float kOriginX = -kTerrain * 0.5f;
    constexpr float kOriginZ = 0.0f;          // near edge AT the eye's Z=0 — the whole plane is in FRONT
                                              // (a centered plane would put half the domain BEHIND the
                                              // +Z-looking camera, where the metric force-splits every
                                              // behind-eye bisector to the cap and exhausts the pool on
                                              // geometry that is never drawn — the sphere has no such
                                              // half because the whole globe sits in front of the eye).
    constexpr float kLookX = 0.0f, kLookZ = 214.0f; // near-patch centre on the plane (Y=0, ~261 m out)
    constexpr double kNearDiscM = 120.0;
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(1.0f, 16.0f / 9.0f, 0.1f, 15000.0f);
    const uint32_t cap = kDefaultBaseDepth + kMaxDecodeSubdiv;

    struct Result { PlanarScan Scan; int Overflow; uint32_t ValErr; uint32_t Zombie; };
    auto converge = [&](bool biasOn) -> Result {
        CBTInstance instance;
        EXPECT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
        EXPECT_TRUE(instance.InitializeRoots(kDomainPlanar));

        // Flat plane at Y=0: heightScale 0, originY 0. Aggressive TPE (3/1.5 px) to force saturation.
        float dummy[16];
        MakeOrthoTopDown(1.0f, dummy);
        CBTFrameParams p = MakeParams(dummy, kTerrain, kTerrain, kOriginX, kOriginZ, 0.0f, 0.0f, 1600.0f,
                                      900.0f, 3.0f, 1.5f);
        p.TerrainOrigin[2] = static_cast<float>(cap);
        if (biasOn)
        {
            p.NearBias[0] = 1.0f;    // enabled
            p.NearBias[1] = 450.0f;  // near radius (m) — contains the 120 m disc around the ~261 m look-at
            p.NearBias[2] = 800.0f;  // far radius (m) — coarsens the outer visible band (~800-1340 m)
            p.NearBias[3] = 8.0f;    // max coarsen factor
        }

        CBTClassifyDesc classify{};
        classify.Mode = kClassifyScreenSpace;
        classify.TargetDepth = cap;
        // Both arms hold the pool-pressure scale at 1x: the near-bias is characterised on a PINNED
        // pool (its redistribution is what this oracle measures), which the scale would un-pin.
        classify.PoolPressure = 0u;

        auto poseAt = [&](float alt) {
            // Look down the plane at ~35deg pitch (steeper than halfFOV, so the whole visible field is
            // BOUNDED — ground from ~0.5*alt to ~9*alt — giving a compact near patch AND a finite outer
            // band for the bias to coarsen; a grazing pose spreads the pool too thin to refine the near
            // patch at all). The eye stays at Z=0 (the near edge) so nothing is behind it; the look-at
            // scales with altitude to hold the pitch on descent.
            const Vector3 eye(0.0f, alt, 0.0f);
            const Matrix4x4 vp = proj * MakeLookAtLH(eye, Vector3(0.0f, 0.0f, alt * 1.43f),
                                                     Vector3(0.0f, 1.0f, 0.0f));
            std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
            p.CameraPos[0] = eye.x; p.CameraPos[1] = eye.y; p.CameraPos[2] = eye.z; p.CameraPos[3] = 1.0f;
        };

        // Graded descent 1200 -> 150 m (a cold grazing close-up mis-saturates), then settle at 150 m.
        constexpr uint32_t kDescend = 140u;
        for (uint32_t f = 0; f < kDescend; ++f)
        {
            poseAt(std::max(150.0f, 1200.0f * std::pow(0.96f, static_cast<float>(f))));
            RunClassify(*m_Device, instance, p, classify, f);
        }
        poseAt(150.0f);
        for (uint32_t f = 0; f < 64u; ++f)
            RunClassify(*m_Device, instance, p, classify, kDescend + f);

        Result r{};
        r.Scan = ScanPlanar(instance, kLookX, kLookZ, kNearDiscM);
        r.Overflow = instance.ReadWorkQueueCounter(kWQOverflowCounter);
        r.ValErr = instance.ReadValidationCounter(kValidationErrorCounter);
        r.Zombie = instance.ReadValidationCounter(kValidationZombieCounter);
        return r;
    };

    const Result off = converge(false);
    const Result on = converge(true);

    std::printf("[view-priority-planar] OFF near(<%.0fm) live=%u medEdge=%.2fm | whole live=%u (%.1f%%) "
                "medEdge=%.2fm overflow=%d\n",
                kNearDiscM, off.Scan.NearLive, off.Scan.NearMedEdge, off.Scan.Live,
                100.0 * off.Scan.Live / kDefaultBisectorPoolSize, off.Scan.MedEdge, off.Overflow);
    std::printf("[view-priority-planar] ON  near(<%.0fm) live=%u medEdge=%.2fm | whole live=%u (%.1f%%) "
                "medEdge=%.2fm overflow=%d\n",
                kNearDiscM, on.Scan.NearLive, on.Scan.NearMedEdge, on.Scan.Live,
                100.0 * on.Scan.Live / kDefaultBisectorPoolSize, on.Scan.MedEdge, on.Overflow);

    ASSERT_GT(off.Scan.NearLive, 16u) << "OFF planar near disc captured too few facets for a median";
    ASSERT_GT(on.Scan.NearLive, 16u) << "ON planar near disc captured too few facets for a median";
    // Saturation premise: the large plane fills the pool at the grazing pose (else there is nothing
    // to redistribute and the oracle is vacuous).
    EXPECT_GT(off.Scan.Live, kDefaultBisectorPoolSize * 85u / 100u)
        << "the planar pose did not saturate the pool — enlarge the terrain / lower the TPE";
    // Redistribution: the near patch gets markedly finer with the bias on (planar target is
    // directional, not the sphere's absolute < 5 m).
    EXPECT_LT(on.Scan.NearMedEdge, off.Scan.NearMedEdge * 0.7)
        << "view-priority did not refine the planar near patch under saturation (gate 5, planar)";
    // Still saturated (redistributed, not un-saturated) and conformity/validation green both ways.
    EXPECT_GT(on.Scan.Live, kDefaultBisectorPoolSize / 2u)
        << "the planar near-bias un-saturated the pool — re-tune the contention ramp";
    EXPECT_EQ(off.ValErr, 0u);
    EXPECT_EQ(on.ValErr, 0u) << "planar near-bias corrupted the neighbor links";
    EXPECT_EQ(off.Zombie, 0u);
    EXPECT_EQ(on.Zombie, 0u) << "planar near-bias produced zombie bisectors";
    // Bound calibration (2026-07-19 deflake): the healthy converged ON/OFF overflow ratio at this
    // pose is 1.18-1.20 (10x isolation runs: 1.1824..1.2006) — the redistribution BY DESIGN re-queues
    // the freed far-band budget as near-patch split demand, ~20% of which overflows the work queue at
    // 100% occupancy. The former 1.2x bound sat ON that healthy band (one run passed by 0.16%), and
    // atomic slot-competition order under shared-GPU load pushes the ratio across it — the ledgered
    // flake. The regression this gate exists for is a runaway feedback (a bias that destabilizes the
    // split/merge queue equilibrium), which is a >= 2x multiplier, not a +2% shift; 1.5x sits ~25%
    // above the healthy band and ~25% below the runaway class. A bias mis-tune in the OTHER direction
    // (over-coarsening) is caught by the near-patch oracle above (measured: medEdge 2.93 m vs the
    // required < 1.45 m), not by this gate.
    EXPECT_LE(on.Overflow, static_cast<int>(off.Overflow * 1.5 + 200000))
        << "planar near-bias raised dropped-split overflow meaningfully";
}

// The water plane (issue #1413): a planar bisector whose three corners all lie more than the
// margin below the water surface is hidden from a camera above it, so Classify scales its split
// and merge thresholds by the coarsen factor instead of refining the seabed to the pixel target.
// A half-deep / half-land plane under the top-down ortho camera (1 px == 1 m, so a bisector's
// projected split edge is its world edge): the land half and the bisectors straddling the shore
// converge on the base target while the deep half converges on the coarse one. The same plane
// with the camera under the surface, and with the term disabled, refines uniformly — the term is
// a strict superset of the metric without it.
TEST_F(CBTScreenSpaceTest, SubmergedSeabedStaysCoarseWhileShorelineRefines)
{
    constexpr uint32_t kTexels = 64u;
    constexpr float kTerrain = 1024.0f;
    constexpr float kOrigin = -kTerrain * 0.5f;
    constexpr float kHeightScale = 40.0f;
    constexpr float kOriginY = -20.0f; // texel 0 -> -20 m (seabed), texel 1 -> +20 m (land)
    constexpr float kWaterY = 0.0f;
    constexpr float kMarginM = 2.0f;
    constexpr float kCoarsen = 8.0f;
    // With the plane filling a 1024 px viewport a root hypotenuse is 1024*sqrt(2) px and each LEB
    // level divides the edge by sqrt(2), so the ladder of edge lengths is 1448, 1024, 724, ...:
    // 14 px lands the land half on 11.3 px (level 14) and the coarse half on 90.5 px (level 8),
    // neither within float noise of a threshold (16.0 and 128 sit on the ladder — avoided).
    constexpr float kSplitPx = 14.0f;
    constexpr float kMergePx = 7.0f;
    constexpr uint32_t kFrames = 48u; // 14 split levels + settle
    // LEB conformity grades the deep half from the shoreline's 8 m legs up to the coarse target's
    // 64 m legs one sqrt(2) step per ring (~135 m of rings), so the seabed the water is supposed
    // to keep coarse is judged past that: every corner farther than this from the shore (Z = 0).
    constexpr float kInteriorZ = -160.0f;
    const uint32_t cap = kDefaultBaseDepth + kMaxDecodeSubdiv;

    struct EdgeScan
    {
        uint32_t Submerged = 0, Interior = 0, Straddling = 0, Land = 0;
        float SubmergedMax = 0.0f, InteriorMin = 1e9f, InteriorMax = 0.0f;
        float StraddlingMax = 0.0f, LandMax = 0.0f;
    };
    auto scan = [&](CBTInstance& instance) {
        const uint32_t allCount = instance.ReadDrawIndexCount(kDrawStreamAll) / 3u;
        const std::vector<uint32_t> indices = instance.DebugReadWords(CBTBinding::IndicesAll, allCount);
        uint32_t maxSlot = 0;
        for (uint32_t s : indices)
            maxSlot = std::max(maxSlot, s);
        const std::vector<uint32_t> verts =
            instance.DebugReadWords(CBTBinding::CurrentVertex, (maxSlot + 1u) * kVertexWordsPerSlot);
        auto asFloat = [](uint32_t bits) { float f; std::memcpy(&f, &bits, sizeof(f)); return f; };
        EdgeScan es{};
        for (uint32_t slot : indices)
        {
            const uint32_t base = slot * kVertexWordsPerSlot;
            float y[3], z[3];
            for (uint32_t k = 0; k < 3u; ++k)
            {
                y[k] = asFloat(verts[base + k * 4u + 1u]);
                z[k] = asFloat(verts[base + k * 4u + 2u]);
            }
            const float dx = asFloat(verts[base + 0u]) - asFloat(verts[base + 8u]);
            const float dz = asFloat(verts[base + 2u]) - asFloat(verts[base + 10u]);
            const float edgePx = std::sqrt(dx * dx + dz * dz); // 1 px == 1 m under this camera
            const float hiddenBelow = kWaterY - kMarginM;
            const bool allBelow = y[0] < hiddenBelow && y[1] < hiddenBelow && y[2] < hiddenBelow;
            const bool anyBelow = y[0] < hiddenBelow || y[1] < hiddenBelow || y[2] < hiddenBelow;
            if (allBelow)
            {
                ++es.Submerged;
                es.SubmergedMax = std::max(es.SubmergedMax, edgePx);
                if (std::max(z[0], std::max(z[1], z[2])) < kInteriorZ)
                {
                    ++es.Interior;
                    es.InteriorMin = std::min(es.InteriorMin, edgePx);
                    es.InteriorMax = std::max(es.InteriorMax, edgePx);
                }
            }
            else if (anyBelow)
            {
                ++es.Straddling;
                es.StraddlingMax = std::max(es.StraddlingMax, edgePx);
            }
            else
            {
                ++es.Land;
                es.LandMax = std::max(es.LandMax, edgePx);
            }
        }
        return es;
    };

    // One instance converged through one camera height per phase, kFrames each, scanned at the end:
    // a second phase exercises the merge direction from a tree the first phase refined.
    auto converge = [&](float coarsen, std::initializer_list<float> cameraYs) {
        CBTInstance instance;
        EXPECT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
        EXPECT_TRUE(instance.InitializeRoots(kDomainPlanar));
        // Rows [0, 32) stay at texel 0 (seabed), rows [32, 64) become texel 1 (land): the shore
        // runs along the plane's Z midline, one texel (16 m) of linear ramp wide.
        TextureHandle height = MakeHeightTextureN(*m_Device, kTexels, 0.0f);
        PokeHeightBand(*m_Device, height, kTexels, kTexels / 2u, kTexels, 1.0f);

        float vp[16];
        MakeOrthoTopDown(2.0f / kTerrain, vp); // the whole plane fills the viewport
        CBTFrameParams p = MakeParams(vp, kTerrain, kTerrain, kOrigin, kOrigin, kHeightScale, kOriginY,
                                      kTerrain, kTerrain, kSplitPx, kMergePx);
        p.CameraPos[3] = 1.0f;
        p.WaterPlane[0] = kWaterY;
        p.WaterPlane[1] = kMarginM;
        p.WaterPlane[2] = coarsen;

        CBTClassifyDesc classify{};
        classify.Mode = kClassifyScreenSpace;
        classify.TargetDepth = cap;
        uint32_t frame = 0;
        for (float cameraY : cameraYs)
        {
            p.CameraPos[1] = cameraY;
            for (uint32_t f = 0; f < kFrames; ++f, ++frame)
            {
                instance.SetHeightSource(frame, height);
                RunClassify(*m_Device, instance, p, classify, frame);
            }
        }
        EdgeScan es = scan(instance);
        EXPECT_EQ(instance.ReadValidationErrorCount(), 0u);
        m_Device->DestroyTexture(height);
        return es;
    };

    const EdgeScan on = converge(kCoarsen, {50.0f});
    const EdgeScan under = converge(kCoarsen, {kWaterY - 10.0f});
    const EdgeScan off = converge(0.0f, {50.0f});
    // Refined uniformly from under the surface, then the camera climbs out: the merge side has to
    // coarsen the hidden seabed back, which is what the coarsened MERGE threshold is for.
    const EdgeScan resurfaced = converge(kCoarsen, {kWaterY - 10.0f, 50.0f});
    std::printf("[water-plane] on: submerged=%u (max %.1f px) interior=%u [%.1f..%.1f px] "
                "straddling=%u (max %.1f px) land=%u (max %.1f px) | under: submerged max %.1f px | "
                "off: submerged max %.1f px | resurfaced: interior=%u [%.1f..%.1f px] land max %.1f px\n",
                on.Submerged, on.SubmergedMax, on.Interior, on.InteriorMin, on.InteriorMax,
                on.Straddling, on.StraddlingMax, on.Land, on.LandMax, under.SubmergedMax,
                off.SubmergedMax, resurfaced.Interior, resurfaced.InteriorMin, resurfaced.InteriorMax,
                resurfaced.LandMax);

    ASSERT_GT(on.Interior, 16u) << "the deep interior captured too few bisectors to judge";
    ASSERT_GT(on.Straddling, 4u) << "no bisector straddles the shore — the ramp is not where expected";
    ASSERT_GT(on.Land, 16u);
    // Land and the shoreline converge on the pixel target exactly as without the term.
    EXPECT_LE(on.LandMax, kSplitPx);
    EXPECT_LE(on.StraddlingMax, kSplitPx);
    // The hidden seabed past the conformity rings stops above the target — the mutation that drops
    // the water term refines it to <= kSplitPx like the land — and never past the coarse target.
    EXPECT_GT(on.InteriorMin, kSplitPx);
    EXPECT_LE(on.SubmergedMax, kCoarsen * kSplitPx);
    // The deep half as a whole is a small fraction of the land half's population: 1/64 of it at
    // the coarse target plus the shoreline's grading rings (the mutation makes the halves equal).
    EXPECT_LE(on.Submerged, on.Land / 4u);
    // A camera under the surface sees the seabed: everything refines to the base target.
    EXPECT_LE(under.SubmergedMax, kSplitPx);
    // Term disabled (zero coarsen, the default every other test runs with): uniform refinement.
    EXPECT_LE(off.SubmergedMax, kSplitPx);
    // Resurfacing coarsens the hidden interior back above the target — the mutation that leaves the
    // merge threshold uncoarsened keeps it at the target — without overshooting the coarse one, and
    // leaves the land alone.
    ASSERT_GT(resurfaced.Interior, 16u);
    EXPECT_GT(resurfaced.InteriorMin, kSplitPx);
    EXPECT_LE(resurfaced.SubmergedMax, kCoarsen * kSplitPx);
    EXPECT_LE(resurfaced.LandMax, kSplitPx);
}
