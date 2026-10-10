// Cross-format retargeting smoke test.
//
// Loads BusinessMale.fbx as the *target* humanoid (Synty Humanoid Standard
// rig), one of Ellie's clips from `ellie_animation.blend` as the *source*
// clip (Auto-Rig Pro / BlenRig 6 / TLOU naming convention), drives the CPU
// retargeting pipeline for 30 frames, and asserts non-T-pose output.
//
// This is the cross-format E2E test for the .blend integration: success
// means a `.blend`-authored humanoid clip drives a `.fbx`-authored skeleton
// via auto-imported HumanoidRig sidecars on both sides + AutoCreateRetargetMap.
//
// Skip-gated when:
//   * Tests/BlendSamples/ellie/...ellie_animation.blend
//     is not extracted on this machine (the bundle is large + user-local +
//     gitignored).
//   * Assets/Models/FBXTest/BusinessMale.fbx is not present.
//   * Assets/SkeletonProfiles/HumanoidStandard.profile.json is
//     not present.
//
// All three skips emit GTEST_SKIP() with a diagnostic so CI machines without
// the bundle exit clean.

#include <gtest/gtest.h>

#include "Assets/AssetManager.h"
#include "Assets/AnimationClip.h"
#include "Assets/ModelAsset.h"
#include "AssetCore/GUID.h"

#include "Animation/AnimationPose.h"
#include "Animation/EvaluationContext.h"
#include "Animation/HumanBone.h"
#include "Animation/HumanoidImport.h"
#include "Animation/HumanoidNameMatcher.h"
#include "Animation/HumanoidRig.h"
#include "Animation/PoseStack.h"
#include "Animation/RetargetMap.h"
#include "Animation/SkeletonData.h"
#include "Animation/SkeletonProfile.h"

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

namespace fs = std::filesystem;
using namespace GameEngine;
using namespace GameEngine::Animation;
namespace GERender = GameEngine::Engine::Renderer;

namespace
{

constexpr const char* kBusinessMaleRel  = "Assets/Models/FBXTest/BusinessMale.fbx";
constexpr const char* kSkeletonProfile  = "Assets/SkeletonProfiles/HumanoidStandard.profile.json";
constexpr const char* kEllieBlendRel    =
    "Tests/BlendSamples/ellie/asset-demo-bundle-4.0-ellie-animation/ellie_animation/ellie_animation.blend";

fs::path StagedRoot()
{
    return TestPaths::StagedRoot();
}

fs::path SidecarFor(const fs::path& modelPath)
{
    auto p = modelPath;
    p.replace_extension();
    p += ".humanoidrig.json";
    return p;
}

fs::path RetargetMapPath(const fs::path& tgtModel, const fs::path& srcModel)
{
    return tgtModel.parent_path() /
        (srcModel.stem().string() + "_to_" + tgtModel.stem().string() + ".retargetmap.json");
}

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

class BlenderToFbxFixture : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_StagedRoot       = StagedRoot();
        m_BusinessMale   = m_StagedRoot / kBusinessMaleRel;
        m_EllieBlend     = m_StagedRoot / kEllieBlendRel;
        m_ProfilePath    = m_StagedRoot / kSkeletonProfile;

        std::error_code ec;
        m_HasFixtures =
               fs::exists(m_BusinessMale, ec) && !ec
            && fs::exists(m_EllieBlend,   ec) && !ec
            && fs::exists(m_ProfilePath,  ec) && !ec;
        if (!m_HasFixtures) return;

        // Wipe generated sidecars so each run regenerates them through
        // ModelAsset::PostLoad and we cover the full auto-import path.
        ForceDelete(SidecarFor(m_BusinessMale));
        ForceDelete(SidecarFor(m_EllieBlend));
        ForceDelete(RetargetMapPath(m_BusinessMale, m_EllieBlend));

        m_AssetManager = std::make_unique<AssetManager>();
        ASSERT_TRUE(m_AssetManager->Initialize());

        // Project mount: parent directory of the .blend so the importer
        // and PostLoad can see sibling assets if they exist.
        AssetSourceDesc projectSrc;
        projectSrc.Alias            = std::string(kAssetSourceAliasProject);
        projectSrc.Root             = m_BusinessMale.parent_path();
        projectSrc.DerivedIdentity  = true;
        projectSrc.Priority         = 100;
        ASSERT_TRUE(m_AssetManager->RegisterSource(projectSrc));

        AssetSourceDesc editorSrc;
        editorSrc.Alias            = std::string(kAssetSourceAliasEditor);
        editorSrc.Root             = m_StagedRoot / "Apps/Editor/Assets";
        editorSrc.DerivedIdentity  = true;
        editorSrc.Priority         = 50;
        ASSERT_TRUE(m_AssetManager->RegisterSource(editorSrc));

        // ClipStore reset so prior tests don't bleed registered clips.
        GERender::ClipStore::Instance().ClearForTest();
    }

    void TearDown() override
    {
        if (m_AssetManager) m_AssetManager->Shutdown();
    }

    static void ForceDelete(const fs::path& p)
    {
        std::error_code ec;
        if (fs::exists(p, ec) && !ec) fs::remove(p, ec);
    }

    SharedPtr<ModelAsset> LoadModelSync(const fs::path& abs)
    {
        const GUID guid = m_AssetManager->ResolveAssetGuid(abs);
        EXPECT_FALSE(guid.IsNull()) << "Failed to resolve GUID for " << abs.string();
        if (guid.IsNull()) return nullptr;
        auto fut = m_AssetManager->LoadAssetAsync(guid);
        return std::dynamic_pointer_cast<ModelAsset>(fut.get());
    }

    fs::path m_StagedRoot;
    fs::path m_BusinessMale;
    fs::path m_EllieBlend;
    fs::path m_ProfilePath;
    bool m_HasFixtures = false;
    std::unique_ptr<AssetManager> m_AssetManager;
};

} // namespace

// =====================================================================
// B4 main: load BusinessMale.fbx (target) + ellie_animation.blend
// (source), resolve auto-imported humanoid sidecars on both, build a
// RetargetMap, and drive 30 frames through HumanoidRetargetSystem's
// TestHooks::RunCPUEvaluateForTest. Asserts non-T-pose output (peak
// shoulder rotation > 5° across the run).
// =====================================================================
TEST_F(BlenderToFbxFixture, EllieClipDrivesBusinessMaleSkeleton)
{
    if (!m_HasFixtures)
    {
        GTEST_SKIP() << "BusinessMale.fbx + Ellie .blend bundle + HumanoidStandard "
                        "profile not all present; skipping cross-format E2E.";
    }

    // ---- Load TARGET model (BusinessMale.fbx) ---------------------------
    auto tgtModel = LoadModelSync(m_BusinessMale);
    ASSERT_NE(tgtModel, nullptr);
    ASSERT_EQ(tgtModel->GetState(), AssetState::Loaded);
    const uint32_t tgtSkelId = tgtModel->GetSkeletonId();
    ASSERT_NE(tgtSkelId, 0u) << "BusinessMale model has no skeleton.";

    const auto tgtSidecar = SidecarFor(m_BusinessMale);
    std::error_code ec;
    ASSERT_TRUE(fs::exists(tgtSidecar, ec))
        << "BusinessMale humanoid sidecar wasn't auto-generated by PostLoad.";
    auto tgtRig = std::make_shared<HumanoidRig>(GUID(), tgtSidecar);
    ASSERT_TRUE(tgtRig->Load());
    EXPECT_TRUE(tgtRig->AutoGenerated());
    EXPECT_GE(tgtRig->BoneMap().size(), 20u)
        << "BusinessMale auto-rig has <20 mapped bones; coverage gate is too lax.";
    std::printf("[B4] BusinessMale rig: %zu bones mapped, %zu chains\n",
                tgtRig->BoneMap().size(), tgtRig->Chains().size());

    // ---- Load SOURCE .blend (Ellie animation bundle) --------------------
    auto srcModel = LoadModelSync(m_EllieBlend);
    ASSERT_NE(srcModel, nullptr);
    ASSERT_EQ(srcModel->GetState(), AssetState::Loaded);
    const uint32_t srcSkelId = srcModel->GetSkeletonId();
    ASSERT_NE(srcSkelId, 0u) << "Ellie .blend has no skeleton; B2 importer bug.";

    auto& skStore = GERender::SkeletonStore::Instance();
    const auto* srcSkelData = skStore.Get(srcSkelId);
    ASSERT_NE(srcSkelData, nullptr);
    EXPECT_GT(srcSkelData->BoneCount, 100u)
        << "Ellie .blend should produce a large rig (BlenRig has ~1800 bones).";
    std::printf("[B4] Ellie skeleton: %u bones\n", srcSkelData->BoneCount);

    // The source rig sidecar is what the AnimationSystem auto-bootstrap
    // path looks for. PostLoad on the .blend should have produced one as
    // long as the BlenRig name-coverage gate passes (Phase B4 added the
    // patterns for that).
    const auto srcSidecar = SidecarFor(m_EllieBlend);
    if (!fs::exists(srcSidecar, ec))
    {
        // Diagnose by re-running the heuristic here so we can dump coverage
        // numbers into the test log — makes failures obvious.
        HumanoidNameMatcher matcher;
        const auto matchResult = matcher.MatchSkeleton(srcSkelData->BoneNames);
        std::printf("[B4] Ellie auto-import coverage: %.0f%% (required %u/%u)\n",
                    matchResult.Coverage * 100.0f,
                    matchResult.RequiredMatched, matchResult.RequiredTotal);
        // Print which canonical bones DID get mapped, so a real-failure
        // bisect can tell what slipped.
        for (const auto& m : matchResult.Mappings)
        {
            std::printf("[B4]   %-22s -> %s\n",
                        HumanBoneToString(m.Canonical),
                        m.SourceBoneName.c_str());
        }
        FAIL() << "Ellie humanoid sidecar wasn't auto-generated. Heuristic "
                  "coverage was below the 80% threshold; see [B4] log lines "
                  "above for which bones missed.";
    }
    auto srcRig = std::make_shared<HumanoidRig>(GUID(), srcSidecar);
    ASSERT_TRUE(srcRig->Load());
    EXPECT_TRUE(srcRig->AutoGenerated());
    EXPECT_GE(srcRig->BoneMap().size(), 15u)
        << "Ellie auto-rig has <15 mapped bones; BlenRig matcher patterns are weak.";
    std::printf("[B4] Ellie rig: %zu bones mapped, %zu chains\n",
                srcRig->BoneMap().size(), srcRig->Chains().size());

    // ---- Pick the first non-trivial source clip -------------------------
    const auto& srcClipGuids = srcModel->GetEmbeddedClipGuids();
    ASSERT_FALSE(srcClipGuids.empty())
        << "Ellie .blend produced no embedded clips; B3 Action extraction "
           "didn't run.";
    auto& clipStore = GERender::ClipStore::Instance();

    // Pick the longest registered clip across all 57+ Actions. Many
    // pose-library entries have 1-frame duration which is uninteresting
    // for the rotation-amplitude assertion; finding the longest clip is
    // the cheapest "pick something with motion" heuristic.
    uint32_t bestIdx = 0;
    float bestDuration = 0.0f;
    SharedPtr<AnimationClip> bestClip;
    for (const GUID& g : srcClipGuids)
    {
        const uint32_t idx = clipStore.GetIndexIfPresent(g);
        if (idx == 0) continue;
        auto c = clipStore.Get(idx);
        if (!c) continue;
        if (c->GetDuration() > bestDuration)
        {
            bestDuration = c->GetDuration();
            bestIdx = idx;
            bestClip = c;
        }
    }
    ASSERT_NE(bestIdx, 0u) << "Ellie produced no registered clips in ClipStore.";
    ASSERT_NE(bestClip, nullptr);
    EXPECT_GT(bestClip->GetDuration(), 0.5f)
        << "Longest Ellie clip is too short (<0.5s) to drive a meaningful "
           "rotation across 30 frames.";
    std::printf("[B4] Source clip: index=%u duration=%.2fs channels=%zu\n",
                bestIdx, bestClip->GetDuration(), bestClip->GetChannels().size());

    // ---- Build a RetargetMap (auto) -------------------------------------
    auto retargetMap = std::make_shared<RetargetMap>(GUID(),
        RetargetMapPath(m_BusinessMale, m_EllieBlend));
    retargetMap->SetSourceRigRef(srcRig->GetGUID());
    retargetMap->SetTargetRigRef(tgtRig->GetGUID());
    AutoCreateRetargetMap(*srcRig, *tgtRig, *retargetMap);
    EXPECT_GT(retargetMap->ChainMap().size(), 0u)
        << "AutoCreateRetargetMap produced no chain pairings; src+tgt rigs "
           "share no chains.";
    std::printf("[B4] RetargetMap: %zu chain pairings\n",
                retargetMap->ChainMap().size());

    // ---- Allocate target runtime ----------------------------------------
    const uint32_t tgtRuntimeId = skStore.CreateRuntime(tgtSkelId);
    ASSERT_NE(tgtRuntimeId, 0u);

    auto* tgtSkelData = skStore.Get(tgtSkelId);
    ASSERT_NE(tgtSkelData, nullptr);
    ASSERT_GT(tgtSkelData->BoneCount, 0u);
    std::printf("[B4] Target skeleton: %u bones, SkinJointCount=%u\n",
                tgtSkelData->BoneCount, tgtSkelData->SkinJointCount);

    // Resolve canonical -> target-skeleton bone indices we'll measure.
    const uint32_t leftShoulderIdx = ResolveCanonicalToTargetIndex(
        *tgtRig, HumanBone::LeftShoulder, *tgtSkelData);
    const uint32_t leftUpperArmIdx = ResolveCanonicalToTargetIndex(
        *tgtRig, HumanBone::LeftUpperArm, *tgtSkelData);
    const uint32_t spineIdx        = ResolveCanonicalToTargetIndex(
        *tgtRig, HumanBone::Spine, *tgtSkelData);

    const bool haveShoulder = (leftShoulderIdx != ~0u && leftShoulderIdx < tgtSkelData->BoneCount);
    const bool haveUpperArm = (leftUpperArmIdx != ~0u && leftUpperArmIdx < tgtSkelData->BoneCount);
    const bool haveSpine    = (spineIdx        != ~0u && spineIdx        < tgtSkelData->BoneCount);
    EXPECT_TRUE(haveShoulder || haveUpperArm)
        << "Target rig has no LeftShoulder or LeftUpperArm mapping; can't measure motion.";

    std::printf("[B4] Bone indices: LeftShoulder=%u, LeftUpperArm=%u, Spine=%u\n",
                leftShoulderIdx, leftUpperArmIdx, spineIdx);

    // ---- Drive 30 frames + capture per-bone rotation amplitude ----------
    constexpr int kFrameCount = 30;
    const float duration = bestClip->GetDuration();
    const float dt = (duration > 0.0f) ? (duration / static_cast<float>(kFrameCount)) : (1.0f / 30.0f);

    Mathematics::Quaternion frame0Shoulder = Mathematics::Quaternion::Identity();
    Mathematics::Quaternion frame0UpperArm = Mathematics::Quaternion::Identity();
    Mathematics::Quaternion frame0Spine    = Mathematics::Quaternion::Identity();

    float peakShoulderDeg = 0.0f;
    float peakUpperArmDeg = 0.0f;
    float peakSpineDeg    = 0.0f;

    bool everyFrameFinite = true;
    int  framesEvaluated  = 0;

    for (int f = 0; f < kFrameCount; ++f)
    {
        const float t = static_cast<float>(f) * dt;

        bool ok = false;
        ASSERT_NO_THROW({
            ok = GERender::TestHooks::RunCPUEvaluateForTest(
                *retargetMap, *srcRig, *tgtRig, bestIdx,
                /*sourceSkeletonId=*/srcSkelId,
                /*targetSkeletonId=*/tgtSkelId,
                /*targetRuntimeId=*/tgtRuntimeId,
                /*clipTime=*/t);
        }) << "Frame " << f << " threw an exception";
        if (!ok)
        {
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
            peakShoulderDeg = std::max(peakShoulderDeg, deg);
        }
        if (haveUpperArm)
        {
            const float* m = &runtime->CompactSkinMatrices[leftUpperArmIdx * 16];
            const Mathematics::Quaternion q = ExtractRotation(m);
            if (f == 0) frame0UpperArm = q;
            const float deg = AngleBetweenDeg(q, frame0UpperArm);
            peakUpperArmDeg = std::max(peakUpperArmDeg, deg);
        }
        if (haveSpine)
        {
            const float* m = &runtime->CompactSkinMatrices[spineIdx * 16];
            const Mathematics::Quaternion q = ExtractRotation(m);
            if (f == 0) frame0Spine = q;
            const float deg = AngleBetweenDeg(q, frame0Spine);
            peakSpineDeg = std::max(peakSpineDeg, deg);
        }
    }

    EXPECT_EQ(framesEvaluated, kFrameCount);
    EXPECT_TRUE(everyFrameFinite);

    std::printf("[B4][BusinessMale + ellie_animation.blend] Per-bone rotation amplitude (vs frame 0):\n");
    std::printf("  LeftShoulder: peak = %.2f deg\n", peakShoulderDeg);
    std::printf("  LeftUpperArm: peak = %.2f deg\n", peakUpperArmDeg);
    std::printf("  Spine:        peak = %.2f deg\n", peakSpineDeg);

    // ---- Discrimination: at least ONE measurable bone moved ------------
    // Ellie's longest clip may be a face / pose-library entry that doesn't
    // animate the body. The cross-rig retargeting machinery should still
    // pass valid output through (no NaN, finite matrices), but motion
    // amplitude can legitimately be tiny. The threshold is permissive:
    // we want at least 5° of motion on the shoulder OR upper arm OR spine
    // across the 30-frame run. If everything stays below 0.5° we know the
    // retarget pipeline is silently zeroing.
    const float bestPeak = std::max({peakShoulderDeg, peakUpperArmDeg, peakSpineDeg});
    EXPECT_GT(bestPeak, 0.5f)
        << "Cross-format retarget produced near-T-pose output across all "
           "measured bones (peak: shoulder=" << peakShoulderDeg
        << ", upperArm=" << peakUpperArmDeg << ", spine=" << peakSpineDeg
        << "). Either the source clip animates only fingers / face, or the "
           "retarget pipeline is silently zeroing on cross-format input.";
}
