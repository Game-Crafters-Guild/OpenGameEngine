// Phase 15 — Editor skinned-mesh spawn-flow contract tests.
//
// The user-reported bug ("BusinessMale.fbx + A_Walk_F_Masc.fbx animates
// broken in the live editor") could have any of three root causes:
//   1. The drag-FBX-into-scene spawn flow doesn't add the components a
//      skinned animator needs (Transform / SkinnedMeshRenderer /
//      SkeletonRef / AnimatorRef).
//   2. The clip-assignment path in the AnimatorRef inspector doesn't
//      flip ClipIndex on the right entities.
//   3. The cross-rig auto-bootstrap path (Phase 2a) doesn't fire when
//      the source clip's skeleton diverges from the target's bone naming.
//
// `ModelAssetHumanoidPostLoadTests` covers the sidecar generation that
// gates (3). `HumanoidRetargetSystemTests` covers the runtime evaluation
// that depends on (3). `HumanoidRetargetRealAssetTests` covers the math
// end-to-end on real assets. This file fills the (1)+(2) gap: a headless
// integration check that `ModelEntityFactory::CreateFromModel` — the
// shared helper used by SceneViewPanel drag-drop, HierarchyPanel,
// asset-browser previews, and the Polyhaven download manager — produces
// the correct ECS shape for skinned models. And that an AnimatorRef
// clip assignment is non-destructive to SkeletonRef + SkinnedMeshRenderer.
//
// Skip-gated: when `Assets/Models/FBXTest/BusinessMale.fbx` isn't present
// or no Vulkan device is available, every test SUCCEED()s with a
// diagnostic. CI without the Synty fixture or without GPU exits clean.

#include <gtest/gtest.h>

#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "AssetCore/GUID.h"

#include "Components/Animation/AnimatorRef.h"
#include "Components/Animation/SkeletonRef.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "Components/Transform.h"

#include "ECS/World.h"
#include "ECS/ECSTemplates.h" // template instantiations for AddComponentImmediate / GetComponent

#include "ECSModules/Rendering/ClipStore.h"
#include "ECSModules/Rendering/SkeletonStore.h"

#include "Engine/Rendering/ModelEntityFactory.h"
#include "Engine/Rendering/RenderServices.h"

#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"

#include <filesystem>
#include <memory>

#include "TestDeviceHelper.h"
#include "StagedTestPaths.h"

using namespace GameEngine;
using namespace GameEngine::Components;
namespace GERender = GameEngine::Engine::Renderer;

namespace
{

constexpr const char* kBusinessMaleRel = "Assets/Models/FBXTest/BusinessMale.fbx";
constexpr const char* kWalkClipRel     = "Assets/Models/FBXTest/A_Walk_F_Masc.fbx";

std::filesystem::path StagedRoot()
{
    return TestPaths::StagedRoot();
}

class EditorSkinnedSpawnFixture : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_StagedRoot     = StagedRoot();
        m_FixtureDir   = m_StagedRoot / "Assets/Models/FBXTest";
        m_BusinessMale = m_StagedRoot / kBusinessMaleRel;
        m_WalkClip     = m_StagedRoot / kWalkClipRel;

        std::error_code ec;
        m_HasFixtures = std::filesystem::exists(m_BusinessMale, ec) && !ec
                      && std::filesystem::exists(m_WalkClip, ec) && !ec;

        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
        {
            m_HasGPU = false;
            return;
        }
        m_HasGPU = true;

        m_RenderServices = std::make_unique<GERender::RenderServices>();
        ASSERT_TRUE(m_RenderServices->Initialize(m_Device.get()));

        if (m_HasFixtures)
        {
            m_AssetManager = std::make_unique<AssetManager>();
            ASSERT_TRUE(m_AssetManager->Initialize());

            AssetSourceDesc projectSrc;
            projectSrc.Alias            = std::string(kAssetSourceAliasProject);
            projectSrc.Root             = m_FixtureDir;
            projectSrc.DerivedIdentity  = true;
            projectSrc.Priority         = 100;
            ASSERT_TRUE(m_AssetManager->RegisterSource(projectSrc));

            AssetSourceDesc editorSrc;
            editorSrc.Alias            = std::string(kAssetSourceAliasEditor);
            editorSrc.Root             = m_StagedRoot / "Apps/Editor/Assets";
            editorSrc.DerivedIdentity  = true;
            editorSrc.Priority         = 50;
            ASSERT_TRUE(m_AssetManager->RegisterSource(editorSrc));
        }
    }

    void TearDown() override
    {
        if (m_AssetManager)
            m_AssetManager->Shutdown();
        if (m_RenderServices)
            m_RenderServices->Shutdown();
        if (m_Device)
            m_Device->Shutdown();
    }

    SharedPtr<ModelAsset> LoadModelSync(const std::filesystem::path& abs)
    {
        const GUID guid = m_AssetManager->ResolveAssetGuid(abs);
        if (guid.IsNull()) return nullptr;
        auto fut = m_AssetManager->LoadAssetAsync(guid);
        return std::dynamic_pointer_cast<ModelAsset>(fut.get());
    }

    GUID ResolveGuid(const std::filesystem::path& abs)
    {
        return m_AssetManager->ResolveAssetGuid(abs);
    }

    std::filesystem::path m_StagedRoot;
    std::filesystem::path m_FixtureDir;
    std::filesystem::path m_BusinessMale;
    std::filesystem::path m_WalkClip;
    bool m_HasFixtures = false;
    bool m_HasGPU = false;
    std::unique_ptr<AssetManager> m_AssetManager;
    std::unique_ptr<Rendering::IDevice> m_Device;
    std::unique_ptr<GERender::RenderServices> m_RenderServices;
};

// Contract test: spawning a skinned ModelAsset must produce ALL of
// Transform, SkinnedMeshRenderer, SkeletonRef, AnimatorRef on every
// skinned submesh entity. This is the editor's drag-FBX-into-scene
// shape — if any of these is missing, animation breaks (e.g. no
// AnimatorRef -> AnimationSystem skips the entity entirely; no
// SkeletonRef -> SkinningUploadSystem can't find the runtime; no
// SkinnedMeshRenderer -> the CPU-skinned bind-pose mesh draws instead
// of the GPU-skinned animated mesh).
TEST_F(EditorSkinnedSpawnFixture, BusinessMaleSpawn_ProducesAllSkinnedComponents)
{
    if (!m_HasGPU)
        GTEST_SKIP() << "No Vulkan device available";
    if (!m_HasFixtures)
        GTEST_SKIP() << "Synty FBX fixtures not present; skipping.";

    auto model = LoadModelSync(m_BusinessMale);
    ASSERT_NE(model, nullptr);
    ASSERT_EQ(model->GetState(), AssetState::Loaded);
    ASSERT_GT(model->GetMeshCount(), 0u);

    // The model must report itself as skinned for the test to make sense.
    bool anySkinned = false;
    for (uint32 i = 0; i < model->GetMeshCount(); ++i)
        if (model->GetMesh(i).IsSkinned()) { anySkinned = true; break; }
    ASSERT_TRUE(anySkinned) << "BusinessMale.fbx must contain at least one skinned mesh";

    ECS::World world;

    const GUID modelGuid = ResolveGuid(m_BusinessMale);
    ASSERT_FALSE(modelGuid.IsNull());

    // Drive the same helper that SceneViewPanel / HierarchyPanel /
    // AssetBrowser / Polyhaven download manager all call.
    auto result = GERender::ModelEntityFactory::CreateFromModel(
        *m_RenderServices, world, *model, modelGuid, "BusinessMale_Test");

    ASSERT_TRUE(result.IsValid());
    ASSERT_TRUE(result.skinned)
        << "ModelEntityResult.skinned must be true for a skinned BusinessMale";
    ASSERT_FALSE(result.submeshEntities.empty());

    // Walk every skinned submesh entity and check the four required
    // components. We allow non-skinned submeshes to coexist (rare for
    // BusinessMale, but the contract is "skinned submeshes get the
    // animation components"; non-skinned submeshes get MeshRenderer only).
    bool foundSkinnedSubmesh = false;
    uint32 skinnedRuntimeId = 0;
    for (auto e : result.submeshEntities)
    {
        ASSERT_TRUE(e.IsValid());
        EXPECT_TRUE(world.GetComponent<Transform>(e) != nullptr)
            << "Every spawned entity must carry a Transform";

        const auto* smr = world.GetComponent<SkinnedMeshRenderer>(e);
        if (smr == nullptr) continue;
        foundSkinnedSubmesh = true;

        EXPECT_NE(smr->skeletonId, 0u)
            << "SkinnedMeshRenderer.skeletonId must be populated";

        const auto* skel = world.GetComponent<SkeletonRef>(e);
        ASSERT_NE(skel, nullptr)
            << "Skinned submesh must have SkeletonRef (otherwise the skinning "
               "upload system can't find a per-instance runtime)";
        EXPECT_EQ(skel->skeletonId, smr->skeletonId);
        EXPECT_NE(skel->runtimeId, 0u)
            << "SkeletonRef.runtimeId must be a freshly-allocated runtime "
               "(SkeletonStore::CreateRuntime), not zero";

        // All skinned submeshes from the same model must share the same
        // per-instance runtime so they animate as one entity.
        if (skinnedRuntimeId == 0)
            skinnedRuntimeId = skel->runtimeId;
        else
            EXPECT_EQ(skel->runtimeId, skinnedRuntimeId)
                << "All skinned submeshes must share the same runtimeId";

        const auto* anim = world.GetComponent<AnimatorRef>(e);
        ASSERT_NE(anim, nullptr)
            << "Skinned submesh must have AnimatorRef (otherwise "
               "AnimationSystem won't process it; even with no clip "
               "assigned, the component must exist for clip selection "
               "to be a non-destructive update later)";
        EXPECT_EQ(anim->ClipIndex, 0u)
            << "Initial AnimatorRef must have no clip (ClipIndex==0); "
               "the inspector / drag-drop later flips it.";
        EXPECT_TRUE(anim->IsLooping())
            << "Default AnimatorRef must be loop-enabled (kFlag_Loop set "
               "by ModelEntityFactory)";
    }
    EXPECT_TRUE(foundSkinnedSubmesh)
        << "Test should have observed at least one skinned submesh";

    // The runtime must be live in SkeletonStore.
    if (skinnedRuntimeId != 0)
    {
        auto* runtime = GERender::SkeletonStore::Instance().GetRuntime(skinnedRuntimeId);
        EXPECT_NE(runtime, nullptr)
            << "SkeletonStore::CreateRuntime should have produced a live runtime";
    }
}

// Contract test: assigning a clip to AnimatorRef is non-destructive to
// the SkeletonRef + SkinnedMeshRenderer state. The animator inspector
// flow does this every time the user picks a clip in the dropdown, and
// regressing it (e.g. via ECS chunk migrations that drop sibling
// components) would silently break animation — the bone-name matching
// auto-bootstrap path can't compensate for missing components.
TEST_F(EditorSkinnedSpawnFixture, ClipAssignment_PreservesSkeletonAndMesh)
{
    if (!m_HasGPU)
        GTEST_SKIP() << "No Vulkan device available";
    if (!m_HasFixtures)
        GTEST_SKIP() << "Synty FBX fixtures not present; skipping.";

    auto model = LoadModelSync(m_BusinessMale);
    ASSERT_NE(model, nullptr);

    // Ensure the source clip is loaded so ClipStore has it.
    auto walkSrc = LoadModelSync(m_WalkClip);
    ASSERT_NE(walkSrc, nullptr);
    ASSERT_FALSE(walkSrc->GetEmbeddedClipGuids().empty())
        << "A_Walk_F_Masc.fbx must produce at least one embedded clip GUID";

    ECS::World world;
    const GUID modelGuid = ResolveGuid(m_BusinessMale);

    auto result = GERender::ModelEntityFactory::CreateFromModel(
        *m_RenderServices, world, *model, modelGuid, "BusinessMale_Test");
    ASSERT_TRUE(result.IsValid());

    // Resolve the clip index the same way the inspector / IPC handler does.
    const GUID clipGuid = walkSrc->GetEmbeddedClipGuids().front();
    uint32 clipIndex = GERender::ClipStore::Instance().GetOrLoadClipIndex(
        clipGuid, *m_AssetManager);
    ASSERT_NE(clipIndex, 0u) << "ClipStore should resolve the embedded clip";

    // Apply ClipIndex to every skinned submesh — exactly what
    // `Editor::ApplyAnimatorPlayState` and `spawn_humanoid_test_pair` do.
    uint32 skinnedRuntimeIdBefore = 0;
    uint32 skeletonIdBefore = 0;
    for (auto e : result.submeshEntities)
    {
        const auto* skel = world.GetComponent<SkeletonRef>(e);
        if (skel)
        {
            if (skinnedRuntimeIdBefore == 0)
                skinnedRuntimeIdBefore = skel->runtimeId;
            if (skeletonIdBefore == 0)
                skeletonIdBefore = skel->skeletonId;
        }
        AnimatorRef updated{};
        updated.ClipIndex = clipIndex;
        updated.Time = 0.0f;
        updated.Speed = 1.0f;
        updated.Flags = AnimatorRef::kFlag_Loop;
        world.AddComponentImmediate(e, updated);
    }

    // After assignment, every previously-skinned submesh entity must
    // still carry SkeletonRef + SkinnedMeshRenderer + Transform AND the
    // updated AnimatorRef.
    bool sawAtLeastOne = false;
    for (auto e : result.submeshEntities)
    {
        const auto* smr = world.GetComponent<SkinnedMeshRenderer>(e);
        if (!smr) continue;
        sawAtLeastOne = true;

        EXPECT_NE(world.GetComponent<Transform>(e), nullptr);
        EXPECT_NE(world.GetComponent<SkeletonRef>(e), nullptr);
        const auto* anim = world.GetComponent<AnimatorRef>(e);
        ASSERT_NE(anim, nullptr);
        EXPECT_EQ(anim->ClipIndex, clipIndex);
        EXPECT_TRUE(anim->IsLooping());

        const auto* skel = world.GetComponent<SkeletonRef>(e);
        EXPECT_EQ(skel->skeletonId, skeletonIdBefore);
        EXPECT_EQ(skel->runtimeId, skinnedRuntimeIdBefore);
    }
    EXPECT_TRUE(sawAtLeastOne);
}

// Contract test: when the assigned clip's source skeleton differs from
// the entity's target skeleton (cross-rig retargeting case), the model
// MUST still spawn correctly with all components, AND a sidecar
// (.humanoidrig.json) must exist for both source and target — those
// are the inputs the AnimationSystem auto-bootstrap reads on the next
// frame to materialize a HumanoidRetargeterComponent.
//
// We don't drive AnimationSystem::Update here (that's
// HumanoidRetargetSystemTests + the live MCP verification); we just
// assert the upstream invariants the bootstrap depends on are met by
// the spawn flow.
TEST_F(EditorSkinnedSpawnFixture, CrossRigSpawn_SidecarsArePresent)
{
    if (!m_HasGPU)
        GTEST_SKIP() << "No Vulkan device available";
    if (!m_HasFixtures)
        GTEST_SKIP() << "Synty FBX fixtures not present; skipping.";

    // Load both target (BusinessMale) and source (A_Walk_F_Masc). After
    // each ModelAsset::PostLoad, the corresponding humanoidrig sidecar
    // must exist on disk — that is the sole input the auto-bootstrap
    // reads to resolve the source rig.
    auto target = LoadModelSync(m_BusinessMale);
    ASSERT_NE(target, nullptr);
    auto source = LoadModelSync(m_WalkClip);
    ASSERT_NE(source, nullptr);

    auto sidecarFor = [](const std::filesystem::path& p) {
        auto sc = p;
        sc.replace_extension();
        sc += ".humanoidrig.json";
        return sc;
    };

    std::error_code ec;
    EXPECT_TRUE(std::filesystem::exists(sidecarFor(m_BusinessMale), ec))
        << "Target rig sidecar must exist after PostLoad — "
           "this is the auto-bootstrap entry point.";
    EXPECT_TRUE(std::filesystem::exists(sidecarFor(m_WalkClip), ec))
        << "Source rig sidecar must exist after PostLoad — "
           "without it AnimationSystem auto-bootstrap drops the "
           "cross-rig clip and the entity stays in T-pose.";

    // Spawn the target and verify it has a SkeletonRef + AnimatorRef so
    // a clip assignment in the next frame can route through bootstrap.
    ECS::World world;
    const GUID modelGuid = ResolveGuid(m_BusinessMale);
    auto result = GERender::ModelEntityFactory::CreateFromModel(
        *m_RenderServices, world, *target, modelGuid, "BusinessMale_CrossRig");
    ASSERT_TRUE(result.IsValid());

    bool anySkinnedHasComponents = false;
    for (auto e : result.submeshEntities)
    {
        if (!world.GetComponent<SkinnedMeshRenderer>(e)) continue;
        anySkinnedHasComponents =
            world.GetComponent<SkeletonRef>(e) != nullptr &&
            world.GetComponent<AnimatorRef>(e) != nullptr;
        if (anySkinnedHasComponents) break;
    }
    EXPECT_TRUE(anySkinnedHasComponents)
        << "After spawn, at least one skinned submesh must carry both "
           "SkeletonRef and AnimatorRef — AnimationSystem requires both "
           "to enqueue a work item that the auto-bootstrap path then "
           "routes via cross-rig sniff.";
}

} // namespace
