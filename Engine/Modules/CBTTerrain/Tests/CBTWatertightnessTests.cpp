// Watertightness of the live tree under the PRODUCTION classify over a camera path, audited on
// the CPU from the HeapIDs (CBTTreeAudit) after the updates — the invariant a hole in the drawn
// terrain violates. The decode-golden oracle covers the deterministic depth-target metric with no
// camera; these suites audit the tree the shipped screen-space model builds while a camera crosses
// LOD transitions, steep relief, the terrain edge, a teleport, and a live height edit through the
// C5 dirty-rect path, and on the closed sphere across an orbit-to-surface descent.
//
//   PlanarCameraPathStaysWatertight — the shipped walking model (perspective, relief at the
//     shipped heightScale, demand tuning, priority ordering, near-field gate, pool pressure) over
//     ten poses plus an edit, audited on the first, second, fourth and last frame of each pose.
//   SphericalCameraPathStaysWatertight — the planet from orbit to a tangent walk; every edge of
//     the closed manifold must have exactly two owners, cube edges included.
//   CBTPathDeterminism — the same path from the same roots twice; the logical tree (the live
//     HeapID multiset, slot placement ignored) must agree frame for frame until the first Split
//     reservation rollback, and on every frame when the target never rolls one back.
//   WideViewDownwardPoseSettlesThePool — the planar off-frustum gate at views whose ungated
//     off-frustum field does not fit (after a walk, and near nadir from 10 to 20 m): occupancy must
//     settle instead of cycling between a refill and a merge cascade, without rolling back a split,
//     and the tree it rests on must be watertight.
//   CBTTreeAuditRedStates — the audit itself: one read-back tree damaged once per defect class
//     (a dropped bisector, a T-junction, an overlap, a duplicated or malformed leaf, a mis-written
//     corner, a foreign link, a zombie bit, a broken draw stream), each of which it must report.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "CBTTerrain/CBTDeepDecode.h"
#include "CBTTerrain/CBTDemandTuning.h"
#include "CBTTerrain/CBTInstance.h"
#include "CBTTerrain/CBTKernelSet.h"
#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrain/CBTNearBias.h"
#include "CBTTerrain/CBTSphereRoots.h"
#include "CBTTerrain/CBTTreeAudit.h"
#include "CBTReliefHeightSource.h"
#include "CBTTestHarness.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Core/CommandList.h"

using namespace GameEngine::CBTTerrain;
using namespace GameEngine::CBTTerrain::Test;
using namespace GameEngine::Rendering;
using GameEngine::Mathematics::Matrix4x4;
using GameEngine::Mathematics::Vector3;

namespace
{
constexpr float kScreenW = 1600.0f;
constexpr float kScreenH = 900.0f;
// The tex_planar_notex live pose (CBTPlanarDemandGateTests): 1024 m, auto MaxDepth 23, an 8 px split
// threshold at kScreenH rows (the shipped default of 11 applies 9.17 px there), and the ComposedIsland
// heightScale of 60 the census's relief arm runs.
constexpr float kSizeM = 1024.0f;
constexpr uint32_t kMaxDepth = 23u;
constexpr float kSplitPx = 8.0f;
constexpr float kMergePx = kSplitPx * 0.5f; // CBTRenderFeature: the hysteresis band
constexpr float kHeightScale = 60.0f;
constexpr uint32_t kReliefDim = 512u;
constexpr uint32_t kFramesPerPose = 12u;
// The frames audited within a pose: the teleport frame, the first settle, one mid-settle, and the
// converged tail. Every frame is validated by the GPU kernel; the CPU audit is the expensive one.
constexpr std::array<uint32_t, 4> kAuditFrames = {0u, 1u, 3u, kFramesPerPose - 1u};
constexpr float kEyeHeightM = 2.0f;

// Sphere arm: the R=1000 planet the cross-face oracles run, at the shipped octave count.
constexpr float kPlanetRadius = 1000.0f;
constexpr float kPlanetReliefAmp = 8.0f;
constexpr float kPlanetReliefFreq = 5.0f;
constexpr float kPlanetOctaves = 4.0f;
constexpr uint32_t kPlanetCapSubdiv = 16u;

struct Pose
{
    const char* Label;
    Vector3 Eye;
    Vector3 Target;
    Vector3 Up;
};

// The relief the test texture holds (MakeReliefHeightTexture), in world metres, so a walking
// pose can stand on the ground rather than under it.
float ReliefHeightAt(float u, float v)
{
    float amp = 0.5f, sum = 0.0f, norm = 0.0f;
    for (uint32_t octave = 0; octave < 6u; ++octave)
    {
        sum += amp * ReliefSmoothNoiseAt(u, v, 4u << octave);
        norm += amp;
        amp *= 0.5f;
    }
    return sum / norm * kHeightScale;
}

Vector3 Ground(float x, float z, float above)
{
    return Vector3(x, ReliefHeightAt(x / kSizeM, z / kSizeM) + above, z);
}

// The steepest point of the relief on a coarse scan: the pose that puts a near-vertical facet
// under the camera.
Vector3 SteepestGround()
{
    constexpr uint32_t kScan = 64u;
    constexpr float kStep = kSizeM / kScan;
    float best = -1.0f;
    Vector3 at(kSizeM * 0.5f, 0.0f, kSizeM * 0.5f);
    for (uint32_t j = 2; j + 2 < kScan; ++j)
        for (uint32_t i = 2; i + 2 < kScan; ++i)
        {
            const float x = (static_cast<float>(i) + 0.5f) * kStep;
            const float z = (static_cast<float>(j) + 0.5f) * kStep;
            const float gx = Ground(x + kStep, z, 0.0f).y - Ground(x - kStep, z, 0.0f).y;
            const float gz = Ground(x, z + kStep, 0.0f).y - Ground(x, z - kStep, 0.0f).y;
            const float g = std::sqrt(gx * gx + gz * gz);
            if (g > best)
            {
                best = g;
                at = Vector3(x, 0.0f, z);
            }
        }
    return at;
}

std::vector<Pose> PlanarPath()
{
    const Vector3 up(0.0f, 1.0f, 0.0f);
    const Vector3 steep = SteepestGround();
    const Vector3 steepEye = Ground(steep.x - 6.0f, steep.z, kEyeHeightM);
    std::vector<Pose> path;
    const Vector3 walk = Ground(512.0f, 512.0f, kEyeHeightM);
    path.push_back({"walk-centre", walk, walk + Vector3(0.0f, 0.0f, 1.0f), up});
    path.push_back({"walk-yaw", walk, walk + Vector3(1.0f, 0.0f, 0.0f), up});
    path.push_back({"look-down", Ground(512.0f, 512.0f, 30.0f),
                    Ground(512.0f, 512.0f, 30.0f) + Vector3(0.0f, -0.87f, 0.5f), up});
    path.push_back({"fly-mid", Vector3(300.0f, 300.0f, 300.0f), Vector3(700.0f, 0.0f, 700.0f), up});
    path.push_back({"edge-out", Ground(1010.0f, 512.0f, 3.0f),
                    Ground(1010.0f, 512.0f, 3.0f) + Vector3(1.0f, 0.0f, 0.0f), up});
    path.push_back({"edge-in", Vector3(1150.0f, 20.0f, 512.0f), Vector3(900.0f, 0.0f, 512.0f), up});
    path.push_back({"teleport-corner", Ground(60.0f, 60.0f, kEyeHeightM),
                    Ground(60.0f, 60.0f, kEyeHeightM) + Vector3(0.0f, 0.0f, 1.0f), up});
    path.push_back({"steep", steepEye, Ground(steep.x + 6.0f, steep.z, 0.0f), up});
    path.push_back({"orbit-down", Vector3(512.0f, 2000.0f, 512.0f), Vector3(512.0f, 0.0f, 512.0f),
                    Vector3(0.0f, 0.0f, 1.0f)});
    path.push_back({"corner-grazing", Ground(5.0f, 5.0f, 1.0f), Vector3(1024.0f, 0.0f, 1024.0f), up});
    return path;
}

void PlanarParams(CBTFrameParams& p, const Pose& pose)
{
    using namespace GameEngine::Mathematics;
    p = CBTFrameParams{};
    p.Screen[0] = kScreenW;
    p.Screen[1] = kScreenH;
    p.Screen[2] = kSplitPx;
    p.Screen[3] = kMergePx;
    p.TerrainSize[0] = kSizeM;
    p.TerrainSize[1] = kSizeM;
    p.TerrainSize[2] = kHeightScale;
    p.TerrainSize[3] = 0.0f;
    p.TerrainOrigin[2] = static_cast<float>(kMaxDepth);

    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(1.05f, kScreenW / kScreenH, 0.5f, 20000.0f);
    const Matrix4x4 vp = proj * MakeLookAtLH(pose.Eye, pose.Target, pose.Up);
    std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
    p.CameraPos[0] = pose.Eye.x;
    p.CameraPos[1] = pose.Eye.y;
    p.CameraPos[2] = pose.Eye.z;
    p.CameraPos[3] = 1.0f;

    const CBTNearBiasRadii nb = ComputeNearBiasRadii(pose.Eye.y);
    p.NearBias[0] = 1.0f;
    p.NearBias[1] = nb.NearRadius;
    p.NearBias[2] = nb.FarRadius;
    p.NearBias[3] = kNearBiasMaxCoarsen;
    p.DemandTuning[0] = kNearFieldFacetTargetM;
    p.DemandTuning[1] = kOffFrustumKeepOcc;
    p.DemandTuning[2] = kEdgeRescueTpeMul * kSplitPx;
    p.DemandTuning[3] = kEdgeRescueOcc;
    p.PriorityParams[0] = 1.0f;
    p.PriorityParams[1] = kPriorityRampStartOcc;
    p.PriorityParams[2] = kPriorityMaxAreaFloorPx2;
    p.PriorityParams[3] = kPriorityKeepLargeNdc;
}

std::vector<Pose> SpherePath()
{
    const float r = kPlanetRadius;
    const Vector3 centre(0.0f, 0.0f, 0.0f);
    const Vector3 surface(0.0f, 0.0f, -(r + kEyeHeightM));
    std::vector<Pose> path;
    path.push_back({"orbit", Vector3(0.0f, 0.0f, -3.0f * r), centre, Vector3(0.0f, 1.0f, 0.0f)});
    path.push_back({"descent", Vector3(0.0f, 0.0f, -(r + 300.0f)), centre, Vector3(0.0f, 1.0f, 0.0f)});
    path.push_back({"approach", Vector3(0.0f, 0.0f, -(r + 60.0f)), centre, Vector3(0.0f, 1.0f, 0.0f)});
    path.push_back({"surface-tangent", surface, surface + Vector3(1.0f, 0.0f, 0.0f),
                    Vector3(0.0f, 0.0f, -1.0f)});
    path.push_back({"surface-yaw", surface, surface + Vector3(0.0f, 1.0f, 0.05f),
                    Vector3(0.0f, 0.0f, -1.0f)});
    // A cube edge in the near field: the eye above the (+X,-Z) edge, looking along it.
    const Vector3 edgeEye = Vector3(1.0f, 0.0f, -1.0f) * (r + 5.0f) / std::sqrt(2.0f);
    path.push_back({"cube-edge", edgeEye, edgeEye + Vector3(0.0f, 1.0f, 0.0f),
                    Vector3(1.0f, 0.0f, -1.0f) / std::sqrt(2.0f)});
    path.push_back({"climb-out", Vector3(0.0f, 0.0f, -(r + 1500.0f)), centre, Vector3(0.0f, 1.0f, 0.0f)});
    return path;
}

void SphereParams(CBTFrameParams& p, const Pose& pose)
{
    using namespace GameEngine::Mathematics;
    p = CBTFrameParams{};
    p.PlanetParams[0] = kPlanetRadius;
    p.PlanetParams[1] = kPlanetReliefAmp;
    p.PlanetParams[2] = kPlanetReliefFreq;
    p.PlanetParams[3] = kPlanetOctaves;
    p.Screen[0] = kScreenW;
    p.Screen[1] = kScreenH;
    p.Screen[2] = kSplitPx;
    p.Screen[3] = kMergePx;
    p.TerrainOrigin[2] = static_cast<float>(kSphereBaseDepth + kPlanetCapSubdiv);

    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(1.2f, kScreenW / kScreenH, 0.5f, 100000.0f);
    const Matrix4x4 vp = proj * MakeLookAtLH(pose.Eye, pose.Target, pose.Up);
    std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
    p.CameraPos[0] = pose.Eye.x;
    p.CameraPos[1] = pose.Eye.y;
    p.CameraPos[2] = pose.Eye.z;
    p.CameraPos[3] = 1.0f;

    const float altitude = std::max(pose.Eye.Length() - kPlanetRadius, kEyeHeightM);
    const CBTNearBiasRadii nb = ComputeNearBiasRadii(altitude);
    p.NearBias[0] = 1.0f;
    p.NearBias[1] = nb.NearRadius;
    p.NearBias[2] = nb.FarRadius;
    p.NearBias[3] = kNearBiasMaxCoarsen;
    p.DemandTuning[0] = kNearFieldFacetTargetM;
    p.DemandTuning[1] = kOffFrustumKeepOcc;
    p.DemandTuning[2] = kEdgeRescueTpeMul * kSplitPx;
    p.DemandTuning[3] = kEdgeRescueOcc;
    p.PriorityParams[0] = 1.0f;
    p.PriorityParams[1] = kPriorityRampStartOcc;
    p.PriorityParams[2] = kPriorityMaxAreaFloorPx2;
    p.PriorityParams[3] = kPriorityKeepLargeNdc;
}

// The classify the editor ships (CBTUpdateSystem): screen-space, every gate on.
CBTClassifyDesc ProductionClassify(uint32_t maxDepth)
{
    CBTClassifyDesc c{};
    c.Mode = kClassifyScreenSpace;
    c.TargetDepth = maxDepth;
    c.GateVertexEval = 1u;
    c.EditRetessEnabled = 1u;
    c.NearFieldGate = 1u;
    c.PoolPressure = 1u;
    return c;
}

std::string Describe(const CBTTreeAuditResult& a)
{
    std::ostringstream s;
    s << "live=" << a.LiveCount << " maxDepth=" << a.MaxDepth
      << " nonConformingEdges=" << a.NonConformingEdges << " overlappingLeaves=" << a.OverlappingLeaves
      << " incompleteRoots=" << a.IncompleteRoots << " malformedLeaves=" << a.MalformedLeaves
      << " nonReciprocalLinks=" << a.NonReciprocalLinks << " staleCornerUVs=" << a.StaleCornerUVs
      << " zombies=" << a.Zombies << " streamAllErrors=" << a.StreamAllErrors
      << " streamVisibleErrors=" << a.StreamVisibleErrors;
    if (!a.SampleBadHeapIds.empty())
    {
        s << " sample=[";
        for (uint64_t h : a.SampleBadHeapIds)
            s << h << ' ';
        s << ']';
    }
    return s.str();
}

// One update recorded on its own command list and waited on, so the next readback sees its tail.
void RecordAndWait(IDevice& device, CBTInstance& inst, const CBTClassifyDesc& desc,
                   const CBTFrameParams& params, uint32_t frame)
{
    auto cl = device.CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    inst.RecordUpdate(*cl, desc, params, frame);
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    device.ExecuteCommandLists(lists);
    device.WaitForIdle();
}

// Overwrite a texel rectangle of the R32_FLOAT relief texture with one value — a plateau — the way
// a sculpt bake lands in the unified heightmap. The region copy needs the texture's resting state
// (a whole-texture Undefined transition would discard the untouched texels).
void RaisePlateau(IDevice& device, TextureHandle tex, uint32_t x0, uint32_t y0, uint32_t w,
                  uint32_t h, float value)
{
    std::vector<float> data(static_cast<size_t>(w) * h, value);
    const size_t bytes = data.size() * sizeof(float);
    BufferHandle staging = device.CreateUploadBuffer(bytes, "CBT.Test.PlateauStaging");
    device.UpdateBuffer(staging, 0, bytes, data.data());
    auto cl = device.CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->CopyBufferToTextureSubresource(staging, tex, 0, 0, w, h, 0, static_cast<size_t>(w) * sizeof(float),
                                       1, 0, x0, y0, ResourceState::ShaderResource);
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(tex, ResourceState::CopyDest,
                                                      ResourceState::ShaderResource));
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    device.ExecuteCommandLists(lists);
    device.WaitForIdle();
    device.DestroyBuffer(staging);
}
} // namespace

class CBTWatertightness : public ::testing::Test
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
        if (m_Relief.IsValid() && m_Device)
            m_Device->DestroyTexture(m_Relief);
        m_KernelSet.Shutdown();
        if (m_Device)
            m_Device->Shutdown();
    }

    void RunFrame(CBTInstance& inst, const CBTClassifyDesc& desc, const CBTFrameParams& params,
                  uint32_t frame)
    {
        RecordAndWait(*m_Device, inst, desc, params, frame);
    }

    // The relief texture, created once per test and kept so the edit arm can overwrite a region.
    void CreateRelief()
    {
        m_Relief = MakeReliefHeightTexture(*m_Device, kReliefDim);
        ASSERT_TRUE(m_Relief.IsValid());
    }

    // Bound to every ring slot, so any frame index samples the relief.
    void BindRelief(CBTInstance& inst)
    {
        for (uint32_t slot = 0; slot < kCBTFrameParamsRing; ++slot)
            inst.SetHeightSource(slot, m_Relief);
    }

    // The CPU audit plus the GPU kernel's own counters, at one instant.
    void ExpectClean(CBTInstance& inst, const char* label, uint32_t frame)
    {
        const CBTTreeSnapshot snap = ReadTreeSnapshot(inst, true);
        const CBTTreeAuditResult a = AuditTree(snap);
        std::printf("[watertight] %-16s frame %2u %s\n", label, frame, Describe(a).c_str());
        EXPECT_TRUE(a.Clean()) << label << " frame " << frame << ": " << Describe(a);
        EXPECT_EQ(inst.ReadValidationErrorCount(), 0u) << label << " frame " << frame;
        EXPECT_EQ(inst.ReadValidationCounter(kValidationBudgetCounter), 0u) << label << " frame " << frame;
        EXPECT_EQ(inst.ReadValidationCounter(kValidationZombieCounter), 0u) << label << " frame " << frame;
        EXPECT_EQ(inst.ReadValidationCounter(kValidationCompactCounter), 0u) << label << " frame " << frame;
    }

    // Run one pose for kFramesPerPose updates, auditing the frames in kAuditFrames.
    void RunPose(CBTInstance& inst, CBTClassifyDesc& classify, const CBTFrameParams& params,
                 const char* label, uint32_t& frameCounter, bool audit)
    {
        for (uint32_t f = 0; f < kFramesPerPose; ++f, ++frameCounter)
        {
            RunFrame(inst, classify, params, frameCounter);
            // An edit rect is consumed by the one classify it is handed to (CBTUpdateSystem).
            classify.DirtyMinU = classify.DirtyMinV = classify.DirtyMaxU = classify.DirtyMaxV = 0.0f;
            if (audit && std::find(kAuditFrames.begin(), kAuditFrames.end(), f) != kAuditFrames.end())
                ExpectClean(inst, label, f);
        }
    }

    std::unique_ptr<IDevice> m_Device;
    CBTKernelSet m_KernelSet;
    TextureHandle m_Relief;
};

TEST_F(CBTWatertightness, PlanarCameraPathStaysWatertight)
{
    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(inst.InitializeRoots(kDomainPlanar));
    CreateRelief();
    if (HasFatalFailure())
        return;
    BindRelief(inst);

    CBTClassifyDesc classify = ProductionClassify(kMaxDepth);
    uint32_t frame = 0;
    CBTFrameParams p{};
    const std::vector<Pose> path = PlanarPath();
    for (const Pose& pose : path)
    {
        PlanarParams(p, pose);
        RunPose(inst, classify, p, pose.Label, frame, true);
        if (HasFatalFailure())
            return;
    }

    // A live edit under the walking camera: a plateau in the middle of the terrain, handed to
    // Classify as the C5 dirty rect (one texel of apron, as CBTUpdateSystem publishes it), then
    // a walk onto it while the crease re-tessellates.
    constexpr uint32_t kPlateauX = 260u, kPlateauY = 268u, kPlateauSize = 24u;
    RaisePlateau(*m_Device, m_Relief, kPlateauX, kPlateauY, kPlateauSize, kPlateauSize, 0.9f);
    const float invDim = 1.0f / static_cast<float>(kReliefDim);
    classify.DirtyMinU = (static_cast<float>(kPlateauX) - 1.0f) * invDim;
    classify.DirtyMinV = (static_cast<float>(kPlateauY) - 1.0f) * invDim;
    classify.DirtyMaxU = (static_cast<float>(kPlateauX + kPlateauSize) + 1.0f) * invDim;
    classify.DirtyMaxV = (static_cast<float>(kPlateauY + kPlateauSize) + 1.0f) * invDim;
    const Vector3 walk = Ground(512.0f, 512.0f, kEyeHeightM);
    const Pose editPose{"edit-walk", walk, walk + Vector3(0.0f, 0.0f, 1.0f), Vector3(0.0f, 1.0f, 0.0f)};
    PlanarParams(p, editPose);
    RunPose(inst, classify, p, editPose.Label, frame, true);
    const Vector3 onPlateau(544.0f, 0.9f * kHeightScale + kEyeHeightM, 545.0f);
    const Pose plateauPose{"edit-onto", onPlateau, onPlateau + Vector3(0.0f, -0.3f, 1.0f),
                           Vector3(0.0f, 1.0f, 0.0f)};
    PlanarParams(p, plateauPose);
    RunPose(inst, classify, p, plateauPose.Label, frame, true);
}

TEST_F(CBTWatertightness, SphericalCameraPathStaysWatertight)
{
    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(inst.InitializeRoots(kDomainSpherical));

    CBTClassifyDesc classify = ProductionClassify(kSphereBaseDepth + kPlanetCapSubdiv);
    classify.PoolPressure = 0u; // Kernel_Reset pins the planar controller at step 0 on the sphere
    uint32_t frame = 0;
    CBTFrameParams p{};
    for (const Pose& pose : SpherePath())
    {
        SphereParams(p, pose);
        RunPose(inst, classify, p, pose.Label, frame, true);
        if (HasFatalFailure())
            return;
    }
}

// Determinism. The Split kernel reserves its compatibility chain's slot budget (2*depth-1) with
// one atomic on FreeCount and rolls the candidate back when the reservation fails; a reservation
// that another chain's not-yet-refunded over-estimate made fail is decided by dispatch order. So
// the LOGICAL tree (the live HeapID multiset, slot placement ignored) is expected to be exactly
// reproducible until the first rollback, and only rollback frames may diverge. Two arms:
//   * the 8 px target over the walking path: two runs agree frame for frame until the first
//     frame whose cumulative rollback count is non-zero (a divergence before any rollback would be
//     a different, unattributed race);
//   * a target the pool serves without a single rollback: two runs agree on every frame.
namespace
{
struct FrameTrace
{
    std::vector<uint64_t> Signature;
    std::vector<uint32_t> Live;
    std::vector<int32_t> OverflowTotal; // cumulative Split reservation rollbacks (kWQOverflowCounter)
};
} // namespace

class CBTPathDeterminism : public CBTWatertightness
{
  protected:
    FrameTrace TracePath(float splitPx, const std::vector<Pose>& path)
    {
        FrameTrace trace;
        CBTInstance inst;
        EXPECT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
        EXPECT_TRUE(inst.InitializeRoots(kDomainPlanar));
        BindRelief(inst);
        CBTClassifyDesc classify = ProductionClassify(kMaxDepth);
        uint32_t frame = 0;
        CBTFrameParams p{};
        for (const Pose& pose : path)
        {
            PlanarParams(p, pose);
            p.Screen[2] = splitPx;
            p.Screen[3] = splitPx * 0.5f;
            p.DemandTuning[2] = kEdgeRescueTpeMul * splitPx;
            for (uint32_t f = 0; f < kFramesPerPose; ++f, ++frame)
            {
                RunFrame(inst, classify, p, frame);
                const CBTTreeSnapshot snap = ReadTreeSnapshot(inst, false);
                trace.Signature.push_back(TreeSignature(snap));
                trace.Live.push_back(snap.AllCount);
                trace.OverflowTotal.push_back(inst.ReadTessellationStats().OverflowTotal);
            }
        }
        return trace;
    }

    // First frame whose signature differs, or the frame count when the runs agree throughout.
    static size_t FirstDivergence(const FrameTrace& a, const FrameTrace& b)
    {
        const size_t n = std::min(a.Signature.size(), b.Signature.size());
        for (size_t i = 0; i < n; ++i)
            if (a.Signature[i] != b.Signature[i])
                return i;
        return n;
    }
};

TEST_F(CBTPathDeterminism, ShippedTargetIsDeterministicUntilTheFirstReservationRollback)
{
    CreateRelief();
    if (HasFatalFailure())
        return;
    const std::vector<Pose> path = PlanarPath();
    const FrameTrace a = TracePath(kSplitPx, path);
    const FrameTrace b = TracePath(kSplitPx, path);
    ASSERT_EQ(a.Signature.size(), b.Signature.size());
    const size_t first = FirstDivergence(a, b);
    size_t divergent = 0;
    for (size_t i = 0; i < a.Signature.size(); ++i)
        divergent += a.Signature[i] != b.Signature[i] ? 1u : 0u;
    for (size_t i = 0; i < a.Signature.size(); ++i)
    {
        const bool show = i < 4u || (i + 2u >= first && i <= first + 2u) || (i % kFramesPerPose) == kFramesPerPose - 1u;
        if (show)
            std::printf("[determinism] frame %3zu %-16s live %7u/%7u overflow %9d/%9d %s\n", i,
                        path[i / kFramesPerPose].Label, a.Live[i], b.Live[i], a.OverflowTotal[i],
                        b.OverflowTotal[i], a.Signature[i] == b.Signature[i] ? "same" : "DIFFERENT");
    }
    std::printf("[determinism] first divergence at frame %zu of %zu; %zu divergent frames\n", first,
                a.Signature.size(), divergent);
    if (first < a.Signature.size())
    {
        // The frame that diverged, or an earlier one, must have rolled a reservation back in one
        // of the runs. A divergence with zero rollbacks anywhere before it is a race elsewhere.
        EXPECT_TRUE(a.OverflowTotal[first] > 0 || b.OverflowTotal[first] > 0)
            << "runs diverged at frame " << first << " before any Split reservation rollback";
    }
}

// Orbit poses only: a ~30k-leaf tree whose per-frame reservations never reach the free pool, so
// the path runs without a single rollback and the two runs must agree on every frame. (A lower
// pixel target is NOT a rollback-free regime: the near-field bound still refines thousands of
// facets per frame while walking, and the 2*depth-1 reservation of each exhausts the budget.)
std::vector<Pose> OrbitPath()
{
    const Vector3 centre(512.0f, 0.0f, 512.0f);
    const Vector3 up(0.0f, 0.0f, 1.0f);
    std::vector<Pose> path;
    path.push_back({"orbit-2000", Vector3(512.0f, 2000.0f, 512.0f), centre, up});
    path.push_back({"orbit-tilt", Vector3(512.0f, 1800.0f, -600.0f), centre, Vector3(0.0f, 1.0f, 0.0f)});
    path.push_back({"orbit-2500", Vector3(300.0f, 2500.0f, 700.0f), centre, up});
    path.push_back({"orbit-corner", Vector3(1400.0f, 1600.0f, 1400.0f), Vector3(200.0f, 0.0f, 200.0f),
                    Vector3(0.0f, 1.0f, 0.0f)});
    path.push_back({"orbit-2000-back", Vector3(512.0f, 2000.0f, 512.0f), centre, up});
    return path;
}

TEST_F(CBTPathDeterminism, RollbackFreeOrbitPathIsDeterministicOnEveryFrame)
{
    CreateRelief();
    if (HasFatalFailure())
        return;
    const std::vector<Pose> path = OrbitPath();
    const FrameTrace a = TracePath(kSplitPx, path);
    const FrameTrace b = TracePath(kSplitPx, path);
    ASSERT_EQ(a.Signature.size(), b.Signature.size());
    const size_t first = FirstDivergence(a, b);
    std::printf("[determinism-orbit] frames %zu peak live %u/%u overflow %d/%d first divergence %zu"
                "%c",
                a.Signature.size(), *std::max_element(a.Live.begin(), a.Live.end()),
                *std::max_element(b.Live.begin(), b.Live.end()), a.OverflowTotal.back(),
                b.OverflowTotal.back(), first, 10);
    // The arm is only meaningful if the pool really served every reservation.
    ASSERT_EQ(a.OverflowTotal.back(), 0) << "the orbit path still rolled reservations back";
    ASSERT_EQ(b.OverflowTotal.back(), 0);
    EXPECT_EQ(first, a.Signature.size())
        << "the logical tree differs between two rollback-free runs at frame " << first;
}

// The audit's red states. Every test above rests on CBTTreeAudit: an audit that went blind to one
// defect class would leave them all green. One real tree (the fly-mid pose after a settle) is read
// back once for the suite; each case damages a copy the way its defect would, keeping every other
// buffer consistent, and asserts the audit names that defect. The untouched snapshot audits clean.
namespace
{
constexpr uint32_t kRedStateSettleFrames = 12u;
constexpr uint32_t kNotFound = 0xFFFFFFFFu;
// A few levels below the roots, so the damaged leaf is one small facet of a refined region.
constexpr uint32_t kRedStateMinSubdiv = 4u;
// Far enough along the slot order that the foreign leaf is not the damaged leaf's neighbour.
constexpr uint32_t kForeignLinkSkip = 400u;

// No corner on the terrain boundary, so every edge of the leaf has a live neighbour across it (a
// boundary edge splits into two boundary edges and opens no T-junction). Reads the cached corners,
// which equal the decode in the clean snapshot.
bool IsInteriorLeaf(const CBTTreeSnapshot& tree, uint32_t slot)
{
    const CBTVertexData& v = tree.Vertices[slot];
    const float cornerU[3] = {v.Corner0[3], v.Corner1[3], v.Corner2[3]};
    for (uint32_t k = 0; k < 3u; ++k)
        if (cornerU[k] <= 0.0f || cornerU[k] >= 1.0f || v.Meta[k] <= 0.0f || v.Meta[k] >= 1.0f)
            return false;
    return true;
}

// The `skip`-th interior live slot at least kRedStateMinSubdiv below the roots, so distinct cases
// damage distinct leaves.
uint32_t FindDeepLiveSlot(const CBTTreeSnapshot& tree, uint32_t skip)
{
    for (uint32_t slot = 0; slot < tree.PoolSize; ++slot)
    {
        const uint64_t h = tree.HeapIds[slot];
        const uint32_t depth = static_cast<uint32_t>(std::bit_width(h)) - 1u;
        if (h != kFreeSlotHeapID && depth >= tree.BaseDepth + kRedStateMinSubdiv && IsInteriorLeaf(tree, slot) &&
            skip-- == 0u)
            return slot;
    }
    return kNotFound;
}

uint32_t FindFreeSlot(const CBTTreeSnapshot& tree, uint32_t skip)
{
    for (uint32_t slot = 0; slot < tree.PoolSize; ++slot)
        if (tree.HeapIds[slot] == kFreeSlotHeapID && skip-- == 0u)
            return slot;
    return kNotFound;
}

// Writes a slot's HeapID and keeps its occupancy bit in step, as Allocate and Simplify do.
void SetSlotHeapId(CBTTreeSnapshot& tree, uint32_t slot, uint64_t h)
{
    const uint32_t bit = 1u << (slot % 32u);
    tree.HeapIds[slot] = h;
    if (h != kFreeSlotHeapID)
        tree.Bitfield[slot / 32u] |= bit;
    else
        tree.Bitfield[slot / 32u] &= ~bit;
}

void RemoveFromStream(std::vector<uint32_t>& stream, uint32_t& count, uint32_t slot)
{
    const auto end = stream.begin() + count;
    const auto it = std::find(stream.begin(), end, slot);
    if (it == end)
        return;
    stream.erase(it);
    --count;
}

void AppendToStream(std::vector<uint32_t>& stream, uint32_t& count, uint32_t slot)
{
    stream.resize(count);
    stream.push_back(slot);
    ++count;
}

// A new live slot carrying `h`, in the ALL stream and, when the copied flags say VISIBLE, the
// VISIBLE stream.
void AddLiveSlot(CBTTreeSnapshot& tree, uint32_t slot, uint64_t h, const CBTBisectorData& bisector)
{
    SetSlotHeapId(tree, slot, h);
    tree.Bisectors[slot] = bisector;
    AppendToStream(tree.IndicesAll, tree.AllCount, slot);
    if ((bisector.Flags & kFlagVisible) != 0u)
        AppendToStream(tree.IndicesVisible, tree.VisibleCount, slot);
}
} // namespace

class CBTTreeAuditRedStates : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        std::unique_ptr<IDevice> device = MakeHeadlessDevice();
        if (!device || !device->GetCapabilities().supportsShaderInt64)
        {
            s_SkipReason = device ? "device lacks shaderInt64" : "no headless Vulkan device";
            if (device)
                device->Shutdown();
            return;
        }
        CBTKernelSet kernels;
        if (!kernels.Initialize(*device, ShaderOutputDir()))
        {
            s_SkipReason = "cbt_kernels.comp.spv missing (glslc unavailable at build)";
            device->Shutdown();
            return;
        }
        {
            CBTInstance inst;
            if (inst.Initialize(*device, kernels) && inst.InitializeRoots(kDomainPlanar))
            {
                const TextureHandle relief = MakeReliefHeightTexture(*device, kReliefDim);
                for (uint32_t slot = 0; slot < kCBTFrameParamsRing; ++slot)
                    inst.SetHeightSource(slot, relief);
                const CBTClassifyDesc classify = ProductionClassify(kMaxDepth);
                CBTFrameParams params{};
                for (const Pose& pose : PlanarPath())
                    if (std::strcmp(pose.Label, "fly-mid") == 0)
                        PlanarParams(params, pose);
                for (uint32_t frame = 0; frame < kRedStateSettleFrames; ++frame)
                    RecordAndWait(*device, inst, classify, params, frame);
                s_Base = std::make_unique<CBTTreeSnapshot>(ReadTreeSnapshot(inst, true));
                device->DestroyTexture(relief);
            }
        }
        kernels.Shutdown();
        device->Shutdown();
    }
    static void TearDownTestSuite() { s_Base.reset(); }

    void SetUp() override
    {
        if (!s_SkipReason.empty())
            GTEST_SKIP() << s_SkipReason;
        ASSERT_NE(s_Base, nullptr) << "the CBT instance failed to initialize";
        m_Tree = *s_Base;
    }

    // A deep live slot of the copy; distinct `skip` values give distinct leaves.
    uint32_t DeepLive(uint32_t skip) const
    {
        const uint32_t slot = FindDeepLiveSlot(m_Tree, skip);
        EXPECT_NE(slot, kNotFound) << "the settled tree has too few deep leaves";
        return slot;
    }
    uint32_t Free(uint32_t skip) const
    {
        const uint32_t slot = FindFreeSlot(m_Tree, skip);
        EXPECT_NE(slot, kNotFound) << "the pool has no free slot";
        return slot;
    }
    CBTTreeAuditResult Audit(const char* label) const
    {
        const CBTTreeAuditResult audit = AuditTree(m_Tree);
        std::printf("[audit-red] %-22s %s\n", label, Describe(audit).c_str());
        return audit;
    }

    static std::unique_ptr<CBTTreeSnapshot> s_Base;
    static std::string s_SkipReason;
    CBTTreeSnapshot m_Tree;
};

std::unique_ptr<CBTTreeSnapshot> CBTTreeAuditRedStates::s_Base;
std::string CBTTreeAuditRedStates::s_SkipReason;

TEST_F(CBTTreeAuditRedStates, UntouchedSnapshotAuditsClean)
{
    const CBTTreeAuditResult audit = Audit("untouched");
    EXPECT_TRUE(audit.Clean()) << Describe(audit);
    EXPECT_GT(audit.LiveCount, 1000u) << "the settled tree is too small to damage meaningfully";
}

TEST_F(CBTTreeAuditRedStates, DroppedBisectorLeavesTheRootIncomplete)
{
    const uint32_t slot = DeepLive(0u);
    ASSERT_NE(slot, kNotFound);
    SetSlotHeapId(m_Tree, slot, kFreeSlotHeapID);
    RemoveFromStream(m_Tree.IndicesAll, m_Tree.AllCount, slot);
    RemoveFromStream(m_Tree.IndicesVisible, m_Tree.VisibleCount, slot);
    const CBTTreeAuditResult audit = Audit("dropped-bisector");
    EXPECT_GT(audit.IncompleteRoots, 0u);
    EXPECT_GT(audit.NonConformingEdges, 0u);
    EXPECT_FALSE(audit.Watertight());
}

// A leaf replaced by its two children with the neighbour across its longest edge left whole: the
// root is still tiled exactly and nothing overlaps, so only the conformity check can see it.
TEST_F(CBTTreeAuditRedStates, TJunctionFailsConformityAlone)
{
    const uint32_t slot = DeepLive(7u);
    const uint32_t spare = Free(0u);
    ASSERT_NE(slot, kNotFound);
    ASSERT_NE(spare, kNotFound);
    const uint64_t h = m_Tree.HeapIds[slot];
    SetSlotHeapId(m_Tree, slot, h << 1);
    AddLiveSlot(m_Tree, spare, (h << 1) | 1u, m_Tree.Bisectors[slot]);
    const CBTTreeAuditResult audit = Audit("t-junction");
    EXPECT_GT(audit.NonConformingEdges, 0u);
    EXPECT_EQ(audit.IncompleteRoots, 0u);
    EXPECT_EQ(audit.OverlappingLeaves, 0u);
}

TEST_F(CBTTreeAuditRedStates, LiveChildUnderALiveParentOverlaps)
{
    const uint32_t slot = DeepLive(11u);
    const uint32_t spare = Free(0u);
    ASSERT_NE(slot, kNotFound);
    ASSERT_NE(spare, kNotFound);
    AddLiveSlot(m_Tree, spare, m_Tree.HeapIds[slot] << 1, m_Tree.Bisectors[slot]);
    const CBTTreeAuditResult audit = Audit("overlap");
    EXPECT_GT(audit.OverlappingLeaves, 0u);
    EXPECT_FALSE(audit.Watertight());
}

TEST_F(CBTTreeAuditRedStates, DuplicatedLeafDoublesItsEdges)
{
    const uint32_t slot = DeepLive(13u);
    const uint32_t spare = Free(0u);
    ASSERT_NE(slot, kNotFound);
    ASSERT_NE(spare, kNotFound);
    AddLiveSlot(m_Tree, spare, m_Tree.HeapIds[slot], m_Tree.Bisectors[slot]);
    const CBTTreeAuditResult audit = Audit("duplicated-leaf");
    EXPECT_GT(audit.NonConformingEdges, 0u);
    EXPECT_GT(audit.IncompleteRoots, 0u);
}

// A HeapID shallower than the root band has no root to decode against.
TEST_F(CBTTreeAuditRedStates, HeapIdAboveTheRootBandIsMalformed)
{
    const uint32_t slot = DeepLive(3u);
    ASSERT_NE(slot, kNotFound);
    SetSlotHeapId(m_Tree, slot, uint64_t(1));
    const CBTTreeAuditResult audit = Audit("malformed");
    EXPECT_EQ(audit.MalformedLeaves, 1u);
    EXPECT_FALSE(audit.Watertight());
}

// The corner mis-write the stale-UV check exists for: every corner of one record as (u+v, u+v).
TEST_F(CBTTreeAuditRedStates, LaneSumCornerWriteIsStaleButWatertight)
{
    const uint32_t slot = DeepLive(17u);
    ASSERT_NE(slot, kNotFound);
    CBTVertexData& v = m_Tree.Vertices[slot];
    float* corners[3] = {v.Corner0, v.Corner1, v.Corner2};
    for (uint32_t k = 0; k < 3u; ++k)
    {
        const float sum = corners[k][3] + v.Meta[k];
        corners[k][3] = sum;
        v.Meta[k] = sum;
    }
    const CBTTreeAuditResult audit = Audit("lane-sum");
    EXPECT_EQ(audit.StaleCornerUVs, 1u);
    EXPECT_TRUE(audit.Watertight());
    EXPECT_FALSE(audit.Clean());
}

// The tolerance sits four decades below the finest facet edge, so a one-millionth shift is stale.
TEST_F(CBTTreeAuditRedStates, OneMillionthUShiftIsStale)
{
    const uint32_t slot = DeepLive(19u);
    ASSERT_NE(slot, kNotFound);
    m_Tree.Vertices[slot].Corner1[3] += 1e-6f;
    const CBTTreeAuditResult audit = Audit("u-shift");
    EXPECT_EQ(audit.StaleCornerUVs, 1u);
}

TEST_F(CBTTreeAuditRedStates, ForeignLinkIsNonReciprocal)
{
    const uint32_t slot = DeepLive(23u);
    ASSERT_NE(slot, kNotFound);
    // A far leaf whose own record does not already point at `slot`.
    uint32_t stranger = kNotFound;
    for (uint32_t skip = kForeignLinkSkip; stranger == kNotFound; ++skip)
    {
        const uint32_t candidate = DeepLive(skip);
        ASSERT_NE(candidate, kNotFound);
        const CBTNeighbors& n = m_Tree.Neighbors[candidate];
        if (n.Neighbor0 != slot && n.Neighbor1 != slot && n.Twin != slot)
            stranger = candidate;
    }
    m_Tree.Neighbors[slot].Neighbor0 = stranger;
    const CBTTreeAuditResult audit = Audit("foreign-link");
    EXPECT_GT(audit.NonReciprocalLinks, 0u);
    EXPECT_TRUE(audit.Watertight());
}

TEST_F(CBTTreeAuditRedStates, OccupancyBitOnAFreeSlotIsAZombie)
{
    const uint32_t spare = Free(3u);
    ASSERT_NE(spare, kNotFound);
    m_Tree.Bitfield[spare / 32u] |= 1u << (spare % 32u);
    const CBTTreeAuditResult audit = Audit("zombie");
    EXPECT_EQ(audit.Zombies, 1u);
}

TEST_F(CBTTreeAuditRedStates, LiveSlotMissingFromTheAllStream)
{
    ASSERT_GE(m_Tree.AllCount, 1u);
    RemoveFromStream(m_Tree.IndicesAll, m_Tree.AllCount, m_Tree.IndicesAll[0]);
    const CBTTreeAuditResult audit = Audit("all-stream-missing");
    EXPECT_GT(audit.StreamAllErrors, 0u);
}

TEST_F(CBTTreeAuditRedStates, DuplicatedVisibleEntry)
{
    ASSERT_GE(m_Tree.VisibleCount, 2u);
    m_Tree.IndicesVisible[1] = m_Tree.IndicesVisible[0];
    const CBTTreeAuditResult audit = Audit("visible-duplicate");
    EXPECT_GT(audit.StreamVisibleErrors, 0u);
}

// The signature names the logical tree: blind to which slot a bisector sits in, not to the tree.
TEST_F(CBTTreeAuditRedStates, SignatureIgnoresSlotsButNotTheTree)
{
    const uint64_t base = TreeSignature(m_Tree);
    const uint32_t live = DeepLive(0u);
    const uint32_t spare = Free(0u);
    ASSERT_NE(live, kNotFound);
    ASSERT_NE(spare, kNotFound);
    CBTTreeSnapshot moved = m_Tree;
    std::swap(moved.HeapIds[live], moved.HeapIds[spare]);
    EXPECT_EQ(TreeSignature(moved), base);
    SetSlotHeapId(m_Tree, live, kFreeSlotHeapID);
    EXPECT_NE(TreeSignature(m_Tree), base);
}

// The off-frustum gate's regime. A 3067x900 scene view (126 degrees horizontal) over a 576 m
// terrain at the 2 samples/m cap (maxDepth 21, TargetPixelError 8), looking down from 10 m to 34 m,
// asks for more than the pool holds once the whole off-frustum field is kept. The planar
// off-frustum gate must settle the pool there rather than flip it between keeping and releasing that
// field on alternate updates, or every few updates: each flip is a refill of hundreds of thousands of
// splits that rolls back what the pool cannot hold, then a merge cascade. Four arms, each on a fresh
// tree: the walk at four headings then straight down from 34 m (the editor sequence), and three
// near-nadir views from roots. Every update of the final pose is read back; its tail must hold its
// occupancy without rolling back a split, and the tree it rests on must be watertight.
namespace
{
constexpr float kEditorScreenW = 3067.0f;
constexpr float kEditorScreenH = 900.0f;
constexpr float kEditorSizeM = 576.0f;
constexpr uint32_t kEditorMaxDepth = 21u;
constexpr float kEditorHeightScale = 6.0f;
constexpr float kEditorFovVertical = 1.0472f; // 60 degrees

float EditorReliefHeightAt(float x, float z)
{
    const float u = x / kEditorSizeM, v = z / kEditorSizeM;
    float amp = 0.5f, sum = 0.0f, norm = 0.0f;
    for (uint32_t octave = 0; octave < 6u; ++octave)
    {
        sum += amp * ReliefSmoothNoiseAt(u, v, 4u << octave);
        norm += amp;
        amp *= 0.5f;
    }
    return sum / norm * kEditorHeightScale;
}

void EditorParams(CBTFrameParams& p, const Vector3& eye, float yawDeg, float pitchDeg)
{
    using namespace GameEngine::Mathematics;
    p = CBTFrameParams{};
    p.Screen[0] = kEditorScreenW;
    p.Screen[1] = kEditorScreenH;
    p.Screen[2] = kSplitPx;
    p.Screen[3] = kMergePx;
    p.TerrainSize[0] = kEditorSizeM;
    p.TerrainSize[1] = kEditorSizeM;
    p.TerrainSize[2] = kEditorHeightScale;
    p.TerrainSize[3] = 0.0f;
    p.TerrainOrigin[2] = static_cast<float>(kEditorMaxDepth);
    const float yaw = yawDeg * 3.14159265f / 180.0f;
    const float pitch = pitchDeg * 3.14159265f / 180.0f;
    const Vector3 forward(std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch));
    const Matrix4x4 proj =
        MakePerspectiveLH_ZO_ReverseZ(kEditorFovVertical, kEditorScreenW / kEditorScreenH, 0.5f, 20000.0f);
    const Matrix4x4 vp = proj * MakeLookAtLH(eye, eye + forward, Vector3(0.0f, 1.0f, 0.0f));
    std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
    p.CameraPos[0] = eye.x;
    p.CameraPos[1] = eye.y;
    p.CameraPos[2] = eye.z;
    p.CameraPos[3] = 1.0f;
    const CBTNearBiasRadii nb = ComputeNearBiasRadii(eye.y - EditorReliefHeightAt(eye.x, eye.z));
    p.NearBias[0] = 1.0f;
    p.NearBias[1] = nb.NearRadius;
    p.NearBias[2] = nb.FarRadius;
    p.NearBias[3] = kNearBiasMaxCoarsen;
    p.DemandTuning[0] = kNearFieldFacetTargetM;
    p.DemandTuning[1] = kOffFrustumKeepOcc;
    p.DemandTuning[2] = kEdgeRescueTpeMul * kSplitPx;
    p.DemandTuning[3] = kEdgeRescueOcc;
    p.PriorityParams[0] = 1.0f;
    p.PriorityParams[1] = kPriorityRampStartOcc;
    p.PriorityParams[2] = kPriorityMaxAreaFloorPx2;
    p.PriorityParams[3] = kPriorityKeepLargeNdc;
}

// One camera placement over the editor terrain: metres above the ground at (X, Z), then a heading.
struct EditorPose
{
    const char* Label;
    float X, Z, Above, Yaw, Pitch;
    uint32_t Frames;
};
} // namespace

TEST_F(CBTWatertightness, WideViewDownwardPoseSettlesThePool)
{
    CreateRelief();
    if (HasFatalFailure())
        return;
    const CBTClassifyDesc classify = ProductionClassify(kEditorMaxDepth);
    constexpr float kX = 489.6f, kZ = 489.6f;
    constexpr uint32_t kWalkFrames = 40u;
    constexpr uint32_t kDownFrames = 120u;
    // The tail: the second half of the final pose, long after the pose change's own transient.
    constexpr size_t kTailFrames = 60u;
    // A settled pool moves by its in-view refinement only; the cycles measured moved 16-45% of the
    // pool per flip. Two percent of the pool is well above the settled frame-to-frame motion and far below
    // one flip.
    constexpr double kMaxSettledSwing = 0.02;
    struct Arm
    {
        const char* Label;
        std::vector<EditorPose> Poses;
    };
    const Arm arms[] = {
        {"walk-then-down-34",
         {{"walk-0", kX, kZ, 2.0f, 0.0f, -6.0f, kWalkFrames},
          {"walk-60", kX, kZ, 2.0f, 60.0f, -6.0f, kWalkFrames},
          {"walk-200", kX, kZ, 2.0f, 200.0f, -6.0f, kWalkFrames},
          {"walk-300", kX, kZ, 2.0f, 300.0f, -6.0f, kWalkFrames},
          {"down-34", kX, kZ, 34.0f, 0.0f, -80.0f, kDownFrames}}},
        {"nadir-10-89", {{"down-10", kX, kZ, 10.0f, 0.0f, -89.0f, kDownFrames}}},
        {"nadir-15-85", {{"down-15", kX, kZ, 15.0f, 0.0f, -85.0f, kDownFrames}}},
        {"nadir-20-85", {{"down-20", kX, kZ, 20.0f, 0.0f, -85.0f, kDownFrames}}},
    };
    for (const Arm& arm : arms)
    {
        CBTInstance inst;
        ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
        ASSERT_TRUE(inst.InitializeRoots(kDomainPlanar));
        BindRelief(inst);
        CBTFrameParams p{};
        uint32_t frame = 0;
        std::vector<double> occupancy;
        std::vector<int32_t> overflow;
        for (size_t poseIndex = 0; poseIndex < arm.Poses.size(); ++poseIndex)
        {
            const EditorPose& pose = arm.Poses[poseIndex];
            const bool last = poseIndex + 1u == arm.Poses.size();
            EditorParams(p, Vector3(pose.X, EditorReliefHeightAt(pose.X, pose.Z) + pose.Above, pose.Z), pose.Yaw,
                         pose.Pitch);
            for (uint32_t f = 0; f < pose.Frames; ++f, ++frame)
            {
                RunFrame(inst, classify, p, frame);
                if (!last)
                    continue;
                const CBTTessellationStats st = inst.ReadTessellationStats();
                occupancy.push_back(static_cast<double>(st.LiveCount) / st.PoolSize);
                overflow.push_back(st.OverflowTotal);
                std::printf("[gate-settle] %-18s %-8s frame %3u live %7u (%.4f) split %d/%d merge %d/%d overflow %d "
                            "keepStep %d\n",
                            arm.Label, pose.Label, f, st.LiveCount, occupancy.back(), st.SplitDemand, st.SplitServed,
                            st.MergeDemand, st.MergeServed, st.OverflowTotal, st.OffFrustumKeepStep);
            }
        }

        double worstSwing = 0.0;
        size_t worstAt = 0;
        for (size_t i = occupancy.size() - kTailFrames + 1u; i < occupancy.size(); ++i)
        {
            const double swing = std::fabs(occupancy[i] - occupancy[i - 1u]);
            if (swing > worstSwing)
            {
                worstSwing = swing;
                worstAt = i;
            }
        }
        const int32_t tailRollbacks = overflow.back() - overflow[overflow.size() - kTailFrames];
        std::printf("[gate-settle] %s tail %zu frames: worst frame-to-frame swing %.4f at frame %zu, rollbacks %d\n",
                    arm.Label, kTailFrames, worstSwing, worstAt, tailRollbacks);
        EXPECT_LE(worstSwing, kMaxSettledSwing)
            << arm.Label << ": the pool alternates between keeping and releasing the off-frustum field: occupancy "
            << (worstAt > 0u ? occupancy[worstAt - 1u] : 0.0) << " -> " << occupancy[worstAt] << " at frame " << worstAt;
        // What the cycle costs: every refill rolls back the split reservations the pool cannot hold. A
        // settled pool asks for no split it cannot serve.
        EXPECT_EQ(tailRollbacks, 0) << arm.Label << ": the settled pool still rolls back split reservations";

        const CBTTreeAuditResult a = AuditTree(ReadTreeSnapshot(inst, true));
        EXPECT_TRUE(a.Watertight()) << arm.Label << ": " << Describe(a);
        inst.Shutdown();
    }
}
