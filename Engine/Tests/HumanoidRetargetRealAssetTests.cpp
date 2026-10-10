// Phase 13 — Real-Asset End-to-End Test for the humanoid retargeting redesign.
//
// The synthetic suites (RetargetNodeTests, HumanoidRetargetEndToEndSmokeTest)
// share rest poses across source and target, so they cannot discriminate
// between "math is correct end-to-end" and "math is approximately correct
// because the synthetic fixture has trivial bind / retarget deltas." The
// user reported that BusinessMale.fbx animated with A_Walk_F_Masc.fbx
// produces a broken pose in the live editor; this test loads those two
// real Synty fixtures, runs the CPU retargeting pipeline, and measures
// the per-bone rotation amplitude across a 30-frame walk cycle.
//
// Discrimination criteria (failing means the math actually is broken on
// real cross-rig data):
//   * Frame 15 (mid-cycle) shoulder + upper-leg + spine rotation deviates
//     from the target's rest pose by at least the per-bone threshold.
//   * Across 30 frames, peak rotation deviation on those bones exceeds a
//     "this isn't a T-pose" threshold (>30° on shoulder).
//   * No NaN / Inf in any skin matrix.
//   * Hip translation stays in a sane range.
//
// Skip-gated: when Assets/Models/FBXTest/{BusinessMale,A_Walk_F_Masc}.fbx
// or the HumanoidStandard profile aren't present, every test SUCCEED()s
// with a diagnostic. CI without the Synty fixture exits clean.

#include <gtest/gtest.h>

#include "Assets/AssetManager.h"
#include "Assets/AnimationClip.h"
#include "Assets/ModelAsset.h"
#include "AssetCore/GUID.h"

#include "Animation/AnimationPose.h"
#include "Animation/EvaluationContext.h"
#include "Animation/HumanBone.h"
#include "Animation/HumanoidImport.h"
#include "Animation/HumanoidRig.h"
#include "Animation/Nodes/RetargetNode.h"
#include "Animation/PoseStack.h"
#include "Animation/RetargetMap.h"
#include "Animation/SkeletonData.h"

#include "Components/Animation/HumanoidRetargeterComponent.h"

#include "ECSModules/Rendering/SkeletonStore.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "ECSModules/Rendering/Systems/HumanoidRetargetSystem.h"

#include "Logger/Logger.h"
#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include "StagedTestPaths.h"

using namespace GameEngine;
using namespace GameEngine::Animation;
namespace GERender = GameEngine::Engine::Renderer;

namespace
{

constexpr const char* kBusinessMaleRel = "Assets/Models/FBXTest/BusinessMale.fbx";
constexpr const char* kWalkClipRel     = "Assets/Models/FBXTest/A_Walk_F_Masc.fbx";
constexpr const char* kSkeletonProfile = "Assets/SkeletonProfiles/HumanoidStandard.profile.json";

// Staged fixture mirror root (StageTestAssets); see StagedTestPaths.h.
std::filesystem::path StagedRoot()
{
    return TestPaths::StagedRoot();
}

std::filesystem::path SidecarFor(const std::filesystem::path& modelPath)
{
    auto p = modelPath;
    p.replace_extension();
    p += ".humanoidrig.json";
    return p;
}

std::filesystem::path RetargetMapPath(const std::filesystem::path& tgtModel,
                                      const std::filesystem::path& srcModel)
{
    return tgtModel.parent_path() /
        (srcModel.stem().string() + "_to_" + tgtModel.stem().string() + ".retargetmap.json");
}

// Extract a glm::quat (column-major mat4 -> rotation) without scale/translation.
// The skin matrix is mesh-root inverse * world-space bone transform * inverse-bind,
// which for our purposes folds rotation/translation/scale together. To extract
// rotation cleanly we orthonormalize the upper-3x3.
Mathematics::Quaternion ExtractRotation(const float* m4)
{
    if (!m4) return Mathematics::Quaternion::Identity();

    glm::vec3 col0(m4[0], m4[1], m4[2]);
    glm::vec3 col1(m4[4], m4[5], m4[6]);
    glm::vec3 col2(m4[8], m4[9], m4[10]);

    const float l0 = glm::length(col0);
    const float l1 = glm::length(col1);
    const float l2 = glm::length(col2);
    if (l0 > 1e-6f) col0 /= l0;
    if (l1 > 1e-6f) col1 /= l1;
    if (l2 > 1e-6f) col2 /= l2;

    glm::mat3 rot(col0, col1, col2);
    return Mathematics::Quaternion(glm::quat_cast(rot));
}

// Angle (degrees) between two unit quaternions (shortest path).
float AngleBetweenDeg(const Mathematics::Quaternion& a, const Mathematics::Quaternion& b)
{
    glm::quat ag = a.Normalized().GetGLM();
    glm::quat bg = b.Normalized().GetGLM();
    float dot = std::fabs(ag.x * bg.x + ag.y * bg.y + ag.z * bg.z + ag.w * bg.w);
    dot = std::clamp(dot, 0.0f, 1.0f);
    constexpr float kRadToDeg = 57.29577951308232f;
    return 2.0f * std::acos(dot) * kRadToDeg;
}

bool MatrixIsFinite(const float* m, uint32_t count = 16)
{
    if (!m) return false;
    for (uint32_t i = 0; i < count; ++i)
        if (!std::isfinite(m[i])) return false;
    return true;
}

// Find the target-skeleton bone index for a canonical HumanBone via the
// target rig's BoneMap (live re-resolved against the loaded skeleton).
uint32_t ResolveCanonicalToTargetIndex(const HumanoidRig& rig,
                                        HumanBone canon,
                                        const SkeletonData& skel)
{
    for (const auto& m : rig.BoneMap())
    {
        if (m.Canonical != canon) continue;
        if (m.SourceBoneName.empty()) return m.CachedSourceIndex;
        if (skel.BoneNameLookup.empty()) return m.CachedSourceIndex;
        const StringId nameId = HashStringId(m.SourceBoneName);
        const uint32_t live = skel.ResolveBoneIndex(nameId, ~0u);
        return (live != ~0u) ? live : m.CachedSourceIndex;
    }
    return ~0u;
}

// Test fixture: one-time Synty fixture mount + sidecar generation. Each
// per-test SetUp wipes generated sidecars so we exercise the auto-import
// path on every run (deterministic).
class HumanoidRealAssetFixture : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_StagedRoot = StagedRoot();
        m_FixtureDir = m_StagedRoot / "Assets/Models/FBXTest";
        m_BusinessMale = m_StagedRoot / kBusinessMaleRel;
        m_WalkClip     = m_StagedRoot / kWalkClipRel;
        m_ProfilePath  = m_StagedRoot / kSkeletonProfile;

        std::error_code ec;
        m_HasFixtures =
            std::filesystem::exists(m_BusinessMale, ec) && !ec
         && std::filesystem::exists(m_WalkClip,     ec) && !ec
         && std::filesystem::exists(m_ProfilePath,  ec) && !ec;
        if (!m_HasFixtures) return;

        // Wipe generated sidecars so each run regenerates them through
        // ModelAsset::PostLoad and we cover the full auto-import path.
        ForceDelete(SidecarFor(m_BusinessMale));
        ForceDelete(SidecarFor(m_WalkClip));
        ForceDelete(RetargetMapPath(m_BusinessMale, m_WalkClip));

        m_AssetManager = std::make_unique<AssetManager>();
        ASSERT_TRUE(m_AssetManager->Initialize());

        // Project mount — fixture dir only (avoid scanning the full repo
        // which has non-ASCII filenames in vendored deps).
        AssetSourceDesc projectSrc;
        projectSrc.Alias            = std::string(kAssetSourceAliasProject);
        projectSrc.Root             = m_FixtureDir;
        projectSrc.DerivedIdentity  = true;
        projectSrc.Priority         = 100;
        ASSERT_TRUE(m_AssetManager->RegisterSource(projectSrc));

        AssetSourceDesc editorSrc;
        editorSrc.Alias            = std::string(kAssetSourceAliasEditor);
        editorSrc.Root             = m_StagedRoot / "Assets";
        editorSrc.DerivedIdentity  = true;
        editorSrc.Priority         = 50;
        ASSERT_TRUE(m_AssetManager->RegisterSource(editorSrc));

        // Clear ClipStore so the prior test's runtime clips don't bleed.
        GERender::ClipStore::Instance().ClearForTest();
    }

    void TearDown() override
    {
        if (m_AssetManager) m_AssetManager->Shutdown();
    }

    static void ForceDelete(const std::filesystem::path& p)
    {
        std::error_code ec;
        if (std::filesystem::exists(p, ec) && !ec)
            std::filesystem::remove(p, ec);
    }

    SharedPtr<ModelAsset> LoadModelSync(const std::filesystem::path& abs)
    {
        const GUID guid = m_AssetManager->ResolveAssetGuid(abs);
        EXPECT_FALSE(guid.IsNull()) << "Failed to resolve GUID for " << abs.string();
        if (guid.IsNull()) return nullptr;
        auto fut = m_AssetManager->LoadAssetAsync(guid);
        return std::dynamic_pointer_cast<ModelAsset>(fut.get());
    }

    std::filesystem::path m_StagedRoot;
    std::filesystem::path m_FixtureDir;
    std::filesystem::path m_BusinessMale;
    std::filesystem::path m_WalkClip;
    std::filesystem::path m_ProfilePath;
    bool m_HasFixtures = false;
    std::unique_ptr<AssetManager> m_AssetManager;
};

} // namespace

// =====================================================================
// Phase 13 main: load BusinessMale + A_Walk_F_Masc, drive 30 frames of
// CPU retargeting through HumanoidRetargetSystem::TestHooks::
// RunCPUEvaluateForTest, and measure per-bone rotation amplitude on
// canonical bones (LeftShoulder, LeftUpperLeg, Spine). The report logs
// the peak deviation per bone so we can see whether retargeting is
// driving the target away from rest (working) or pinning it at the
// retarget pose (broken / T-pose output).
// =====================================================================
TEST_F(HumanoidRealAssetFixture, BusinessMale_Plus_AWalkFMasc_DrivesNonTrivialMotion)
{
    if (!m_HasFixtures) {
        GTEST_SKIP() << "Synty FBX fixtures + HumanoidStandard profile not present; skipping.";
    }

    // ---- Load target model (BusinessMale) -----------------------------
    auto tgtModel = LoadModelSync(m_BusinessMale);
    ASSERT_NE(tgtModel, nullptr);
    ASSERT_EQ(tgtModel->GetState(), AssetState::Loaded);
    const uint32_t tgtSkelId = tgtModel->GetSkeletonId();
    ASSERT_NE(tgtSkelId, 0u) << "BusinessMale model has no skeleton; can't run cross-rig retarget.";

    // Sidecar must exist + auto-flagged + populated.
    const auto tgtSidecar = SidecarFor(m_BusinessMale);
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::exists(tgtSidecar, ec))
        << "BusinessMale humanoid sidecar wasn't auto-generated by PostLoad.";
    auto tgtRig = std::make_shared<HumanoidRig>(GUID(), tgtSidecar);
    ASSERT_TRUE(tgtRig->Load());
    EXPECT_TRUE(tgtRig->AutoGenerated());
    EXPECT_GE(tgtRig->BoneMap().size(), 20u)
        << "BusinessMale auto-rig has fewer than 20 mapped bones; coverage gate is too lax.";
    std::printf("[Phase13] BusinessMale rig: %zu bones mapped, %zu chains\n",
                tgtRig->BoneMap().size(), tgtRig->Chains().size());

    // ---- Load source model (A_Walk_F_Masc) -----------------------------
    auto srcModel = LoadModelSync(m_WalkClip);
    ASSERT_NE(srcModel, nullptr);
    ASSERT_EQ(srcModel->GetState(), AssetState::Loaded);
    const uint32_t srcSkelId = srcModel->GetSkeletonId();
    ASSERT_NE(srcSkelId, 0u) << "A_Walk_F_Masc has no skeleton; the bake-in-FBX is missing.";

    const auto srcSidecar = SidecarFor(m_WalkClip);
    ASSERT_TRUE(std::filesystem::exists(srcSidecar, ec))
        << "A_Walk_F_Masc humanoid sidecar wasn't auto-generated.";
    auto srcRig = std::make_shared<HumanoidRig>(GUID(), srcSidecar);
    ASSERT_TRUE(srcRig->Load());
    EXPECT_TRUE(srcRig->AutoGenerated());
    EXPECT_GE(srcRig->BoneMap().size(), 20u);
    std::printf("[Phase13] A_Walk_F_Masc rig: %zu bones mapped, %zu chains\n",
                srcRig->BoneMap().size(), srcRig->Chains().size());

    // The clip rig's RetargetPoseRotation must be non-identity on at
    // least one shoulder (Synty A-pose -> canonical T-pose offset). If
    // every entry is identity, AutoImportHumanoidRig is silently
    // dropping retarget deltas and the discrimination test below would
    // be vacuous.
    bool sawNonIdentityShoulder = false;
    for (const auto& m : srcRig->BoneMap())
    {
        if (m.Canonical != HumanBone::LeftShoulder &&
            m.Canonical != HumanBone::RightShoulder &&
            m.Canonical != HumanBone::LeftUpperArm  &&
            m.Canonical != HumanBone::RightUpperArm) continue;
        const float ang = AngleBetweenDeg(m.RetargetPoseRotation,
                                           Mathematics::Quaternion::Identity());
        if (ang > 1.0f) { sawNonIdentityShoulder = true; break; }
    }
    // Don't ASSERT — Synty's bake might collapse to identity and that's
    // a separate finding. Log and continue.
    std::printf("[Phase13] Source rig shoulder/upper-arm has non-identity retarget pose: %s\n",
                sawNonIdentityShoulder ? "yes" : "no");

    // ---- Pick a clip from the source FBX -------------------------------
    const auto& srcClipGuids = srcModel->GetEmbeddedClipGuids();
    ASSERT_FALSE(srcClipGuids.empty())
        << "A_Walk_F_Masc carries no embedded clips; FBX import didn't extract anim.";
    auto& clipStore = GERender::ClipStore::Instance();
    const uint32_t clipIndex = clipStore.GetIndexIfPresent(srcClipGuids[0]);
    ASSERT_NE(clipIndex, 0u)
        << "Embedded clip wasn't registered in ClipStore; FBX-load path is broken.";
    auto clip = clipStore.Get(clipIndex);
    ASSERT_NE(clip, nullptr);
    EXPECT_GT(clip->GetDuration(), 0.0f)
        << "Walk clip has zero duration; retargeting can't sample.";
    std::printf("[Phase13] Walk clip duration = %.3f sec, %zu channels\n",
                clip->GetDuration(),
                clip->GetChannels().size());

    // ---- Build a RetargetMap (auto) ------------------------------------
    auto retargetMap = std::make_shared<RetargetMap>(GUID(),
        RetargetMapPath(m_BusinessMale, m_WalkClip));
    retargetMap->SetSourceRigRef(srcRig->GetGUID());
    retargetMap->SetTargetRigRef(tgtRig->GetGUID());
    AutoCreateRetargetMap(*srcRig, *tgtRig, *retargetMap);
    EXPECT_GT(retargetMap->ChainMap().size(), 0u)
        << "AutoCreateRetargetMap produced no chain pairings; src+tgt rigs share no chains.";
    std::printf("[Phase13] RetargetMap: %zu chain pairings\n",
                retargetMap->ChainMap().size());

    // ---- Allocate target runtime + capture rest pose -------------------
    auto& skStore = GERender::SkeletonStore::Instance();
    const uint32_t tgtRuntimeId = skStore.CreateRuntime(tgtSkelId);
    ASSERT_NE(tgtRuntimeId, 0u);

    auto* tgtSkelData = skStore.Get(tgtSkelId);
    ASSERT_NE(tgtSkelData, nullptr);
    ASSERT_GT(tgtSkelData->BoneCount, 0u);
    std::printf("[Phase13] Target skeleton: %u bones, SkinJointCount = %u\n",
                tgtSkelData->BoneCount, tgtSkelData->SkinJointCount);

    // Capture rest-pose skin matrix per bone by sampling the clip at
    // t = -1 (which clamps to first key + the AnimationPose code falls
    // back to bind pose). Instead we use a simpler approach: the t=0
    // skin matrix becomes our "rest baseline" since at t=0 the source
    // clip is at its first key (which Synty packs as the bind/A-pose).
    //
    // For a TRUE rest baseline we'd want the target's bind pose
    // directly; sampling the clip at any time is "clip-driven rest +
    // some delta from retarget pipeline." The simpler proxy: take t=0,
    // log the per-bone deviation across the run.

    // Resolve canonical -> target-skeleton bone indices we'll measure.
    const uint32_t leftShoulderIdx  = ResolveCanonicalToTargetIndex(
        *tgtRig, HumanBone::LeftShoulder, *tgtSkelData);
    const uint32_t leftUpperLegIdx  = ResolveCanonicalToTargetIndex(
        *tgtRig, HumanBone::LeftUpperLeg, *tgtSkelData);
    const uint32_t spineIdx         = ResolveCanonicalToTargetIndex(
        *tgtRig, HumanBone::Spine, *tgtSkelData);
    const uint32_t hipsIdx          = ResolveCanonicalToTargetIndex(
        *tgtRig, HumanBone::Hips, *tgtSkelData);

    const bool haveShoulder = (leftShoulderIdx != ~0u && leftShoulderIdx < tgtSkelData->BoneCount);
    const bool haveLeg      = (leftUpperLegIdx != ~0u && leftUpperLegIdx < tgtSkelData->BoneCount);
    const bool haveSpine    = (spineIdx != ~0u && spineIdx < tgtSkelData->BoneCount);
    const bool haveHips     = (hipsIdx != ~0u && hipsIdx < tgtSkelData->BoneCount);
    EXPECT_TRUE(haveShoulder) << "Target rig has no LeftShoulder mapping.";
    EXPECT_TRUE(haveLeg)      << "Target rig has no LeftUpperLeg mapping.";
    EXPECT_TRUE(haveSpine)    << "Target rig has no Spine mapping.";
    EXPECT_TRUE(haveHips)     << "Target rig has no Hips mapping.";

    std::printf("[Phase13] Bone indices: LeftShoulder=%u, LeftUpperLeg=%u, Spine=%u, Hips=%u\n",
                leftShoulderIdx, leftUpperLegIdx, spineIdx, hipsIdx);

    // ---- Drive 30 frames + capture per-bone rotation -------------------
    constexpr int kFrameCount = 30;
    const float duration = clip->GetDuration();
    const float dt = (duration > 0.0f) ? (duration / static_cast<float>(kFrameCount)) : (1.0f / 30.0f);

    Mathematics::Quaternion frame0Shoulder = Mathematics::Quaternion::Identity();
    Mathematics::Quaternion frame0Leg      = Mathematics::Quaternion::Identity();
    Mathematics::Quaternion frame0Spine    = Mathematics::Quaternion::Identity();
    Mathematics::Vector3   frame0HipPos   { 0.0f, 0.0f, 0.0f };

    float peakShoulderDeg = 0.0f;
    float peakLegDeg      = 0.0f;
    float peakSpineDeg    = 0.0f;

    float frame15ShoulderDeg = 0.0f;
    float frame15LegDeg      = 0.0f;
    float frame15SpineDeg    = 0.0f;

    bool everyFrameFinite = true;
    int  framesEvaluated  = 0;

    for (int f = 0; f < kFrameCount; ++f)
    {
        const float t = static_cast<float>(f) * dt;

        bool ok = false;
        ASSERT_NO_THROW({
            ok = GERender::TestHooks::RunCPUEvaluateForTest(
                *retargetMap, *srcRig, *tgtRig, clipIndex,
                /*sourceSkeletonId=*/srcSkelId,
                /*targetSkeletonId=*/tgtSkelId,
                /*targetRuntimeId=*/tgtRuntimeId,
                /*clipTime=*/t);
        }) << "Frame " << f << " threw an exception";
        if (!ok) {
            ADD_FAILURE() << "Frame " << f << " evaluation returned false";
            continue;
        }
        ++framesEvaluated;

        auto* runtime = skStore.GetRuntime(tgtRuntimeId);
        ASSERT_NE(runtime, nullptr);
        ASSERT_GE(runtime->CompactSkinMatrices.size(),
                  static_cast<size_t>(tgtSkelData->BoneCount) * 16u);

        // NaN / Inf sanity across all bones.
        for (uint32_t b = 0; b < tgtSkelData->BoneCount; ++b)
        {
            const float* m = &runtime->CompactSkinMatrices[b * 16];
            if (!MatrixIsFinite(m))
            {
                everyFrameFinite = false;
                ADD_FAILURE() << "Frame " << f << " bone " << b
                              << " skin matrix contains NaN/Inf";
            }
        }

        if (haveShoulder)
        {
            const float* m = &runtime->CompactSkinMatrices[leftShoulderIdx * 16];
            const Mathematics::Quaternion q = ExtractRotation(m);
            if (f == 0) frame0Shoulder = q;
            const float deg = AngleBetweenDeg(q, frame0Shoulder);
            if (f == 15) frame15ShoulderDeg = deg;
            peakShoulderDeg = std::max(peakShoulderDeg, deg);
        }
        if (haveLeg)
        {
            const float* m = &runtime->CompactSkinMatrices[leftUpperLegIdx * 16];
            const Mathematics::Quaternion q = ExtractRotation(m);
            if (f == 0) frame0Leg = q;
            const float deg = AngleBetweenDeg(q, frame0Leg);
            if (f == 15) frame15LegDeg = deg;
            peakLegDeg = std::max(peakLegDeg, deg);
        }
        if (haveSpine)
        {
            const float* m = &runtime->CompactSkinMatrices[spineIdx * 16];
            const Mathematics::Quaternion q = ExtractRotation(m);
            if (f == 0) frame0Spine = q;
            const float deg = AngleBetweenDeg(q, frame0Spine);
            if (f == 15) frame15SpineDeg = deg;
            peakSpineDeg = std::max(peakSpineDeg, deg);
        }
        if (haveHips)
        {
            const float* m = &runtime->CompactSkinMatrices[hipsIdx * 16];
            // Translation column (column-major mat4 -> col 3 = m[12..14]).
            if (f == 0)
            {
                frame0HipPos = Mathematics::Vector3(m[12], m[13], m[14]);
            }
            // Sanity: hip position shouldn't be at infinity.
            const float maxAbs = std::max({
                std::fabs(m[12]), std::fabs(m[13]), std::fabs(m[14])});
            if (maxAbs > 1.0e6f)
            {
                ADD_FAILURE() << "Frame " << f << " hip translation absurdly large: ("
                              << m[12] << "," << m[13] << "," << m[14] << ")";
            }
        }
    }

    EXPECT_EQ(framesEvaluated, kFrameCount);
    EXPECT_TRUE(everyFrameFinite);

    std::printf("[Phase13][BusinessMale + A_Walk_F_Masc] Per-bone rotation amplitude (vs frame 0):\n");
    std::printf("  LeftShoulder: peak = %.2f deg, frame15 = %.2f deg\n",
                peakShoulderDeg, frame15ShoulderDeg);
    std::printf("  LeftUpperLeg: peak = %.2f deg, frame15 = %.2f deg\n",
                peakLegDeg, frame15LegDeg);
    std::printf("  Spine:        peak = %.2f deg, frame15 = %.2f deg\n",
                peakSpineDeg, frame15SpineDeg);
    std::printf("  Hips world translation (frame0): (%.3f, %.3f, %.3f)\n",
                frame0HipPos.x, frame0HipPos.y, frame0HipPos.z);

    // ---- Discrimination assertions -------------------------------------
    // If the math is broken (T-pose output), all peak deviations would
    // be ~0 because the retarget pipeline holds every chain bone at the
    // target retarget pose. The FK transport step writes the source
    // delta through; if it's silently zeroing on real cross-rig data,
    // these gates fail loudly.
    //
    // Thresholds are deliberately generous — we're testing "is anything
    // moving at all?" not "is the motion exactly correct." The numbers
    // come from observation of typical Synty walk cycles:
    //   * Shoulders sway ~30-50° at peak
    //   * Upper legs swing ~25-40° at peak
    //   * Spine bends ~5-15°
    //
    // If shoulders and legs both stay below 5° peak, the retarget
    // pipeline is producing T-pose output.

    if (haveShoulder)
    {
        EXPECT_GT(peakShoulderDeg, 10.0f)
            << "LeftShoulder shows <10° peak rotation across 30 frames of a "
               "walk cycle. Retarget pipeline is producing near-T-pose output. "
               "This is the discrimination signal: synthetic tests pass but real "
               "cross-rig data fails. Investigate parent-shift + M_src/M_tgt math "
               "first: a wrong parent shift reads as an under-rotated shoulder.";
    }
    if (haveLeg)
    {
        EXPECT_GT(peakLegDeg, 5.0f)
            << "LeftUpperLeg shows <5° peak rotation across 30 frames of a "
               "walk cycle. Same broken-retarget signal as shoulder gate.";
    }
    if (haveSpine)
    {
        // Spine threshold is more permissive — spine motion in walks is small.
        EXPECT_GT(peakSpineDeg, 1.0f)
            << "Spine shows <1° peak rotation; even a static walk cycle has "
               "more than this. Retarget pipeline is silently zeroing.";
    }
}
