#include <gtest/gtest.h>

#include "Assets/AnimationClip.h"
#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Components/Animation/Animator.h"
#include "Components/Animation/AnimatorRef.h"
#include "Components/Animation/SkeletonRef.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/SceneEntityTag.h"
#include "ECS/ECSTemplates.h"
#include "ECS/UnresolvedComponentStore.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "Engine/Rendering/AnimatorScenePlayback.h"
#include "Scene/SceneIO.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Animation/AnimationController.h"
#include "Animation/AnimationLibrary.h"
#include "Assets/AssetRegistry.h"
#include "EngineLogCapture.h"
#include "JobSystem/WorkStealingThreadPool.h"
#if defined(EMBEDDED_ANIMATION_EDITOR_TESTS)
#include "PlayMode/ModelAnimationPlayMode.h"
#endif
#include <algorithm>
#include <chrono>
#include <memory>
#include <string>

#include <filesystem>
#include <fstream>
#include <iterator>

using namespace GameEngine;
using namespace GameEngine::Components;
using namespace GameEngine::Engine::Renderer;

namespace
{
class PendingModel final : public ModelAsset
{
  public:
    using ModelAsset::ModelAsset;
    void MarkPending() { SetState(AssetState::Loading); }
};
class EmbeddedAnimationPersistence : public testing::Test, public Scene::ISceneAssetResolver
{
  protected:
    void SetUp() override
    {
        Scene::EnsureBuiltInSchemasRegistered();
        Root = std::filesystem::temp_directory_path() / ("embedded-animation-" + GUID::Generate().ToString());
        std::filesystem::create_directories(Root);
        ModelPath = Root / "source.fbx";
        ScenePath = Root / "test.scene";
        std::ofstream(ModelPath) << "source fixture";
        ClipGuid = ModelAsset::DeriveEmbeddedClipGuid(ModelGuid, 0);
    }
    void TearDown() override
    {
        ClipStore::Instance().ClearForTest();
        std::error_code ignored;
        std::filesystem::remove_all(Root, ignored);
    }
    std::filesystem::path GetAssetRoot() const override { return Root; }
    // One mount in this fixture: Root is both the project mount and the asset root, so
    // a miss still names the Root candidate — the answer AssetManager gives.
    std::filesystem::path ResolveAssetPath(const std::filesystem::path& authoredPath) const override
    {
        if (authoredPath.is_absolute())
            return authoredPath;
        if (Root.empty())
            return {};
        return (Root / authoredPath).lexically_normal();
    }
    GUID ResolveGuid(const GUID& guid) const override { return !OldModelGuid.IsNull() && guid == OldModelGuid ? ModelGuid : guid; }
    GUID GetOrCreateAssetGuid(const std::filesystem::path& path) override
    {
        return path == ModelPath ? ModelGuid : GUID{};
    }
    bool TryGetPathAndType(const GUID& guid, std::filesystem::path& path, AssetType& type) const override
    {
        if (!StandaloneGuid.IsNull() && guid == StandaloneGuid)
        {
            path = Root / "standalone.anim";
            type = AssetType::Animation;
            return true;
        }
        if (guid != ModelGuid)
            return false;
        path = ModelPath;
        type = AssetType::Model;
        return true;
    }
    bool TryGetGuidAndType(const std::filesystem::path& path, GUID& guid, AssetType& type) const override
    {
        if (!StandaloneGuid.IsNull() && path == Root / "standalone.anim")
        {
            guid = StandaloneGuid;
            type = AssetType::Animation;
            return true;
        }
        if (path != ModelPath)
            return false;
        guid = ModelGuid;
        type = AssetType::Model;
        return true;
    }
    // Whether the load recorded `guid` as a reference that bound to nothing.
    static bool IsUnresolved(const ECS::World& world, const GUID& guid)
    {
        const ECS::UnresolvedComponentStore* store = world.TryGetUnresolvedComponents();
        return !guid.IsNull() && store && store->FindUnresolvedReference(guid.ToString());
    }
    // Whether the load recorded any of the GUIDs this fixture binds or names as unresolved.
    bool AnyFixtureGuidUnresolved(const ECS::World& world) const
    {
        for (const GUID& guid : {ModelGuid, ClipGuid, OldModelGuid, StandaloneGuid})
            if (IsUnresolved(world, guid))
                return true;
        return false;
    }
    void PublishClip()
    {
        auto clip = MakeShared<AnimationClip>(ClipGuid, ModelPath);
        clip->SetSourceInfo(ModelPath, 0);
        clip->SetChannelsAndDurationForTest({}, 2.0f);
        ClipStore::Instance().RegisterRuntimeClip(ClipGuid, clip);
    }
    std::string SaveAnimator(const Animator& animator)
    {
        ECS::World world;
        world.Create<Animator>(animator);
        Scene::SaveOptions options;
        options.assetRootOverride = Root;
        options.assetResolver = this;
        EXPECT_TRUE(Scene::SaveSceneToFile(world, ScenePath, options));
        std::ifstream input(ScenePath);
        return {std::istreambuf_iterator<char>(input), {}};
    }
    std::string Save()
    {
        Animator animator;
        animator.clipGuid.Set(ClipGuid);
        return SaveAnimator(animator);
    }
    void Load(ECS::World& world)
    {
        Scene::LoadOptions options;
        options.assetRootOverride = Root;
        options.assetResolver = this;
        options.outDegradation = &Degradation;
        EXPECT_TRUE(Scene::LoadSceneFromFile(world, ScenePath, options));
    }
    void Write(std::string_view text)
    {
        std::ofstream(ScenePath) << text;
    }
    Animator SourceAnimator() const
    {
        Animator a;
        a.clipSourceModelGuid.Set(ModelGuid);
        a.clipSourceAnimationIndex = 0;
        a.clipGuid.Set(ClipGuid);
        return a;
    }
    ECS::EntityHandle PlaybackEntity(ECS::World& world, const Animator& animator, GUID target)
    {
        const auto h = world.Create<Animator>(animator).GetHandle();
        world.AddComponentImmediate(h, AnimatorRef{});
        world.AddComponentImmediate(h, SkeletonRef{});
        MeshRenderer mesh;
        mesh.modelAssetGuid.Set(target);
        world.AddComponentImmediate(h, mesh);
        return h;
    }
    std::filesystem::path WriteModel(std::string_view name)
    {
        const auto path = Root / (std::string(name) + ".gltf");
        const float vertices[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        const uint16_t indices[] = {0, 1, 2, 0};
        const float times[] = {0, 1};
        const float positions[] = {0, 0, 0, 0, 2, 0};
        std::ofstream binary(Root / "animation.bin", std::ios::binary);
        binary.write(reinterpret_cast<const char*>(vertices), sizeof(vertices));
        binary.write(reinterpret_cast<const char*>(indices), sizeof(indices));
        binary.write(reinterpret_cast<const char*>(times), sizeof(times));
        binary.write(reinterpret_cast<const char*>(positions), sizeof(positions));
        binary.close();
        std::ofstream(path) << R"({"asset":{"version":"2.0"},"buffers":[{"uri":"animation.bin","byteLength":76}],
"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6},{"buffer":0,"byteOffset":44,"byteLength":8},{"buffer":0,"byteOffset":52,"byteLength":24}],
"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"},{"bufferView":2,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]},{"bufferView":3,"componentType":5126,"count":2,"type":"VEC3"}],
"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],"nodes":[{"name":"Root","mesh":0}],"scenes":[{"nodes":[0]}],"scene":0,
"animations":[{"name":"SourceMotion","samplers":[{"input":2,"output":3,"interpolation":"LINEAR"}],"channels":[{"sampler":0,"target":{"node":0,"path":"translation"}}]}]})";
        return path;
    }
    std::filesystem::path Root, ModelPath, ScenePath;
    GUID ModelGuid = GUID::Generate(), ClipGuid, OldModelGuid, StandaloneGuid;
    Scene::SceneLoadDegradation Degradation;
};
} // namespace

TEST_F(EmbeddedAnimationPersistence, SavesVerifiedContainerInsteadOfRecordlessClip)
{
    PublishClip();
    const auto text = Save();
    EXPECT_NE(text.find("Animator.clipSourceModelGuid ="), std::string::npos);
    EXPECT_NE(text.find("Animator.clipSourceAnimationIndex = 0"), std::string::npos);
    EXPECT_EQ(text.find("Animator.clipGuid ="), std::string::npos);
}

TEST_F(EmbeddedAnimationPersistence, GltfEmbeddedClipSavesItsContainerFromResidentMetadata)
{
    ModelPath = WriteModel("source");
    auto model = MakeShared<ModelAsset>(ModelGuid, ModelPath);
    ASSERT_TRUE(model->Load());
    ASSERT_NE(ClipStore::Instance().GetIndexIfPresent(ClipGuid), 0u);
    const auto text = Save();
    EXPECT_NE(text.find("Animator.clipSourceModelGuid ="), std::string::npos);
    EXPECT_NE(text.find("Animator.clipSourceAnimationIndex = 0"), std::string::npos);
    EXPECT_EQ(text.find("Animator.clipGuid ="), std::string::npos);
}

TEST_F(EmbeddedAnimationPersistence, ActualSceneLoadDoesNotReportKnownEmbeddedClipMissing)
{
    PublishClip();
    Save();
    ClipStore::Instance().ClearForTest();
    ECS::World restored;
    Load(restored);
    EXPECT_FALSE(AnyFixtureGuidUnresolved(restored));
    EXPECT_FALSE(Degradation.IsDegraded());
    unsigned animators = 0;
    restored.Query<ECS::Read<Animator>>().Each([&](const Animator& animator)
                                               { ++animators; EXPECT_EQ(animator.clipGuid.ToGuid(), ClipGuid); });
    EXPECT_EQ(animators, 1u);
}

TEST_F(EmbeddedAnimationPersistence, UnknownClipRemainsUnresolved)
{
    Save();
    ECS::World restored;
    Load(restored);
    EXPECT_TRUE(IsUnresolved(restored, ClipGuid));
}

// A scene file written before the pair existed keeps its derived clip GUID on
// load, and re-saving it once the clip is resident upgrades it to the pair.
TEST_F(EmbeddedAnimationPersistence, LegacySceneFileReSavesAsTheDurablePair)
{
    Write("[scene name=\"x\" version=1]\n[entity id=\"a\"]\nAnimator.clipGuid = [guid=\""
          + ClipGuid.ToString() + "\"]\n");
    ECS::World world;
    Load(world);
    unsigned animators = 0;
    world.Query<ECS::Read<Animator>>().Each([&](const Animator& animator) {
        ++animators;
        EXPECT_EQ(animator.clipGuid.ToGuid(), ClipGuid);
        EXPECT_TRUE(animator.clipSourceModelGuid.IsNull());
    });
    ASSERT_EQ(animators, 1u);

    PublishClip();
    Scene::SaveOptions options;
    options.assetRootOverride = Root;
    options.assetResolver = this;
    ASSERT_TRUE(Scene::SaveSceneToFile(world, ScenePath, options));
    std::ifstream input(ScenePath);
    const std::string text{std::istreambuf_iterator<char>(input), {}};
    EXPECT_NE(text.find("Animator.clipSourceModelGuid ="), std::string::npos);
    EXPECT_NE(text.find("Animator.clipSourceAnimationIndex = 0"), std::string::npos);
    EXPECT_EQ(text.find("Animator.clipGuid ="), std::string::npos);
}

TEST_F(EmbeddedAnimationPersistence, PairLoadsInEitherOrderAndEnumeratesContainer)
{
    for (bool reverse : {false, true})
    {
        const std::string model = "Animator.clipSourceModelGuid = [guid=\"" + ModelGuid.ToString() + "\"]\n";
        const std::string index = "Animator.clipSourceAnimationIndex = 0\n";
        Write("[scene name=\"x\" version=1]\n[entity id=\"a\"]\n" + (reverse ? index + model : model + index));
        ECS::World world;
        Load(world);
        ASSERT_FALSE(Degradation.IsDegraded());
        world.Query<ECS::Read<Animator>>().Each([&](auto h, const Animator& a)
                                                {
            EXPECT_EQ(a.clipGuid.ToGuid(), ClipGuid);
            EXPECT_EQ(a.clipSourceModelGuid.ToGuid(), ModelGuid);
            unsigned visited = 0;
            Scene::SceneSchemaRegistry::Find("Animator")->EnumerateAssetReferences(world, h,
                [&](const GUID& guid, AssetType type, auto, auto name) {
                    ++visited; EXPECT_EQ(guid, ModelGuid); EXPECT_EQ(type, AssetType::Model);
                    EXPECT_EQ(name, "clipsourcemodelguid");
                });
            EXPECT_EQ(visited, 1u); });
    }
}

TEST_F(EmbeddedAnimationPersistence, InvalidPairsDoNotPartiallyReplaceExistingAnimator)
{
    const auto* schema = Scene::SceneSchemaRegistry::Find("Animator");
    ASSERT_NE(schema, nullptr);
    Scene::SceneLoadContext context;
    context.Resolver = this;
    context.AssetRoot = Root;
    for (const std::string bad : {"-1", "4294967295", "4294967296", "0junk", "embedded:0"})
    {
        ECS::World world;
        Animator initial;
        initial.speedScale = 3;
        const auto h = world.Create<Animator>(initial).GetHandle();
        const std::string model = "[guid=\"" + ModelGuid.ToString() + "\"]";
        const std::pair<std::string_view, std::string_view> props[] = {
            {"speedscale", "7"}, {"clipsourcemodelguid", model}, {"clipsourceanimationindex", bad}};
        std::string error;
        std::size_t failed = 0;
        EXPECT_FALSE(schema->ApplyProperties(world, h, context, props, &error, &failed));
        EXPECT_EQ(world.GetComponent<Animator>(h)->speedScale, 3);
        EXPECT_TRUE(world.GetComponent<Animator>(h)->clipSourceModelGuid.IsNull());
    }
}

TEST_F(EmbeddedAnimationPersistence, NamedAndGuidCommandsClearPreviousSource)
{
    for (unsigned command = 0; command < 5; ++command)
    {
        auto a = SourceAnimator();
        switch (command)
        {
        case 0:
            a.PlayState("name");
            break;
        case 1:
            a.PlayState(GUID::Generate());
            break;
        case 2:
            a.CrossFadeSeconds("name", 0.2f);
            break;
        case 3:
            a.CrossFadeSeconds(GUID::Generate(), 0.2f);
            break;
        case 4:
            a.PlaySection("name", 0, 1);
            break;
        }
        EXPECT_TRUE(a.clipSourceModelGuid.IsNull());
        EXPECT_EQ(a.clipSourceAnimationIndex, UINT32_MAX);
        if (command == 0 || command == 2 || command == 4)
            EXPECT_TRUE(a.clipGuid.IsNull());
    }
}

TEST_F(EmbeddedAnimationPersistence, SelectionApiKeepsClipAndSourcePairConsistent)
{
    Animator a;
    a.source = AnimatorPlaybackSource::Timeline;
    a.SelectEmbeddedClip(ModelGuid, 0);
    EXPECT_EQ(a.source, AnimatorPlaybackSource::Clip);
    EXPECT_EQ(a.clipGuid.ToGuid(), ClipGuid);
    EXPECT_EQ(a.clipSourceModelGuid.ToGuid(), ModelGuid);
    EXPECT_EQ(a.clipSourceAnimationIndex, 0u);

    const GUID standalone = GUID::Generate();
    a.SelectClip(standalone);
    EXPECT_EQ(a.clipGuid.ToGuid(), standalone);
    EXPECT_TRUE(a.clipSourceModelGuid.IsNull());
    EXPECT_EQ(a.clipSourceAnimationIndex, UINT32_MAX);

    // Clearing after an embedded selection leaves nothing for playback to
    // fall back to: the earlier source pair must go with the clip.
    a.SelectEmbeddedClip(ModelGuid, 0);
    a.SelectClip(GUID::Null());
    EXPECT_TRUE(a.clipGuid.IsNull());
    EXPECT_TRUE(a.clipSourceModelGuid.IsNull());
    AssetManager assets;
    bool pending = true;
    EXPECT_EQ(ResolveAnimatorClipIndex(a, GUID::Null(), assets, &pending), 0u);
    EXPECT_FALSE(pending);
}

TEST_F(EmbeddedAnimationPersistence, ClearingSourceClearsTheWholePairAndRejectsNonModelSource)
{
    const auto* schema = Scene::SceneSchemaRegistry::Find("Animator");
    ASSERT_NE(schema, nullptr);
    Scene::SceneLoadContext context;
    context.Resolver = this;
    context.AssetRoot = Root;
    ECS::World world;
    const auto h = world.Create<Animator>(SourceAnimator()).GetHandle();
    std::string error;
    ASSERT_TRUE(schema->ApplyProperty(world, h, context, "clipsourcemodelguid", "0", &error));
    const auto* cleared = world.GetComponent<Animator>(h);
    EXPECT_TRUE(cleared->clipSourceModelGuid.IsNull());
    EXPECT_EQ(cleared->clipSourceAnimationIndex, UINT32_MAX);
    EXPECT_TRUE(cleared->clipGuid.IsNull());

    StandaloneGuid = GUID::Generate();
    std::ofstream(Root / "standalone.anim") << "standalone fixture";
    const std::string reference = "[guid=\"" + StandaloneGuid.ToString() + "\"]";
    const std::pair<std::string_view, std::string_view> properties[] = {
        {"clipsourcemodelguid", reference}, {"clipsourceanimationindex", "0"}};
    std::size_t failed = 0;
    EXPECT_FALSE(schema->ApplyProperties(world, h, context, properties, &error, &failed));
    EXPECT_TRUE(world.GetComponent<Animator>(h)->clipSourceModelGuid.IsNull());
}

TEST_F(EmbeddedAnimationPersistence, ColdPlaybackLoadsExactSourceAndRejectsBadIndex)
{
    ModelPath = WriteModel("source");
    const auto targetPath = WriteModel("target");
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(Root));
    ASSERT_TRUE(assets.GetRegistry().RegisterAsset(ModelPath));
    ASSERT_TRUE(assets.GetRegistry().RegisterAsset(targetPath));
    ModelGuid = assets.GetRegistry().GetAssetGUID(ModelPath);
    ClipGuid = ModelAsset::DeriveEmbeddedClipGuid(ModelGuid, 0);
    const auto target = assets.GetRegistry().GetAssetGUID(targetPath);
    auto targetModel = MakeShared<ModelAsset>(target, targetPath);
    ASSERT_TRUE(targetModel->Load());
    assets.RegisterLoadedAsset(target, targetModel);
    ASSERT_FALSE(targetModel->GetEmbeddedClipGuids().empty());
    ASSERT_EQ(ClipStore::Instance().GetIndexIfPresent(ClipGuid), 0u);
    ECS::World world;
    const auto h = PlaybackEntity(world, SourceAnimator(), target);
    BootstrapSceneAnimators(world, assets);
    const uint32 sourceIndex = ClipStore::Instance().GetIndexIfPresent(ClipGuid);
    ASSERT_NE(sourceIndex, 0u);
    EXPECT_EQ(world.GetComponent<AnimatorRef>(h)->ClipIndex, sourceIndex);
    EXPECT_NE(sourceIndex, ClipStore::Instance().GetIndexIfPresent(targetModel->GetEmbeddedClipGuids().front()));

    auto named = SourceAnimator();
    named.PlaySection("SourceMotion", 0, 0.5f);
    named.pendingCommand = AnimatorPlaybackCommand::None; // Bootstrap uses the same selection resolver.
    const auto namedEntity = PlaybackEntity(world, named, target);
    BootstrapSceneAnimators(world, assets);
    EXPECT_EQ(world.GetComponent<AnimatorRef>(namedEntity)->ClipIndex,
              ClipStore::Instance().GetIndexIfPresent(targetModel->GetEmbeddedClipGuids().front()));

    auto invalid = SourceAnimator();
    invalid.clipSourceAnimationIndex = 999;
    invalid.clipGuid.Set(ModelAsset::DeriveEmbeddedClipGuid(ModelGuid, 999));
    const auto bad = PlaybackEntity(world, invalid, target);
    BootstrapSceneAnimators(world, assets);
    EXPECT_EQ(world.GetComponent<AnimatorRef>(bad)->ClipIndex, 0u);

    assets.UnloadAssetAsync(ModelGuid).get();
    ClipStore::Instance().ClearForTest();
    ECS::World second;
    const auto fresh = PlaybackEntity(second, SourceAnimator(), target);
    BootstrapSceneAnimators(second, assets);
    const auto reloaded = ClipStore::Instance().GetIndexIfPresent(ClipGuid);
    ASSERT_NE(reloaded, 0u);
    EXPECT_EQ(second.GetComponent<AnimatorRef>(fresh)->ClipIndex, reloaded);
}

TEST_F(EmbeddedAnimationPersistence, SourceRedirectDerivesCanonicalClipWithoutOldStore)
{
    OldModelGuid = GUID::Generate();
    Write("[scene name=\"x\" version=1]\n[entity id=\"a\"]\nAnimator.clipSourceModelGuid = [guid=\"" +
          OldModelGuid.ToString() + "\"]\nAnimator.clipSourceAnimationIndex = 0\n");
    ECS::World world;
    Load(world);
    ASSERT_FALSE(Degradation.IsDegraded());
    EXPECT_FALSE(AnyFixtureGuidUnresolved(world));
    world.Query<ECS::Read<Animator>>().Each([&](const Animator& a)
                                            {
        EXPECT_EQ(a.clipSourceModelGuid.ToGuid(),ModelGuid);
        EXPECT_EQ(a.clipGuid.ToGuid(),ClipGuid); });
}

TEST_F(EmbeddedAnimationPersistence, MissingSourceStillReportsUnresolved)
{
    const auto missing = GUID::Generate();
    Write("[scene name=\"x\" version=1]\n[entity id=\"a\"]\nAnimator.clipSourceModelGuid = [guid=\"" +
          missing.ToString() + "\"]\nAnimator.clipSourceAnimationIndex = 0\n");
    ECS::World world;
    Load(world);
    EXPECT_TRUE(IsUnresolved(world, missing));
}

TEST_F(EmbeddedAnimationPersistence, PartialAndContradictorySourcesAreRejected)
{
    const auto model = "Animator.clipSourceModelGuid = [guid=\"" + ModelGuid.ToString() + "\"]\n";
    const std::string index = "Animator.clipSourceAnimationIndex = 0\n";
    for (const std::string& props : {model, index, model + index + "Animator.clipGuid = [guid=\"" + ModelGuid.ToString() + "\"]\n"})
    {
        Degradation = {};
        Write("[scene name=\"x\" version=1]\n[entity id=\"a\"]\n" + props);
        ECS::World world;
        Load(world);
        EXPECT_TRUE(Degradation.IsDegraded());
    }
    auto partial = SourceAnimator();
    partial.clipSourceAnimationIndex = UINT32_MAX;
    partial.clipGuid.Clear();
    SaveAnimator(partial);
    ECS::World world;
    Degradation = {};
    Load(world);
    EXPECT_TRUE(Degradation.IsDegraded());
}

TEST_F(EmbeddedAnimationPersistence, DirectStandaloneAssignmentReplacesStaleEmbeddedSource)
{
    StandaloneGuid = GUID::Generate();
    auto clip = MakeShared<AnimationClip>(StandaloneGuid, Root / "standalone.anim");
    clip->SetSourceInfo(ModelPath, 0); // Imported .anim may retain origin info.
    clip->SetChannelsAndDurationForTest({}, 2.0f);
    ASSERT_TRUE(clip->SaveToPath(Root / "standalone.anim"));
    const auto index = ClipStore::Instance().RegisterRuntimeClip(StandaloneGuid, clip);
    auto a = SourceAnimator();
    a.clipGuid.Set(StandaloneGuid); // Existing direct game assignment.
    const auto saved = SaveAnimator(a);
    EXPECT_NE(saved.find("Animator.clipGuid ="), std::string::npos);
    EXPECT_EQ(saved.find("Animator.clipSourceModelGuid ="), std::string::npos);
    ECS::World restored;
    Load(restored);
    EXPECT_FALSE(AnyFixtureGuidUnresolved(restored));
    ASSERT_FALSE(Degradation.IsDegraded());
    AssetManager assets;
    assets.RegisterLoadedAsset(StandaloneGuid, clip);
    ECS::World world;
    const auto h = PlaybackEntity(world, a, GUID::Generate());
    BootstrapSceneAnimators(world, assets);
    EXPECT_EQ(world.GetComponent<AnimatorRef>(h)->ClipIndex, index);
}

TEST_F(EmbeddedAnimationPersistence, RepeatedSaveRestoreRetainsSourceWithoutRuntimeClip)
{
    PublishClip();
    Save();
    ClipStore::Instance().ClearForTest();
    for (unsigned i = 0; i < 3; ++i)
    {
        ECS::World world;
        Load(world);
        EXPECT_FALSE(AnyFixtureGuidUnresolved(world));
        ASSERT_FALSE(Degradation.IsDegraded());
        Animator value;
        world.Query<ECS::Read<Animator>>().Each([&](const Animator& a)
                                                { value = a; });
        const auto text = SaveAnimator(value);
        EXPECT_NE(text.find("Animator.clipSourceModelGuid ="), std::string::npos);
        EXPECT_EQ(value.clipGuid.ToGuid(), ClipGuid);
    }
}

TEST_F(EmbeddedAnimationPersistence, WorldSnapshotPreservesTypedSourcePair)
{
    ECS::World source;
    source.Create<Animator>(SourceAnimator());
    const auto bytes = source.SerializeWorld();
    source.Clear();
    ECS::World restored;
    restored.DeserializeWorld(bytes);
    unsigned count = 0;
    restored.Query<ECS::Read<Animator>>().Each([&](const Animator& a)
                                               {
        ++count;
        EXPECT_EQ(a.clipSourceModelGuid.ToGuid(),ModelGuid);
        EXPECT_EQ(a.clipSourceAnimationIndex,0u);
        EXPECT_EQ(a.clipGuid.ToGuid(),ClipGuid); });
    EXPECT_EQ(count, 1u);
}

TEST_F(EmbeddedAnimationPersistence, PendingSourceRetriesThroughExistingCommandOwner)
{
    ModelPath = WriteModel("source");
    AssetManager assets;
    auto model = MakeShared<PendingModel>(ModelGuid, ModelPath);
    model->MarkPending();
    assets.RegisterLoadedAsset(ModelGuid, model);
    ECS::World world;
    auto authored = SourceAnimator();
    authored.seekTimeSeconds = 0.375f;
    const auto h = PlaybackEntity(world, authored, GUID::Generate());
    BootstrapSceneAnimators(world, assets);
    EXPECT_EQ(world.GetComponent<Animator>(h)->pendingCommand, AnimatorPlaybackCommand::Autoplay);
    ProcessClipAnimatorCommands(world, assets);
    EXPECT_EQ(world.GetComponent<Animator>(h)->pendingCommand, AnimatorPlaybackCommand::Autoplay);
    EXPECT_EQ(world.GetComponent<AnimatorRef>(h)->ClipIndex, 0u);
    ASSERT_TRUE(model->Load());
    ProcessClipAnimatorCommands(world, assets);
    EXPECT_EQ(world.GetComponent<Animator>(h)->pendingCommand, AnimatorPlaybackCommand::None);
    EXPECT_EQ(world.GetComponent<AnimatorRef>(h)->ClipIndex, ClipStore::Instance().GetIndexIfPresent(ClipGuid));
    EXPECT_NE(world.GetComponent<AnimatorRef>(h)->ClipIndex, 0u);
    EXPECT_FLOAT_EQ(world.GetComponent<AnimatorRef>(h)->Time, authored.seekTimeSeconds);

    // Ordinary same-clip Play remains idempotent; selecting another clip
    // starts at zero rather than applying an old authored seek.
    world.GetComponentForWrite<AnimatorRef>(h)->Time = 0.6f;
    auto replay = *world.GetComponent<Animator>(h);
    replay.PlayState(ClipGuid);
    world.AddComponentImmediate(h, replay);
    ProcessClipAnimatorCommands(world, assets);
    EXPECT_FLOAT_EQ(world.GetComponent<AnimatorRef>(h)->Time, 0.6f);
    const auto otherGuid = GUID::Generate();
    auto other = MakeShared<AnimationClip>(otherGuid, Root / "other.anim");
    other->SetChannelsAndDurationForTest({}, 1.0f);
    ClipStore::Instance().RegisterRuntimeClip(otherGuid, other);
    replay.PlayState(otherGuid);
    world.AddComponentImmediate(h, replay);
    ProcessClipAnimatorCommands(world, assets);
    EXPECT_FLOAT_EQ(world.GetComponent<AnimatorRef>(h)->Time, 0.0f);
}

// A scene authored before the source pair existed carries only the derived
// embedded GUID. No registry record resolves it, so playback waits for the
// container that registers it instead of giving up on the first miss.
TEST_F(EmbeddedAnimationPersistence, LegacyDerivedClipGuidWaitsForItsContainer)
{
    ModelPath = WriteModel("source");
    AssetManager assets;
    auto model = MakeShared<PendingModel>(ModelGuid, ModelPath);
    model->MarkPending();
    assets.RegisterLoadedAsset(ModelGuid, model);
    ECS::World world;
    Animator legacy;
    legacy.clipGuid.Set(ClipGuid);
    const auto h = PlaybackEntity(world, legacy, ModelGuid);
    BootstrapSceneAnimators(world, assets);
    EXPECT_EQ(world.GetComponent<Animator>(h)->pendingCommand, AnimatorPlaybackCommand::Autoplay);
    ASSERT_TRUE(model->Load());
    ProcessClipAnimatorCommands(world, assets);
    EXPECT_EQ(world.GetComponent<Animator>(h)->pendingCommand, AnimatorPlaybackCommand::None);
    EXPECT_NE(world.GetComponent<AnimatorRef>(h)->ClipIndex, 0u);
    EXPECT_EQ(world.GetComponent<AnimatorRef>(h)->ClipIndex, ClipStore::Instance().GetIndexIfPresent(ClipGuid));
}

TEST_F(EmbeddedAnimationPersistence, ExplicitPauseAndStopSupersedePendingAutoplay)
{
    ModelPath = WriteModel("source");
    AssetManager assets;
    auto model = MakeShared<PendingModel>(ModelGuid, ModelPath);
    model->MarkPending();
    assets.RegisterLoadedAsset(ModelGuid, model);
    ECS::World world;
    const auto paused = PlaybackEntity(world, SourceAnimator(), GUID::Generate());
    const auto stopped = PlaybackEntity(world, SourceAnimator(), GUID::Generate());
    BootstrapSceneAnimators(world, assets);
    ASSERT_EQ(world.GetComponent<Animator>(paused)->pendingCommand, AnimatorPlaybackCommand::Autoplay);
    ASSERT_EQ(world.GetComponent<Animator>(stopped)->pendingCommand, AnimatorPlaybackCommand::Autoplay);
    world.GetComponentForWrite<Animator>(paused)->Pause();
    world.GetComponentForWrite<Animator>(stopped)->Stop();
    ProcessClipAnimatorCommands(world, assets);
    ASSERT_TRUE(model->Load());
    ProcessClipAnimatorCommands(world, assets);
    for (const auto h : {paused, stopped})
    {
        EXPECT_EQ(world.GetComponent<Animator>(h)->pendingCommand, AnimatorPlaybackCommand::None);
        EXPECT_EQ(world.GetComponent<AnimatorRef>(h)->ClipIndex, 0u);
        EXPECT_TRUE(world.GetComponent<AnimatorRef>(h)->IsPaused());
    }
}

// A library whose clip is embedded in a model: the library names the clip by its derived GUID, so
// playback waits for the model that registers it instead of staying at rest for good.
namespace
{

SharedPtr<Animation::AnimationLibrary> LibraryOf(const GUID& clipGuid)
{
    const std::string json = R"({"schemaVersion":1,"assetType":"AnimationLibrary","animations":[{"name":"Motion","assetGuid":")"
        + clipGuid.ToString() + R"(","type":"Animation","loop":true}]})";
    auto library = MakeShared<Animation::AnimationLibrary>(GUID::Generate(), "embedded.animlib");
    EXPECT_TRUE(library->LoadFromData(Vector<uint8>(json.begin(), json.end())));
    return library;
}

SharedPtr<Animation::AnimationController> ControllerOf(const GUID& clipGuid)
{
    const std::string json = R"({"entryState":"Run","states":[{"name":"Run","motion":{"assetGuid":")"
        + clipGuid.ToString() + R"(","type":"Animation"}}]})";
    auto controller = MakeShared<Animation::AnimationController>(GUID::Generate(), "embedded.animcontroller");
    EXPECT_TRUE(controller->LoadFromData(Vector<uint8>(json.begin(), json.end())));
    return controller;
}

Animator ControllerAnimator(const GUID& controllerGuid)
{
    Animator animator;
    animator.source = AnimatorPlaybackSource::Controller;
    animator.controllerGuid.Set(controllerGuid);
    return animator;
}

Animator LibraryAnimator(const GUID& libraryGuid)
{
    Animator animator;
    animator.source = AnimatorPlaybackSource::Library;
    animator.libraryGuid.Set(libraryGuid);
    return animator;
}

} // namespace

// The runtime hosts (the Player) start a library or a controller through the same path as the
// editor's play mode: no editor code, and no "only Clip playback" refusal.
TEST_F(EmbeddedAnimationPersistence, RuntimeLibrarySourceStartsWithoutEditorCode)
{
    PublishClip();
    AssetManager assets;
    const auto library = LibraryOf(ClipGuid);
    assets.RegisterLoadedAsset(library->GetGUID(), library);
    ECS::World world;
    const auto h = PlaybackEntity(world, LibraryAnimator(library->GetGUID()), GUID::Generate());
    BootstrapSceneAnimators(world, assets);
    EXPECT_EQ(world.GetComponent<AnimatorRef>(h)->ClipIndex, ClipStore::Instance().GetIndexIfPresent(ClipGuid))
        << "the runtime start left a library-sourced Animator at rest";
    EXPECT_NE(world.GetComponent<AnimatorRef>(h)->ClipIndex, 0u);
}

TEST_F(EmbeddedAnimationPersistence, RuntimeControllerSourceStartsWithoutEditorCode)
{
    PublishClip();
    AssetManager assets;
    const auto controller = ControllerOf(ClipGuid);
    assets.RegisterLoadedAsset(controller->GetGUID(), controller);
    ECS::World world;
    const auto h = PlaybackEntity(world, ControllerAnimator(controller->GetGUID()), GUID::Generate());
    BootstrapSceneAnimators(world, assets);
    EXPECT_EQ(world.GetComponent<AnimatorRef>(h)->ClipIndex, ClipStore::Instance().GetIndexIfPresent(ClipGuid))
        << "the runtime start left a controller-sourced Animator at rest";
    EXPECT_NE(world.GetComponent<AnimatorRef>(h)->ClipIndex, 0u);
}

// The clip is embedded in the entity's own model, still loading at play start (the subtree model).
TEST_F(EmbeddedAnimationPersistence, RuntimeLibraryClipOfTheLoadingTargetStartsWhenTheTargetLands)
{
    ModelPath = WriteModel("target");
    AssetManager assets;
    auto model = MakeShared<PendingModel>(ModelGuid, ModelPath);
    model->MarkPending();
    assets.RegisterLoadedAsset(ModelGuid, model);
    const auto library = LibraryOf(ClipGuid);
    assets.RegisterLoadedAsset(library->GetGUID(), library);
    ECS::World world;
    const auto h = PlaybackEntity(world, LibraryAnimator(library->GetGUID()), ModelGuid);

    BootstrapSceneAnimators(world, assets);
    EXPECT_EQ(world.GetComponent<Animator>(h)->pendingCommand, AnimatorPlaybackCommand::Autoplay)
        << "the library start gave up on a clip whose model is still loading";
    ProcessClipAnimatorCommands(world, assets);
    EXPECT_EQ(world.GetComponent<AnimatorRef>(h)->ClipIndex, 0u);

    ASSERT_TRUE(model->Load());
    ProcessClipAnimatorCommands(world, assets);
    EXPECT_EQ(world.GetComponent<Animator>(h)->pendingCommand, AnimatorPlaybackCommand::None);
    EXPECT_NE(world.GetComponent<AnimatorRef>(h)->ClipIndex, 0u);
    EXPECT_EQ(world.GetComponent<AnimatorRef>(h)->ClipIndex, ClipStore::Instance().GetIndexIfPresent(ClipGuid));
}

TEST_F(EmbeddedAnimationPersistence, RuntimeControllerClipOfTheLoadingTargetStartsWhenTheTargetLands)
{
    ModelPath = WriteModel("target");
    AssetManager assets;
    auto model = MakeShared<PendingModel>(ModelGuid, ModelPath);
    model->MarkPending();
    assets.RegisterLoadedAsset(ModelGuid, model);
    const auto controller = ControllerOf(ClipGuid);
    assets.RegisterLoadedAsset(controller->GetGUID(), controller);
    ECS::World world;
    const auto h = PlaybackEntity(world, ControllerAnimator(controller->GetGUID()), ModelGuid);

    BootstrapSceneAnimators(world, assets);
    EXPECT_EQ(world.GetComponent<Animator>(h)->pendingCommand, AnimatorPlaybackCommand::Autoplay)
        << "the controller start gave up on a clip whose model is still loading";
    ProcessClipAnimatorCommands(world, assets);
    EXPECT_EQ(world.GetComponent<AnimatorRef>(h)->ClipIndex, 0u);

    ASSERT_TRUE(model->Load());
    ProcessClipAnimatorCommands(world, assets);
    EXPECT_EQ(world.GetComponent<Animator>(h)->pendingCommand, AnimatorPlaybackCommand::None);
    EXPECT_NE(world.GetComponent<AnimatorRef>(h)->ClipIndex, 0u);
    EXPECT_EQ(world.GetComponent<AnimatorRef>(h)->ClipIndex, ClipStore::Instance().GetIndexIfPresent(ClipGuid));
}

// The clip is embedded in another model, never loaded this session but journaled in the project's
// asset database: the registry names it as the clip's container, which loads, and playback starts.
TEST_F(EmbeddedAnimationPersistence, RuntimeLibraryClipOfAJournaledModelLoadsItAndStarts)
{
    const auto assetsRoot = Root / "Assets";
    std::filesystem::create_directories(assetsRoot);
    const auto written = WriteModel("clips");
    std::filesystem::rename(written, assetsRoot / "clips.gltf");
    std::filesystem::rename(Root / "animation.bin", assetsRoot / "animation.bin");

    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(assetsRoot, &pool, Root / "AssetDatabase.assetdb", Root / ".Cache" / "AssetDatabase"));
    assets.GetRegistry().WaitForStartupScan();
    const GUID clipsModel = assets.ResolveAssetGuid("clips.gltf");
    ASSERT_FALSE(clipsModel.IsNull());
    ASSERT_TRUE(assets.GetRegistry().RegisterSubassetDeriveKeys(assetsRoot / "clips.gltf", {"embedded:0"}));
    const GUID clip = ModelAsset::DeriveEmbeddedClipGuid(clipsModel, 0);
    ASSERT_FALSE(assets.IsAssetLoaded(clipsModel));

    const auto library = LibraryOf(clip);
    assets.RegisterLoadedAsset(library->GetGUID(), library);
    ECS::World world;
    const auto h = PlaybackEntity(world, LibraryAnimator(library->GetGUID()), GUID::Generate());
    BootstrapSceneAnimators(world, assets);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (world.GetComponent<AnimatorRef>(h)->ClipIndex == 0 && std::chrono::steady_clock::now() < deadline)
    {
        assets.Update();
        ProcessClipAnimatorCommands(world, assets);
    }
    EXPECT_NE(world.GetComponent<AnimatorRef>(h)->ClipIndex, 0u)
        << "the library's clip never resolved through its journaled container";
    EXPECT_EQ(world.GetComponent<AnimatorRef>(h)->ClipIndex, ClipStore::Instance().GetIndexIfPresent(clip));
    assets.Shutdown();
}

// The clip's container is journaled but its file is gone: the load fails, nothing is pending, so
// the start says once that the Animator has nothing to play, and stops trying.
TEST_F(EmbeddedAnimationPersistence, RuntimeLibraryClipOfAFailedContainerWarnsOnceAndStopsRetrying)
{
    const auto assetsRoot = Root / "Assets";
    std::filesystem::create_directories(assetsRoot);
    const auto written = WriteModel("gone");
    std::filesystem::rename(written, assetsRoot / "gone.gltf");
    std::filesystem::rename(Root / "animation.bin", assetsRoot / "animation.bin");

    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(assetsRoot, &pool, Root / "AssetDatabase.assetdb", Root / ".Cache" / "AssetDatabase"));
    assets.GetRegistry().WaitForStartupScan();
    const GUID goneModel = assets.ResolveAssetGuid("gone.gltf");
    ASSERT_FALSE(goneModel.IsNull());
    ASSERT_TRUE(assets.GetRegistry().RegisterSubassetDeriveKeys(assetsRoot / "gone.gltf", {"embedded:0"}));
    std::filesystem::remove(assetsRoot / "animation.bin");
    std::filesystem::remove(assetsRoot / "gone.gltf");

    const GUID goneClip = ModelAsset::DeriveEmbeddedClipGuid(goneModel, 0);
    const auto library = LibraryOf(goneClip);
    assets.RegisterLoadedAsset(library->GetGUID(), library);
    ECS::World world;
    const auto h = PlaybackEntity(world, LibraryAnimator(library->GetGUID()), GUID::Generate());

    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Warning);
        Logger::Log::Warning("capture live");
        BootstrapSceneAnimators(world, assets);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (world.GetComponent<Animator>(h)->pendingCommand != AnimatorPlaybackCommand::None &&
               std::chrono::steady_clock::now() < deadline)
        {
            assets.Update();
            ProcessClipAnimatorCommands(world, assets);
        }
        for (int frame = 0; frame < 3; ++frame)
            ProcessClipAnimatorCommands(world, assets);
        Logger::Log::Flush();
    }
    ASSERT_NE(std::find(lines.begin(), lines.end(), "capture live"), lines.end()) << "the log capture saw nothing";
    EXPECT_EQ(world.GetComponent<Animator>(h)->pendingCommand, AnimatorPlaybackCommand::None)
        << "the start kept retrying after the container failed to load";
    EXPECT_EQ(world.GetComponent<AnimatorRef>(h)->ClipIndex, 0u);
    const auto isWarning = [](const std::string& line)
    { return line.find("no clip to play from animation library") != std::string::npos; };
    EXPECT_EQ(std::count_if(lines.begin(), lines.end(), isWarning), 1)
        << "the unresolved library clip was not reported exactly once";
    const auto warning = std::find_if(lines.begin(), lines.end(), isWarning);
    ASSERT_NE(warning, lines.end());
    EXPECT_NE(warning->find("entry 'Motion' names clip " + goneClip.ToString()), std::string::npos)
        << "the warning must name the entry used and the clip that resolved to nothing: " << *warning;
    assets.Shutdown();
}

#if defined(EMBEDDED_ANIMATION_EDITOR_TESTS)
TEST_F(EmbeddedAnimationPersistence, EditorDefaultNamedAndModelHintWaitForLoadingTarget)
{
    ModelPath = WriteModel("target");
    AssetManager assets;
    auto model = MakeShared<PendingModel>(ModelGuid, ModelPath);
    model->MarkPending();
    assets.RegisterLoadedAsset(ModelGuid, model);
    ECS::World world;
    Animator defaultChoice;
    Animator named;
    named.SetAssignedAnimation("SourceMotion");
    Animator hint;
    hint.clipGuid.Set(ModelGuid); // Legacy model-as-clip hint.
    const auto first = PlaybackEntity(world, defaultChoice, ModelGuid);
    const auto second = PlaybackEntity(world, named, ModelGuid);
    const auto third = PlaybackEntity(world, hint, GUID::Generate());
    GameEngine::Editor::ApplyAnimatorOnEnterPlayMode(world, assets);
    for (const auto h : {first, second, third})
    {
        EXPECT_EQ(world.GetComponent<Animator>(h)->pendingCommand, AnimatorPlaybackCommand::Autoplay);
        EXPECT_EQ(world.GetComponent<AnimatorRef>(h)->ClipIndex, 0u);
    }
    ASSERT_TRUE(model->Load());
    ProcessClipAnimatorCommands(world, assets);
    const auto clip = ClipStore::Instance().GetIndexIfPresent(ClipGuid);
    ASSERT_NE(clip, 0u);
    for (const auto h : {first, second, third})
    {
        EXPECT_EQ(world.GetComponent<Animator>(h)->pendingCommand, AnimatorPlaybackCommand::None);
        EXPECT_EQ(world.GetComponent<AnimatorRef>(h)->ClipIndex, clip);
    }
}

TEST_F(EmbeddedAnimationPersistence, EditorEnterPlayLoadsColdAnimationSourceInsteadOfTargetClip)
{
    ModelPath = WriteModel("source");
    const auto targetPath = WriteModel("target");
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(Root));
    ASSERT_TRUE(assets.GetRegistry().RegisterAsset(ModelPath));
    ASSERT_TRUE(assets.GetRegistry().RegisterAsset(targetPath));
    ModelGuid = assets.GetRegistry().GetAssetGUID(ModelPath);
    ClipGuid = ModelAsset::DeriveEmbeddedClipGuid(ModelGuid, 0);
    const auto targetGuid = assets.GetRegistry().GetAssetGUID(targetPath);
    ASSERT_EQ(ClipStore::Instance().GetIndexIfPresent(ClipGuid), 0u);
    ECS::World world;
    const auto h = PlaybackEntity(world, SourceAnimator(), targetGuid);
    GameEngine::Editor::ApplyAnimatorOnEnterPlayMode(world, assets);
    const auto selected = ClipStore::Instance().GetIndexIfPresent(ClipGuid);
    ASSERT_NE(selected, 0u);
    EXPECT_EQ(world.GetComponent<AnimatorRef>(h)->ClipIndex, selected);
    auto invalid = SourceAnimator();
    invalid.clipSourceAnimationIndex = 999;
    invalid.clipGuid.Set(ModelAsset::DeriveEmbeddedClipGuid(ModelGuid, 999));
    const auto bad = PlaybackEntity(world, invalid, targetGuid);
    GameEngine::Editor::ApplyAnimatorOnEnterPlayMode(world, assets);
    EXPECT_EQ(world.GetComponent<AnimatorRef>(bad)->ClipIndex, 0u);
}

TEST_F(EmbeddedAnimationPersistence, EditorPendingSourceUsesRuntimeRetryWithoutNewInput)
{
    ModelPath = WriteModel("source");
    AssetManager assets;
    auto model = MakeShared<PendingModel>(ModelGuid, ModelPath);
    model->MarkPending();
    assets.RegisterLoadedAsset(ModelGuid, model);
    ECS::World world;
    auto authored = SourceAnimator();
    authored.sectionStartSeconds = 0.2f;
    authored.sectionEndSeconds = 0.8f;
    const auto h = PlaybackEntity(world, authored, GUID::Generate());
    GameEngine::Editor::ApplyAnimatorOnEnterPlayMode(world, assets);
    EXPECT_EQ(world.GetComponent<Animator>(h)->pendingCommand, AnimatorPlaybackCommand::Autoplay);
    ASSERT_TRUE(model->Load());
    ProcessClipAnimatorCommands(world, assets);
    EXPECT_EQ(world.GetComponent<Animator>(h)->pendingCommand, AnimatorPlaybackCommand::None);
    EXPECT_NE(world.GetComponent<AnimatorRef>(h)->ClipIndex, 0u);
    EXPECT_EQ(world.GetComponent<AnimatorRef>(h)->ClipIndex, ClipStore::Instance().GetIndexIfPresent(ClipGuid));
    EXPECT_FLOAT_EQ(world.GetComponent<AnimatorRef>(h)->Time, authored.sectionStartSeconds);
    EXPECT_NE(world.GetComponent<AnimatorRef>(h)->Flags & AnimatorRef::kFlag_Section, 0u);
}
#endif
