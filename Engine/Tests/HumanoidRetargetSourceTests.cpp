#include <gtest/gtest.h>

#include "Animation/HumanoidImport.h"
#include "Animation/HumanoidRig.h"
#include "Animation/Nodes/RetargetNode.h"
#include "Animation/RetargetMap.h"
#include "AssetCore/AssetEvents.h"
#include "Assets/AnimationClip.h"
#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Components/Animation/AnimatorRef.h"
#include "Components/Animation/HumanoidRetargeterComponent.h"
#include "Components/Animation/SkeletonRef.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "ECSModules/Rendering/Systems/HumanoidRetargetSystem.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/RetargetGPUDataStore.h"
#include "EngineLogCapture.h"

#include <cstring>
#include <fstream>
#include <numeric>
#include <string>

#include <glm/gtc/quaternion.hpp>

using namespace GameEngine;
namespace R = GameEngine::Engine::Renderer;
namespace A = GameEngine::Animation;
namespace C = GameEngine::Components;
namespace fs = std::filesystem;

namespace
{
template <class T>
struct TestAsset : T
{
    using T::SetState;
    using T::T;
};
class RetargetSourceTest : public testing::Test
{
  protected:
    fs::path Root, PreviousDirectory, SourcePath;
    AssetManager* Assets = nullptr;
    std::unique_ptr<ECS::World> World;
    std::unique_ptr<R::HumanoidRetargetSystem> System;
    std::shared_ptr<TestAsset<ModelAsset>> SourceModel;
    std::shared_ptr<A::HumanoidRig> SourceRig, TargetRig;
    std::shared_ptr<TestAsset<A::RetargetMap>> Map;
    uint32 Source = 0, Target = 0;
    std::vector<uint32> Runtimes;

    void SetUp() override
    {
        PreviousDirectory = fs::current_path();
        Root = fs::temp_directory_path() / ("retarget-source-" + GUID::Generate().ToString());
        fs::create_directories(Root / "Assets");
        ScriptsConfig scripts;
        scripts.disableClr = true;
        scripts.enableHotReload = false;
        scripts.enableAutoProjectGeneration = false;
        auto& engine = EngineCore::GetInstance();
        engine.SetScriptsConfig(scripts);
        ApplicationConfig config;
        config.WorkspaceDirectory = Root.string();
        config.AssetDirectory = "Assets";
        ASSERT_TRUE(engine.Initialize(config));
        Assets = &engine.GetAssetManager();
        World = std::make_unique<ECS::World>(nullptr);
        System = std::make_unique<R::HumanoidRetargetSystem>(nullptr);
        SourcePath = Root / "Assets/source.fbx";
        std::ofstream(SourcePath) << "in-memory model fixture";
        Source = Skeleton(2, SourcePath);
        Target = Skeleton(3, Root / "Assets/target.fbx");
        SourceModel = std::make_shared<TestAsset<ModelAsset>>(Assets->ResolveAssetGuid(SourcePath), SourcePath);
        SourceModel->SetSkeletonIdForTest(Source);
        SourceModel->SetMeshesForTest({});
        Assets->RegisterLoadedAsset(SourceModel->GetGUID(), SourceModel);
        SourceRig = Rig("source.humanoidrig.json");
        TargetRig = Rig("target.humanoidrig.json");
        Map = std::make_shared<TestAsset<A::RetargetMap>>(GUID::Generate(), Root / "Assets/pair.retargetmap.json");
        A::AutoCreateRetargetMap(*SourceRig, *TargetRig, *Map);
        Map->SetSourceRigRef(SourceRig->GetGUID());
        Map->SetTargetRigRef(TargetRig->GetGUID());
        Map->SetState(AssetState::Loaded);
        Assets->RegisterLoadedAsset(Map->GetGUID(), Map);
    }

    void TearDown() override
    {
        System.reset();
        World.reset();
        for (const auto runtime : Runtimes)
            R::SkeletonStore::Instance().ReleaseRuntime(runtime);
        SourceModel.reset();
        SourceRig.reset();
        TargetRig.reset();
        Map.reset();
        EngineCore::GetInstance().Shutdown();
        R::ClipStore::Instance().ClearForTest();
        std::error_code error;
        fs::current_path(PreviousDirectory, error);
        fs::remove_all(Root, error);
    }

    uint32 Skeleton(uint32 count, const fs::path& path)
    {
        auto& store = R::SkeletonStore::Instance();
        const auto id = store.CreateSkeleton(count);
        auto& s = *store.Get(id);
        s.SourceModelPath = path;
        s.BoneCount = count;
        s.SkinJointCount = count;
        s.Parent.resize(count, 0);
        s.Parent[0] = -1;
        s.BoneNames = {"Hips", "Spine"};
        while (s.BoneNames.size() < count)
            s.BoneNames.push_back("Extra" + std::to_string(s.BoneNames.size()));
        s.RestTranslation.assign(count * 3, 0);
        s.RestTranslation[4] = 1;
        s.RestRotation.assign(count * 4, 0);
        s.RestScale.assign(count * 3, 1);
        s.BindPose.assign(count * 16, 0);
        s.InverseBind.assign(count * 16, 0);
        for (uint32 b = 0; b < count; ++b)
        {
            s.RestRotation[b * 4 + 3] = 1;
            for (uint32 k : {0u, 5u, 10u, 15u})
                s.BindPose[b * 16 + k] = s.InverseBind[b * 16 + k] = 1;
        }
        s.JointNodes.resize(count);
        std::iota(s.JointNodes.begin(), s.JointNodes.end(), 0);
        s.BuildBoneNameLookup();
        return id;
    }

    std::shared_ptr<A::HumanoidRig> Rig(const char* name)
    {
        auto rig = std::make_shared<TestAsset<A::HumanoidRig>>(GUID::Generate(), Root / "Assets" / name);
        for (const auto bone : {A::HumanBone::Hips, A::HumanBone::Spine})
        {
            A::HumanoidBoneMapping mapping;
            mapping.Canonical = bone;
            mapping.SourceBoneName = bone == A::HumanBone::Hips ? "Hips" : "Spine";
            mapping.CachedSourceIndex = bone == A::HumanBone::Hips ? 0 : 1;
            mapping.RetargetPoseRotation = Mathematics::Quaternion::Identity();
            rig->BoneMapMutable().push_back(mapping);
        }
        A::HumanoidChain chain;
        chain.Kind = A::ChainKind::Spine;
        chain.Start = A::HumanBone::Hips;
        chain.End = A::HumanBone::Spine;
        chain.IncludeBones = {A::HumanBone::Hips, A::HumanBone::Spine};
        rig->ChainsMutable().push_back(chain);
        rig->ProportionsMutable().HipHeight = 1;
        rig->SetState(AssetState::Loaded);
        Assets->RegisterLoadedAsset(rig->GetGUID(), rig);
        return rig;
    }

    // Unloads `rig` from the asset manager, as an eviction does, and registers a new object under
    // its GUID: the same rig with a tilted spine. The copy is taken before the unload clears it.
    void ReturnWithTiltedSpine(std::shared_ptr<A::HumanoidRig>& rig)
    {
        auto returned = std::make_shared<TestAsset<A::HumanoidRig>>(rig->GetGUID(), rig->GetPath());
        returned->BoneMapMutable() = rig->BoneMap();
        returned->ChainsMutable() = rig->Chains();
        returned->ProportionsMutable() = rig->Proportions();
        returned->BoneMapMutable()[1].RetargetPoseRotation =
            Mathematics::Quaternion(glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1)));
        returned->SetState(AssetState::Loaded);
        Assets->UnregisterLoadedAsset(rig->GetGUID());
        Assets->GetEventDispatcher().DispatchEvent(
            AssetEvents::AssetUnloaded(rig->GetGUID(), AssetType::HumanoidRig, rig->GetPath().string()));
        Assets->RegisterLoadedAsset(returned->GetGUID(), returned);
        rig = returned;
    }

    uint32 Clip(bool sourceInfo = true, fs::path assetPath = "memory.anim")
    {
        auto clip = std::make_shared<AnimationClip>(GUID::Generate(), std::move(assetPath));
        AnimChannel channel;
        channel.boneIndex = 1;
        channel.targetName = "Spine";
        channel.targetNameId = HashStringId("Spine");
        channel.path = AnimPath::Rotation;
        AnimKeyframe key{};
        key.rotation[0] = .258819f;
        key.rotation[3] = .965926f;
        key.scale[0] = key.scale[1] = key.scale[2] = 1;
        channel.keys.push_back(key);
        clip->SetChannelsAndDurationForTest({channel}, 1);
        if (sourceInfo)
            clip->SetSourceInfo(SourcePath, 0);
        return R::ClipStore::Instance().RegisterRuntimeClip(clip->GetGUID(), clip);
    }

    ECS::EntityHandle Actor(uint32 clip, uint32 cachedSource, uint32 previousClip = 0)
    {
        C::SkeletonRef ref;
        ref.skeletonId = Target;
        ref.runtimeId = R::SkeletonStore::Instance().CreateRuntime(Target);
        Runtimes.push_back(ref.runtimeId);
        C::AnimatorRef anim;
        anim.ClipIndex = clip;
        anim.Flags = C::AnimatorRef::kFlag_Loop;
        C::HumanoidRetargeterComponent retarget;
        retarget.Map.Set(Map->GetGUID());
        retarget.SourceClipIndex = previousClip ? previousClip : clip;
        retarget.SourceSkeletonId = cachedSource;
        return World->Create(ref, anim, retarget).GetHandle();
    }
    const C::HumanoidRetargeterComponent& State(ECS::EntityHandle h) { return *World->GetComponent<C::HumanoidRetargeterComponent>(h); }
    std::vector<float>& Palette(ECS::EntityHandle h)
    {
        return R::SkeletonStore::Instance().GetRuntime(World->GetComponent<C::SkeletonRef>(h)->runtimeId)->CompactSkinMatrices;
    }
};

TEST_F(RetargetSourceTest, ClipChangeResolvesTheAnimationModelBeforeEvaluation)
{
    const auto actor = Actor(Clip(), Target, Clip());
    System->Update(*World, .1f);
    EXPECT_EQ(State(actor).SourceSkeletonId, Source);
    EXPECT_FALSE(Palette(actor).empty());
}

TEST_F(RetargetSourceTest, FirstColdActorCannotSeedTargetAsSourceForTheSharedMap)
{
    const auto changed = Actor(Clip(), Target, Clip());
    const auto idle = Actor(Clip(), Source);
    System->Update(*World, .1f);
    ASSERT_EQ(State(changed).SourceSkeletonId, Source);
    ASSERT_EQ(State(idle).SourceSkeletonId, Source);
    EXPECT_EQ(Palette(changed), Palette(idle));

    // Inspect the unchanged GPU upload consumer's actual header. No device or
    // shader is needed to inspect its CPU mirror before upload.
    A::RetargetNode node;
    node.Configure(nullptr, SourceRig.get(), TargetRig.get(), Map.get());
    auto& store = R::SkeletonStore::Instance();
    ASSERT_TRUE(node.Build(*store.Get(State(changed).SourceSkeletonId), *store.Get(Target)));
    R::RetargetGPUDataStore gpu;
    const auto header = gpu.EnsureRigPairUploaded(Map->GetGUID(), node, *store.Get(Source), *store.Get(Target), 3);
    ASSERT_NE(header, R::kRetargetInvalidIndex);
    R::GPURigPairHeader value{};
    const auto data = gpu.GetRigDataForTest();
    ASSERT_GE(data.size(), header * 4 + sizeof(value) / sizeof(uint32));
    std::memcpy(&value, data.data() + header * 4, sizeof(value));
    EXPECT_EQ(value.srcBoneCount, 2u);
    EXPECT_EQ(value.tgtBoneCount, 3u);
}

TEST_F(RetargetSourceTest, EmptyAndWrongSourceCachesCannotOverrideExplicitModel)
{
    const auto empty = Actor(Clip(), 0);
    const auto wrong = Actor(Clip(), Target);
    System->Update(*World, .1f);
    EXPECT_EQ(State(empty).SourceSkeletonId, Source);
    EXPECT_EQ(State(wrong).SourceSkeletonId, Source);
}

TEST_F(RetargetSourceTest, LoadingSourceSkipsPublicationAndRetriesWithoutAnotherClipChange)
{
    SourceModel->SetState(AssetState::Loading);
    const auto actor = Actor(Clip(), Source);
    const auto before = Palette(actor);
    System->Update(*World, .1f);
    EXPECT_EQ(Palette(actor), before);
    EXPECT_EQ(State(actor).SourceSkeletonId, 0u);
    SourceModel->SetState(AssetState::Loaded);
    System->Update(*World, .1f);
    EXPECT_EQ(State(actor).SourceSkeletonId, Source);
    EXPECT_FALSE(Palette(actor).empty());
}

TEST_F(RetargetSourceTest, SwitchingAnimationModelsUsesTheNewModelsSkeleton)
{
    const auto actor = Actor(Clip(), Source);
    System->Update(*World, .1f);
    ASSERT_EQ(State(actor).SourceSkeletonId, Source);
    const auto path = Root / "Assets/walk.fbx";
    std::ofstream(path) << "second in-memory animation model";
    const auto skeleton = Skeleton(2, path);
    auto model = std::make_shared<TestAsset<ModelAsset>>(Assets->ResolveAssetGuid(path), path);
    model->SetSkeletonIdForTest(skeleton);
    model->SetMeshesForTest({});
    Assets->RegisterLoadedAsset(model->GetGUID(), model);
    const auto next = Clip();
    R::ClipStore::Instance().Get(next)->SetSourceInfo(path, 0);
    World->GetComponentForWrite<C::AnimatorRef>(actor)->ClipIndex = next;
    System->Update(*World, .1f);
    EXPECT_EQ(State(actor).SourceSkeletonId, skeleton);
    EXPECT_NE(State(actor).SourceSkeletonId, Source);
    EXPECT_FALSE(Palette(actor).empty());
}

TEST_F(RetargetSourceTest, LoadedModelWithoutSkeletonDoesNotUseTheTarget)
{
    SourceModel->SetSkeletonIdForTest(0);
    const auto actor = Actor(Clip(), 0);
    const auto before = Palette(actor);
    System->Update(*World, .1f);
    EXPECT_EQ(Palette(actor), before);
    EXPECT_EQ(State(actor).SourceSkeletonId, 0u);
}

TEST_F(RetargetSourceTest, UnusableSourceModelIsReportedOncePerCharacter)
{
    SourceModel->SetSkeletonIdForTest(0);
    const auto actor = Actor(Clip(), 0);
    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Warning);
        Logger::Log::Warning("RetargetSourceTest sink is live");
        for (int update = 0; update < 3; ++update)
            System->Update(*World, .1f);
        Logger::Log::Flush();
    }
    ASSERT_EQ(TestLog::CountLinesContaining(lines, "sink is live"), 1u);
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "cannot read a source skeleton"), 1u);
    EXPECT_NE(TestLog::FirstLineContaining(lines, "cannot read a source skeleton")
                  .find(SourcePath.filename().string()),
              std::string::npos);
    EXPECT_EQ(State(actor).SourceSkeletonId, 0u);
}

TEST_F(RetargetSourceTest, LoadingSourceModelStaysQuietUntilItCannotResolve)
{
    SourceModel->SetState(AssetState::Loading);
    const auto actor = Actor(Clip(), 0);
    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Warning);
        Logger::Log::Warning("RetargetSourceTest sink is live");
        for (int update = 0; update < 3; ++update)
            System->Update(*World, .1f);
        Logger::Log::Flush();
        ASSERT_EQ(TestLog::CountLinesContaining(lines, "sink is live"), 1u);
        EXPECT_EQ(TestLog::CountLinesContaining(lines, "cannot read a source skeleton"), 0u);

        SourceModel->SetSkeletonIdForTest(0);
        SourceModel->SetState(AssetState::Loaded);
        System->Update(*World, .1f);
        Logger::Log::Flush();
    }
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "cannot read a source skeleton"), 1u);
    EXPECT_EQ(State(actor).SourceSkeletonId, 0u);
}

TEST_F(RetargetSourceTest, LegacyEmbeddedModelPathResolvesWithoutSourceInfo)
{
    const auto actor = Actor(Clip(false, SourcePath), 0);
    System->Update(*World, .1f);
    EXPECT_EQ(State(actor).SourceSkeletonId, Source);
}

TEST_F(RetargetSourceTest, SourceLessProceduralClipPreservesExplicitSource)
{
    const auto actor = Actor(Clip(false), Source);
    System->Update(*World, .1f);
    EXPECT_EQ(State(actor).SourceSkeletonId, Source);
    EXPECT_FALSE(Palette(actor).empty());
}

TEST_F(RetargetSourceTest, SourceLessSameRigClipRetainsTargetFallback)
{
    const auto actor = Actor(Clip(false), 0);
    System->Update(*World, .1f);
    EXPECT_FALSE(Palette(actor).empty());
}

TEST_F(RetargetSourceTest, SharedPendingClipRetriesAllActorsAndKeepsOtherSourcesIndependent)
{
    const auto shared = Clip();
    const auto first = Actor(shared, Source);
    const auto second = Actor(shared, Target);
    const auto path = Root / "Assets/other.fbx";
    std::ofstream(path) << "independent in-memory animation model";
    const auto otherSkeleton = Skeleton(2, path);
    auto otherModel = std::make_shared<TestAsset<ModelAsset>>(Assets->ResolveAssetGuid(path), path);
    otherModel->SetSkeletonIdForTest(otherSkeleton);
    otherModel->SetMeshesForTest({});
    Assets->RegisterLoadedAsset(otherModel->GetGUID(), otherModel);
    const auto otherClip = Clip();
    R::ClipStore::Instance().Get(otherClip)->SetSourceInfo(path, 0);
    const auto other = Actor(otherClip, 0);

    SourceModel->SetState(AssetState::Loading);
    const auto beforeFirst = Palette(first);
    const auto beforeSecond = Palette(second);
    System->Update(*World, .1f);
    EXPECT_EQ(State(first).SourceSkeletonId, 0u);
    EXPECT_EQ(State(second).SourceSkeletonId, 0u);
    EXPECT_EQ(Palette(first), beforeFirst);
    EXPECT_EQ(Palette(second), beforeSecond);
    EXPECT_EQ(State(other).SourceSkeletonId, otherSkeleton);

    SourceModel->SetState(AssetState::Loaded);
    System->Update(*World, .1f);
    EXPECT_EQ(State(first).SourceSkeletonId, Source);
    EXPECT_EQ(State(second).SourceSkeletonId, Source);
    EXPECT_EQ(State(other).SourceSkeletonId, otherSkeleton);

    // A model can replace its derived skeleton without changing the clip slot.
    const auto replacement = Skeleton(2, SourcePath);
    SourceModel->SetSkeletonIdForTest(replacement);
    System->Update(*World, .1f);
    EXPECT_EQ(State(first).SourceSkeletonId, replacement);
    EXPECT_EQ(State(second).SourceSkeletonId, replacement);
    EXPECT_EQ(State(other).SourceSkeletonId, otherSkeleton);
}

TEST_F(RetargetSourceTest, ReplacedClipAtTheSameIndexIsResolvedWithoutRetainedFrameOwnership)
{
    const auto index = Clip();
    auto old = R::ClipStore::Instance().Get(index);
    const std::weak_ptr<AnimationClip> weak = old;
    const auto actor = Actor(index, Source);
    System->Update(*World, .1f);
    ASSERT_EQ(State(actor).SourceSkeletonId, Source);

    const auto path = Root / "Assets/replaced.fbx";
    std::ofstream(path) << "replacement in-memory animation model";
    const auto replacementSkeleton = Skeleton(2, path);
    auto model = std::make_shared<TestAsset<ModelAsset>>(Assets->ResolveAssetGuid(path), path);
    model->SetSkeletonIdForTest(replacementSkeleton);
    model->SetMeshesForTest({});
    Assets->RegisterLoadedAsset(model->GetGUID(), model);
    auto replacement = std::make_shared<AnimationClip>(old->GetGUID(), old->GetPath());
    replacement->SetChannelsAndDurationForTest(old->GetChannels(), old->GetDuration());
    replacement->SetSourceInfo(path, 0);
    ASSERT_EQ(R::ClipStore::Instance().RegisterRuntimeClip(replacement->GetGUID(), replacement), index);
    old.reset();
    EXPECT_TRUE(weak.expired());
    System->Update(*World, .1f);
    EXPECT_EQ(State(actor).SourceSkeletonId, replacementSkeleton);
}

TEST_F(RetargetSourceTest, PausedCharacterLetsAStillSceneSettle)
{
    // Every palette notification advances the world's caster version, which
    // re-arms the depth-derived passes; a paused character must stop doing so.
    R::RenderServices services;
    System = std::make_unique<R::HumanoidRetargetSystem>(&services);
    const auto actor = Actor(Clip(), Source);
    const auto casterVersion = [&] { return services.ShadowCasterContentVersion(World->GetWorldId()); };
    System->Update(*World, .1f);
    ASSERT_FALSE(Palette(actor).empty());

    auto before = casterVersion();
    System->Update(*World, .1f);
    EXPECT_GT(casterVersion(), before) << "a playing character re-poses every frame";

    World->GetComponentForWrite<C::AnimatorRef>(actor)->Flags |= C::AnimatorRef::kFlag_Paused;
    System->Update(*World, .1f);
    before = casterVersion();
    for (int frame = 0; frame < 3; ++frame)
        System->Update(*World, .1f);
    EXPECT_EQ(casterVersion(), before) << "a paused character's palette inputs are unchanged";

    World->GetComponentForWrite<C::AnimatorRef>(actor)->Flags &= ~C::AnimatorRef::kFlag_Paused;
    System->Update(*World, .1f);
    EXPECT_GT(casterVersion(), before) << "resuming playback re-poses the character";
}

TEST_F(RetargetSourceTest, PausedCharacterReportsAReimportedClipAndAReloadedMap)
{
    // A re-import or a hot reload changes the pose with clip index, time and
    // map GUID unchanged; the record's clip identity and build generation are
    // what report it.
    R::RenderServices services;
    System = std::make_unique<R::HumanoidRetargetSystem>(&services);
    const auto index = Clip();
    const auto actor = Actor(index, Source);
    World->GetComponentForWrite<C::AnimatorRef>(actor)->Flags |= C::AnimatorRef::kFlag_Paused;
    const auto casterVersion = [&] { return services.ShadowCasterContentVersion(World->GetWorldId()); };
    System->Update(*World, .1f);
    ASSERT_FALSE(Palette(actor).empty());
    auto before = casterVersion();
    System->Update(*World, .1f);
    ASSERT_EQ(casterVersion(), before) << "the paused character must have settled";

    // Hold the previous clip like any other owner might; the replacement is
    // built while the slot still holds it, as a re-import does.
    const auto old = R::ClipStore::Instance().Get(index);
    auto replacement = std::make_shared<AnimationClip>(old->GetGUID(), old->GetPath());
    replacement->SetChannelsAndDurationForTest(old->GetChannels(), old->GetDuration());
    replacement->SetSourceInfo(SourcePath, 0);
    ASSERT_EQ(R::ClipStore::Instance().RegisterRuntimeClip(replacement->GetGUID(), replacement), index);
    System->Update(*World, .1f);
    EXPECT_GT(casterVersion(), before) << "a re-imported clip re-poses the character";

    before = casterVersion();
    System->Update(*World, .1f);
    ASSERT_EQ(casterVersion(), before) << "the paused character must settle again";
    Assets->GetEventDispatcher().DispatchEvent(
        AssetEvents::AssetReloaded(Map->GetGUID(), AssetType::RetargetMap, Map->GetPath().string()));
    System->Update(*World, .1f);
    EXPECT_GT(casterVersion(), before) << "a reloaded retarget map rebuilds the pose";
}

TEST_F(RetargetSourceTest, AMapThatLeavesMemoryIsResolvedAgainWhenItReturns)
{
    // A map that leaves the asset manager (an eviction, a transient release) and comes back is a
    // new object under the same GUID. A character bound to it must retarget through the new
    // object, as a character created afterwards does, never through the one that left.
    const auto clip = Clip();
    const auto bound = Actor(clip, Source);
    System->Update(*World, .1f);
    ASSERT_FALSE(Palette(bound).empty());
    const std::vector<float> throughDeparted = Palette(bound);

    // Held here so that a character still pointing at it reads its content, not freed memory.
    const auto departed = Map;
    Assets->UnregisterLoadedAsset(departed->GetGUID());
    Assets->GetEventDispatcher().DispatchEvent(
        AssetEvents::AssetUnloaded(departed->GetGUID(), AssetType::RetargetMap, departed->GetPath().string()));

    auto tilted = Rig("tilted.humanoidrig.json");
    tilted->BoneMapMutable()[1].RetargetPoseRotation =
        Mathematics::Quaternion(glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1)));
    Map = std::make_shared<TestAsset<A::RetargetMap>>(departed->GetGUID(), departed->GetPath());
    A::AutoCreateRetargetMap(*tilted, *TargetRig, *Map);
    Map->SetSourceRigRef(tilted->GetGUID());
    Map->SetTargetRigRef(TargetRig->GetGUID());
    Map->SetState(AssetState::Loaded);
    Assets->RegisterLoadedAsset(Map->GetGUID(), Map);

    const auto fresh = Actor(clip, Source);
    System->Update(*World, .1f);
    ASSERT_NE(Palette(fresh), throughDeparted) << "the returning map must pose differently for this test to see it";
    EXPECT_EQ(Palette(bound), Palette(fresh)) << "the bound character still retargets through the map that left";
}

TEST_F(RetargetSourceTest, ASourceRigThatLeavesMemoryIsResolvedAgainWhenItReturns)
{
    // The same for the source rig the map names: the bound character must use the returning
    // object, not the one that left.
    const auto clip = Clip();
    const auto bound = Actor(clip, Source);
    System->Update(*World, .1f);
    ASSERT_FALSE(Palette(bound).empty());
    const std::vector<float> throughDeparted = Palette(bound);

    ReturnWithTiltedSpine(SourceRig);
    const auto fresh = Actor(clip, Source);
    System->Update(*World, .1f);
    ASSERT_NE(Palette(fresh), throughDeparted) << "the returning rig must pose differently for this test to see it";
    EXPECT_EQ(Palette(bound), Palette(fresh)) << "the bound character still retargets through the rig that left";
}

TEST_F(RetargetSourceTest, ATargetRigThatLeavesMemoryIsResolvedAgainWhenItReturns)
{
    // The same for the target rig the map names: the bound character must use the returning
    // object, not the one that left.
    const auto clip = Clip();
    const auto bound = Actor(clip, Source);
    System->Update(*World, .1f);
    ASSERT_FALSE(Palette(bound).empty());
    const std::vector<float> throughDeparted = Palette(bound);

    ReturnWithTiltedSpine(TargetRig);
    const auto fresh = Actor(clip, Source);
    System->Update(*World, .1f);
    ASSERT_NE(Palette(fresh), throughDeparted) << "the returning rig must pose differently for this test to see it";
    EXPECT_EQ(Palette(bound), Palette(fresh)) << "the bound character still retargets through the rig that left";
}

TEST_F(RetargetSourceTest, SourceLessSharedClipRetainsEachActorsOwnSource)
{
    const auto shared = Clip(false);
    const auto crossRig = Actor(shared, Source);
    const auto sameRig = Actor(shared, Target);
    System->Update(*World, .1f);
    EXPECT_EQ(State(crossRig).SourceSkeletonId, Source);
    EXPECT_EQ(State(sameRig).SourceSkeletonId, Target);
}
} // namespace
