// Content-aware split (#2659): a planar facet splits only while its height range projects above
// the threshold (CBTFrameParams::WaterPlane[3]) at the nearest point of its 3D box, and a pair
// merges once its parent's range projects below half of it. Real-device arms over a 576 m terrain
// on a 1025^2 lattice (cap 21, the cap-from-lattice rule), the camera at eye height above the
// ground it stands on:
//
//   FlatTerrainHoldsNoMoreThanTheCap19Tree — flat ground with the split on holds no more live
//     facets than the same ground at cap 19 without it, while relief still refines to the cap.
//   ThePagedResolveSplitsFlatGroundLikeTheUnifiedTexture — the paged height arm (AtlasParams1.z 2)
//     holds the same flat-ground tree as the unified arm: the split reads the unified texture it keeps.
//   FlatteningTheHeightsCoarsensARefinedTree — the merge half and the pyramid rebuild on a newly
//     bound texture: a tree converged on relief coarsens once the heights go flat, and settles.
//   SmallBumpInFrontOfTheCameraRefines — a 0.3 m bump 12 m ahead at a terrain corner is drawn: a
//     large facet whose split-edge midpoint lies far away must not veto the split of relief under
//     the camera.
//   HeightsOutsideZeroToOneRefine — a 3 m hill on ground above normalized 1 and a 3 m ditch below
//     normalized 0 are drawn: the pyramid holds the heights the terrain holds, unclamped.
//   InPlaceEditAnnouncedByADirtyRectCoarsens — relief flattened in the SAME texture, announced only
//     by an edit rect (the brush and modifier path), coarsens: the edit rect rebuilds the pyramid.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "CBTTerrain/CBTDemandTuning.h"
#include "CBTTerrain/CBTInstance.h"
#include "CBTTerrain/CBTKernelSet.h"
#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrain/CBTNearBias.h"
#include "CBTTerrain/CBTTreeAudit.h"
#include "CBTReliefHeightSource.h"
#include "CBTTestHarness.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Core/CommandList.h"

using namespace GameEngine::CBTTerrain;
using namespace GameEngine::CBTTerrain::Test;
using namespace GameEngine::Rendering;

namespace
{
constexpr float kScreenW = 1600.0f;
constexpr float kScreenH = 900.0f;
constexpr float kFovY = 1.05f;
constexpr float kTerrainM = 576.0f;
constexpr uint32_t kLatticeDim = 1025u; // 576 m at 1 sample per metre rounds up to 1024 intervals
constexpr uint32_t kLatticeCap = 21u;   // cap from the lattice: 2 x log2(1024) + 1
constexpr uint32_t kAuthoredCap = 19u;  // the authored-density cap before cap-from-lattice
constexpr float kHeightScaleM = 60.0f;
constexpr float kEyeM = 1.7f;
constexpr float kTargetPx = 8.0f;
constexpr float kFlatPx = 1.0f;
constexpr uint32_t kConvergeFrames = 120u;
// A feature counts as drawn when the rendered surface at its peak is within this many pixels of the
// data, seen from the test's 12 m: a few times the 1 px threshold, far below the 19 to 194 px a
// dropped feature reads.
constexpr float kDrawnPx = 4.0f;

// The relief texture's normalized height at uv (MakeReliefHeightTexture's own octave sum), so a
// camera over relief can stand at eye height above the ground under it.
float ReliefNormalizedAt(float u, float v)
{
    float amp = 0.5f, sum = 0.0f, norm = 0.0f;
    for (uint32_t octave = 0; octave < 6u; ++octave)
    {
        sum += amp * ReliefSmoothNoiseAt(u, v, 4u << octave);
        norm += amp;
        amp *= 0.5f;
    }
    return sum / norm;
}

// Flat `base` (normalized) plus a Gaussian feature of `featureM` metres (negative digs a ditch),
// sigma `sigmaM`, centred at terrain-local (cx, cz).
std::vector<float> FeatureField(float base, float featureM, float cx, float cz, float sigmaM)
{
    std::vector<float> d(static_cast<size_t>(kLatticeDim) * kLatticeDim, base);
    const float spacing = kTerrainM / static_cast<float>(kLatticeDim - 1u);
    for (uint32_t y = 0; y < kLatticeDim; ++y)
        for (uint32_t x = 0; x < kLatticeDim; ++x)
        {
            const float wx = static_cast<float>(x) * spacing - cx;
            const float wz = static_cast<float>(y) * spacing - cz;
            d[static_cast<size_t>(y) * kLatticeDim + x] +=
                (featureM / kHeightScaleM) * std::exp(-(wx * wx + wz * wz) / (2.0f * sigmaM * sigmaM));
        }
    return d;
}

uint32_t HeapDepth(uint64_t h)
{
    uint32_t d = 0;
    while (h > 1u)
    {
        h >>= 1u;
        ++d;
    }
    return d;
}

// The rendered surface height at terrain uv (u, v): the live leaf containing it, interpolated.
bool RenderedHeightAt(const CBTTreeSnapshot& s, float u, float v, float& outY)
{
    for (uint32_t i = 0; i < s.PoolSize; ++i)
    {
        if (s.HeapIds[i] == 0u)
            continue;
        const CBTVertexData& vd = s.Vertices[i];
        const float ax = vd.Corner0[3], ay = vd.Meta[0];
        const float bx = vd.Corner1[3], by = vd.Meta[1];
        const float cx = vd.Corner2[3], cy = vd.Meta[2];
        const float den = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
        if (std::fabs(den) < 1e-20f)
            continue;
        const float w0 = ((by - cy) * (u - cx) + (cx - bx) * (v - cy)) / den;
        const float w1 = ((cy - ay) * (u - cx) + (ax - cx) * (v - cy)) / den;
        const float w2 = 1.0f - w0 - w1;
        if (w0 < -1e-6f || w1 < -1e-6f || w2 < -1e-6f)
            continue;
        outY = w0 * vd.Corner0[1] + w1 * vd.Corner1[1] + w2 * vd.Corner2[1];
        return true;
    }
    return false;
}

class CBTContentAwareSplit : public ::testing::Test
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

    // Uploads `data` (kLatticeDim^2 normalized heights) into `tex`, from `prior`.
    void Upload(TextureHandle tex, const std::vector<float>& data, ResourceState prior)
    {
        const size_t bytes = data.size() * sizeof(float);
        BufferHandle staging = m_Device->CreateUploadBuffer(bytes, "CBT.Test.HeightStaging");
        m_Device->UpdateBuffer(staging, 0, bytes, data.data());
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->CopyBufferToTextureSubresource(staging, tex, 0, 0, kLatticeDim, kLatticeDim, 0,
                                           static_cast<size_t>(kLatticeDim) * sizeof(float), 1, 0,
                                           0, 0, prior);
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(tex, ResourceState::CopyDest,
                                                          ResourceState::ShaderResource));
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();
        m_Device->DestroyBuffer(staging);
    }

    TextureHandle MakeTexture(const std::vector<float>& data)
    {
        TextureDesc td{};
        td.width = kLatticeDim;
        td.height = kLatticeDim;
        td.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
        td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) |
                   static_cast<uint32_t>(TextureUsage::TransferDst);
        td.persistent = true;
        td.debugName = "CBT.Test.Heights";
        TextureHandle tex = m_Device->CreateTexture(td);
        if (tex.IsValid())
            Upload(tex, data, ResourceState::Undefined);
        return tex;
    }

    TextureHandle MakeFlatTexture(float value)
    {
        return MakeTexture(std::vector<float>(static_cast<size_t>(kLatticeDim) * kLatticeDim, value));
    }

    void BindEverySlot(CBTInstance& inst, TextureHandle tex)
    {
        for (uint32_t slot = 0; slot < kCBTFrameParamsRing; ++slot)
            inst.SetHeightSource(slot, tex);
    }

    // The camera at terrain-local (x, eyeY, z), looking 10 degrees down along +Z.
    CBTFrameParams CameraParams(uint32_t maxDepth, float flatPx, float x, float eyeY, float z)
    {
        using namespace GameEngine::Mathematics;
        CBTFrameParams p{};
        p.Screen[0] = kScreenW;
        p.Screen[1] = kScreenH;
        p.Screen[2] = kTargetPx;
        p.Screen[3] = kTargetPx * 0.5f;
        p.TerrainSize[0] = kTerrainM;
        p.TerrainSize[1] = kTerrainM;
        p.TerrainSize[2] = kHeightScaleM;
        p.TerrainOrigin[2] = static_cast<float>(maxDepth);
        const Vector3 eye(x, eyeY, z);
        const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(kFovY, kScreenW / kScreenH, 0.5f, 20000.0f);
        const Matrix4x4 vp = proj * MakeLookAtLH(eye, eye + Vector3(0.0f, -0.176f, 1.0f), Vector3(0, 1, 0));
        std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
        p.CameraPos[0] = eye.x;
        p.CameraPos[1] = eye.y;
        p.CameraPos[2] = eye.z;
        p.CameraPos[3] = 1.0f;
        const CBTNearBiasRadii nb = ComputeNearBiasRadii(kEyeM);
        p.NearBias[0] = 1.0f;
        p.NearBias[1] = nb.NearRadius;
        p.NearBias[2] = nb.FarRadius;
        p.NearBias[3] = kNearBiasMaxCoarsen;
        p.DemandTuning[0] = kNearFieldFacetTargetM;
        p.DemandTuning[1] = kOffFrustumKeepOcc;
        p.WaterPlane[3] = flatPx;
        return p;
    }

    // Eye height above the centre of a terrain whose ground there is `groundNormalized`.
    CBTFrameParams WalkParams(uint32_t maxDepth, float flatPx, float groundNormalized)
    {
        return CameraParams(maxDepth, flatPx, kTerrainM * 0.5f, groundNormalized * kHeightScaleM + kEyeM,
                            kTerrainM * 0.5f);
    }

    struct Result
    {
        uint32_t Live = 0;
        uint32_t DeepestLeaf = 0;
        uint32_t TailLiveMin = UINT32_MAX;
        uint32_t TailLiveMax = 0;
        bool Watertight = false;
        CBTTreeSnapshot Snapshot;
    };

    // `frames` updates at `p` from the instance's current tree. `forceFirst` forces a full corner
    // refresh on the first (as a re-seed or a newly bound texture does); `editFirst` announces the
    // whole terrain as edited on the first instead (the brush and modifier path: same texture, no
    // forced refresh). The last ten form the settle window.
    Result Converge(CBTInstance& inst, const CBTFrameParams& p, uint32_t maxDepth, uint32_t frames,
                    uint32_t firstFrame, bool forceFirst = true, bool editFirst = false)
    {
        CBTClassifyDesc c{};
        c.Mode = kClassifyScreenSpace;
        c.TargetDepth = maxDepth;
        c.NearFieldGate = 1u;
        Result r;
        for (uint32_t f = 0; f < frames; ++f)
        {
            c.GateVertexEval = (f == 0u && forceFirst) ? 0u : 1u;
            const bool edit = editFirst && f == 0u;
            c.DirtyMinU = 0.0f;
            c.DirtyMinV = 0.0f;
            c.DirtyMaxU = edit ? 1.0f : 0.0f;
            c.DirtyMaxV = edit ? 1.0f : 0.0f;
            auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
            cl->Begin();
            inst.RecordUpdate(*cl, c, p, firstFrame + f);
            cl->End();
            std::vector<CommandList*> lists{cl.get()};
            m_Device->ExecuteCommandLists(lists);
            m_Device->WaitForIdle();
            if (f + 10u >= frames)
            {
                const uint32_t live = inst.ReadTessellationStats().LiveCount;
                r.TailLiveMin = std::min(r.TailLiveMin, live);
                r.TailLiveMax = std::max(r.TailLiveMax, live);
            }
        }
        r.Snapshot = ReadTreeSnapshot(inst, true);
        const CBTTreeAuditResult audit = AuditTree(r.Snapshot);
        r.Live = audit.LiveCount;
        r.DeepestLeaf = audit.MaxDepth;
        r.Watertight = audit.Watertight();
        return r;
    }

    // Converges a fresh instance on `data` with the camera `distM` before the feature at (fx, fz),
    // at eye height above `groundNormalized`, and returns the rendered error at the feature's peak in
    // pixels at that distance.
    float FeatureErrorPx(const std::vector<float>& data, float groundNormalized, float featureM, float fx,
                         float fz, float distM, const char* label)
    {
        const TextureHandle tex = MakeTexture(data);
        EXPECT_TRUE(tex.IsValid());
        CBTInstance inst;
        EXPECT_TRUE(inst.Initialize(*m_Device, m_KernelSet) && inst.InitializeRoots(kDomainPlanar));
        BindEverySlot(inst, tex);
        const float groundY = groundNormalized * kHeightScaleM;
        const Result r = Converge(inst, CameraParams(kLatticeCap, kFlatPx, fx, groundY + kEyeM, fz - distM),
                                  kLatticeCap, kConvergeFrames, 0u);
        float renderedY = 0.0f;
        const bool found = RenderedHeightAt(r.Snapshot, fx / kTerrainM, fz / kTerrainM, renderedY);
        const float focalPx = 0.5f * kScreenH / std::tan(0.5f * kFovY);
        const float errPx = std::fabs(groundY + featureM - renderedY) * focalPx / distM;
        std::printf("%s: live %u deepest %u rendered %.3f true %.3f -> %.1f px\n", label, r.Live, r.DeepestLeaf,
                    renderedY, groundY + featureM, errPx);
        EXPECT_TRUE(found) << label;
        EXPECT_TRUE(r.Watertight) << label;
        inst.Shutdown();
        m_Device->DestroyTexture(tex);
        return found ? errPx : 1e9f;
    }

    std::unique_ptr<IDevice> m_Device;
    CBTKernelSet m_KernelSet;
};
} // namespace

TEST_F(CBTContentAwareSplit, FlatTerrainHoldsNoMoreThanTheCap19Tree)
{
    const TextureHandle flat = MakeFlatTexture(0.5f);
    ASSERT_TRUE(flat.IsValid());

    auto runFlat = [&](uint32_t maxDepth, float flatPx) {
        CBTInstance inst;
        EXPECT_TRUE(inst.Initialize(*m_Device, m_KernelSet) && inst.InitializeRoots(kDomainPlanar));
        BindEverySlot(inst, flat);
        const Result r = Converge(inst, WalkParams(maxDepth, flatPx, 0.5f), maxDepth, kConvergeFrames, 0u);
        inst.Shutdown();
        return r;
    };
    const Result authored = runFlat(kAuthoredCap, 0.0f);
    const Result split = runFlat(kLatticeCap, kFlatPx);
    std::printf("flat ground: cap %u without the split %u live; cap %u with it %u live (deepest %u)\n",
                kAuthoredCap, authored.Live, kLatticeCap, split.Live, split.DeepestLeaf);
    ASSERT_GT(authored.Live, 10000u) << "the cap-19 reference tree is trivial; the comparison is void";
    EXPECT_LE(split.Live, authored.Live)
        << "flat ground with the content-aware split refined past the cap-19 tree";
    EXPECT_TRUE(split.Watertight);
    EXPECT_EQ(split.TailLiveMin, split.TailLiveMax) << "flat ground did not settle";

    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet) && inst.InitializeRoots(kDomainPlanar));
    ReliefHeightSource relief(*m_Device, inst, kHeightScaleM, kLatticeDim);
    ASSERT_TRUE(relief.IsArmed());
    const Result ridges = Converge(inst, WalkParams(kLatticeCap, kFlatPx, ReliefNormalizedAt(0.5f, 0.5f)),
                                   kLatticeCap, kConvergeFrames, 0u);
    std::printf("relief: cap %u with the split %u live (deepest %u)\n", kLatticeCap, ridges.Live,
                ridges.DeepestLeaf);
    EXPECT_EQ(ridges.DeepestLeaf, kLatticeCap) << "relief no longer reaches the cap near the camera";
    EXPECT_GT(ridges.Live, split.Live);
    EXPECT_TRUE(ridges.Watertight);
    inst.Shutdown();
    m_Device->DestroyTexture(flat);
}

TEST_F(CBTContentAwareSplit, ThePagedResolveSplitsFlatGroundLikeTheUnifiedTexture)
{
    // The paged arm keeps the unified texture bound for the range pyramid. Its empty page table
    // resolves every vertex to the terrain's base, so both arms stand on the same flat ground.
    const TextureHandle flat = MakeFlatTexture(0.0f);
    ASSERT_TRUE(flat.IsValid());
    auto runFlat = [&](float heightArm) {
        CBTInstance inst;
        EXPECT_TRUE(inst.Initialize(*m_Device, m_KernelSet) && inst.InitializeRoots(kDomainPlanar));
        BindEverySlot(inst, flat);
        CBTFrameParams p = WalkParams(kLatticeCap, kFlatPx, 0.0f);
        p.AtlasParams1[2] = heightArm;
        const Result r = Converge(inst, p, kLatticeCap, kConvergeFrames, 0u);
        inst.Shutdown();
        return r;
    };
    const Result unified = runFlat(0.0f);
    const Result paged = runFlat(2.0f);
    std::printf("flat ground: unified arm %u live, paged arm %u live\n", unified.Live, paged.Live);
    EXPECT_EQ(paged.Live, unified.Live) << "the paged arm skipped the content-aware split";
    EXPECT_TRUE(paged.Watertight);
    m_Device->DestroyTexture(flat);
}

TEST_F(CBTContentAwareSplit, FlatteningTheHeightsCoarsensARefinedTree)
{
    const TextureHandle flat = MakeFlatTexture(0.5f);
    ASSERT_TRUE(flat.IsValid());
    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet) && inst.InitializeRoots(kDomainPlanar));
    Result ridges;
    {
        ReliefHeightSource relief(*m_Device, inst, kHeightScaleM, kLatticeDim);
        ASSERT_TRUE(relief.IsArmed());
        ridges = Converge(inst, WalkParams(kLatticeCap, kFlatPx, ReliefNormalizedAt(0.5f, 0.5f)), kLatticeCap,
                          kConvergeFrames, 0u);
        BindEverySlot(inst, flat); // before the relief texture is destroyed
    }
    const Result flattened =
        Converge(inst, WalkParams(kLatticeCap, kFlatPx, 0.5f), kLatticeCap, kConvergeFrames, kConvergeFrames);
    std::printf("relief %u live, then flattened %u live\n", ridges.Live, flattened.Live);
    EXPECT_LT(flattened.Live * 4u, ridges.Live) << "a refined tree did not coarsen once the ground went flat";
    EXPECT_EQ(flattened.TailLiveMin, flattened.TailLiveMax) << "the flattened tree did not settle";
    EXPECT_TRUE(flattened.Watertight);
    inst.Shutdown();
    m_Device->DestroyTexture(flat);
}

TEST_F(CBTContentAwareSplit, SmallBumpInFrontOfTheCameraRefines)
{
    // 40 m from two edges: far from the root facets' split-edge midpoints, which sit at the
    // terrain's centre line, so a midpoint distance would read hundreds of metres here.
    const float errPx = FeatureErrorPx(FeatureField(0.5f, 0.3f, 40.0f, 40.0f, 3.0f), 0.5f, 0.3f, 40.0f, 40.0f,
                                       12.0f, "0.3 m bump 12 m ahead, terrain corner");
    EXPECT_LT(errPx, kDrawnPx) << "a 0.3 m bump 12 m in front of the camera was not drawn";
}

TEST_F(CBTContentAwareSplit, HeightsOutsideZeroToOneRefine)
{
    const float hillPx = FeatureErrorPx(FeatureField(1.1f, 3.0f, 300.0f, 300.0f, 3.0f), 1.1f, 3.0f, 300.0f,
                                        300.0f, 12.0f, "3 m hill on ground at normalized 1.1");
    EXPECT_LT(hillPx, kDrawnPx) << "a hill on ground above normalized 1 was not drawn";
    const float ditchPx = FeatureErrorPx(FeatureField(-0.3f, -3.0f, 300.0f, 300.0f, 3.0f), -0.3f, -3.0f, 300.0f,
                                         300.0f, 12.0f, "3 m ditch in ground at normalized -0.3");
    EXPECT_LT(ditchPx, kDrawnPx) << "a ditch in ground below normalized 0 was not drawn";
}

TEST_F(CBTContentAwareSplit, InPlaceEditAnnouncedByADirtyRectCoarsens)
{
    std::vector<float> relief(static_cast<size_t>(kLatticeDim) * kLatticeDim);
    const float spacing = kTerrainM / static_cast<float>(kLatticeDim - 1u);
    for (uint32_t y = 0; y < kLatticeDim; ++y)
        for (uint32_t x = 0; x < kLatticeDim; ++x)
            relief[static_cast<size_t>(y) * kLatticeDim + x] =
                0.5f + (3.0f / kHeightScaleM) * std::sin(static_cast<float>(x) * spacing * 0.3f) *
                           std::sin(static_cast<float>(y) * spacing * 0.3f);
    const TextureHandle tex = MakeTexture(relief);
    ASSERT_TRUE(tex.IsValid());
    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet) && inst.InitializeRoots(kDomainPlanar));
    BindEverySlot(inst, tex);
    const CBTFrameParams p = WalkParams(kLatticeCap, kFlatPx, 0.5f);
    const Result ridged = Converge(inst, p, kLatticeCap, kConvergeFrames, 0u);
    Upload(tex, std::vector<float>(relief.size(), 0.5f), ResourceState::ShaderResource);
    const Result flat = Converge(inst, p, kLatticeCap, kConvergeFrames, kConvergeFrames,
                                 /*forceFirst*/ false, /*editFirst*/ true);
    std::printf("in-place edit: relief %u live, flattened (edit rect only) %u live\n", ridged.Live, flat.Live);
    EXPECT_LT(flat.Live * 4u, ridged.Live) << "an in-place flatten announced by an edit rect did not coarsen";
    EXPECT_TRUE(flat.Watertight);
    inst.Shutdown();
    m_Device->DestroyTexture(tex);
}
