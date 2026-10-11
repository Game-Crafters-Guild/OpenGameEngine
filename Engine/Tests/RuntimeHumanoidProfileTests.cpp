#include "Animation/HumanoidImport.h"
#include "Animation/HumanoidRig.h"
#include "Animation/SkeletonProfile.h"
#include "AssetCore/AssetEvents.h"
#include "AssetDatabase/AssetStore_TextJsonl.h"
#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Assets/RuntimeHumanoidProfile.h"
#include "Components/Animation/AnimatorRef.h"
#include "Components/Animation/HumanoidRetargeterComponent.h"
#include "Components/Animation/SkeletonRef.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "ECSModules/Rendering/Systems/AnimationSystem.h"
#include "Engine/Build/AssetCollector.h"
#include "Engine/Build/PackagedGameLayout.h"
#include "Engine/Rendering/RenderServices.h"
#include "EngineLogCapture.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <numeric>
#include <sstream>
#include <thread>

using namespace GameEngine;
namespace fs = std::filesystem;
namespace Renderer = GameEngine::Engine::Renderer;

namespace
{
constexpr const char* relativeProfile = "SkeletonProfiles/HumanoidStandard.profile.json";

class RuntimeHumanoidProfileTest : public testing::Test
{
  protected:
    fs::path root;
    fs::path previousCwd;
    AssetManager* assets = nullptr;
    Animation::SkeletonProfile reference{GUID(), fs::path(RUNTIME_PROFILE_FIXTURE)};

    void SetUp() override
    {
        previousCwd = fs::current_path();
        ASSERT_TRUE(reference.Load());
        // Spell the root as the engine does. EngineCore::Initialize canonicalizes its workspace,
        // so the project mount is the real path; on macOS the temp directory is under /var, a
        // symlink to /private/var. A path spelled through the symlink is outside that mount,
        // and the registry gives it a fresh GUID instead of the project's derived identity.
        root = fs::canonical(fs::temp_directory_path()) / ("runtime-humanoid-" + GUID::Generate().ToString());
        fs::create_directories(root / "project");
        ScriptsConfig scripts;
        scripts.disableClr = true;
        scripts.enableHotReload = false;
        scripts.enableAutoProjectGeneration = false;
        auto& engine = EngineCore::GetInstance();
        engine.SetScriptsConfig(scripts);
        ApplicationConfig config;
        config.WorkspaceDirectory = root.string();
        config.AssetDirectory = "project";
        ASSERT_TRUE(engine.Initialize(config));
        assets = &engine.GetAssetManager();
        ASSERT_EQ(assets->GetAssetRoot(), root / "project")
            << "the fixture must spell its paths as the engine's project mount";
    }
    void TearDown() override
    {
        EngineCore::GetInstance().Shutdown();
        std::error_code ec;
        fs::current_path(previousCwd, ec);
        fs::remove_all(root, ec);
    }
    void Mount(const char* alias, const fs::path& path)
    {
        fs::create_directories(path);
        AssetSourceDesc source;
        source.Alias = alias;
        source.Root = path;
        source.RequiresScan = false;
        source.RegisterFileWatcher = false;
        ASSERT_TRUE(assets->RegisterSource(source));
    }
    fs::path Stage(const fs::path& path)
    {
        const auto file = path / relativeProfile;
        fs::create_directories(file.parent_path());
        fs::copy_file(reference.GetPath(), file);
        return file;
    }
    uint32 Skeleton(const fs::path& path)
    {
        const auto count = static_cast<uint32>(reference.Bones().size());
        auto& store = Renderer::SkeletonStore::Instance();
        const auto id = store.CreateSkeleton(count);
        Animation::SkeletonData data;
        data.BoneCount = count;
        data.SourceModelPath = path;
        for (const auto& bone : reference.Bones())
        {
            data.BoneNames.emplace_back(Animation::HumanBoneToString(bone.Bone));
            int32 parent = -1;
            for (size_t i = 0; i < data.BoneNames.size() - 1; ++i)
                if (reference.Bones()[i].Bone == bone.Parent)
                    parent = static_cast<int32>(i);
            data.Parent.push_back(parent);
            data.RestTranslation.insert(data.RestTranslation.end(),
                                        {bone.RestTranslation.x, bone.RestTranslation.y, bone.RestTranslation.z});
            data.RestRotation.insert(data.RestRotation.end(), {0, 0, 0, 1});
            data.RestScale.insert(data.RestScale.end(), {1, 1, 1});
        }
        data.SkinJointCount = count;
        data.JointNodes.resize(count);
        std::iota(data.JointNodes.begin(), data.JointNodes.end(), 0);
        data.BindPose.resize(count * 16);
        data.InverseBind.resize(count * 16);
        for (uint32 b = 0; b < count; ++b)
            for (uint32 k : {0u, 5u, 10u, 15u})
                data.BindPose[b * 16 + k] = data.InverseBind[b * 16 + k] = 1;
        data.BuildBoneNameLookup();
        *store.Get(id) = std::move(data);
        return id;
    }
    void Import(const fs::path& modelPath, uint32 skeleton)
    {
        ModelAsset model(GUID::Generate(), modelPath);
        model.SetSkeletonIdForTest(skeleton);
        AssetManager::ScopedThreadAssetManager context(assets);
        model.PostLoad();
    }
    ECS::EntityHandle Actor(ECS::World& world)
    {
        const auto source = root / "project/source.fbx";
        const auto target = root / "project/target.fbx";
        std::ofstream(source) << "fixture";
        const auto sourceId = Skeleton(source);
        const auto targetId = Skeleton(target);
        auto sourceModel = std::make_shared<ModelAsset>(assets->ResolveAssetGuid(source), source);
        sourceModel->SetMeshesForTest({});
        sourceModel->SetSkeletonIdForTest(sourceId);
        assets->RegisterLoadedAsset(sourceModel->GetGUID(), sourceModel);
        for (const auto& pair : {std::pair(source, sourceId), std::pair(target, targetId)})
        {
            Animation::HumanoidRig rig(GUID(), {});
            EXPECT_TRUE(Animation::AutoImportHumanoidRig(
                *Renderer::SkeletonStore::Instance().Get(pair.second), reference, rig));
            auto sidecar = pair.first;
            sidecar.replace_extension(".humanoidrig.json");
            EXPECT_TRUE(rig.SaveToPath(sidecar));
        }
        auto clip = std::make_shared<AnimationClip>(GUID::Generate(), source);
        AnimChannel channel{};
        channel.boneIndex = 0; // Source ordering deliberately differs from target Spine=1.
        channel.targetName = "Spine";
        channel.targetNameId = HashStringId("Spine");
        channel.path = AnimPath::Rotation;
        AnimKeyframe key{};
        key.rotation[3] = 1;
        channel.keys.push_back(key);
        clip->SetChannelsAndDurationForTest({channel}, 1);
        Components::AnimatorRef animator;
        animator.ClipIndex = Renderer::ClipStore::Instance().RegisterRuntimeClip(clip->GetGUID(), clip);
        Components::SkeletonRef skeleton;
        skeleton.skeletonId = targetId;
        skeleton.runtimeId = Renderer::SkeletonStore::Instance().CreateRuntime(targetId);
        return world.Create(animator, skeleton).GetHandle();
    }
};

TEST_F(RuntimeHumanoidProfileTest, ModelPostLoadUsesProjectProfileWithoutEditorMount)
{
    const auto profilePath = Stage(root / "project");
    const auto modelPath = root / "project/model.fbx";
    const auto expected = assets->ResolveAssetGuid(profilePath, "project");
    ASSERT_FALSE(expected.IsNull());
    Import(modelPath, Skeleton(modelPath));
    Animation::HumanoidRig rig(GUID(), root / "project/model.humanoidrig.json");
    ASSERT_TRUE(rig.Load());
    EXPECT_EQ(rig.ProfileRef(), expected);
    EXPECT_TRUE(rig.AutoGenerated());
}

// An import of an unchanged model derives the sidecar it already has. PostLoad
// must not write it again: the write reports a change to the file watcher (the
// Assets panel lists the folder again) and touches a file the project commits.
TEST_F(RuntimeHumanoidProfileTest, UnchangedSidecarIsNotRewritten)
{
    Stage(root / "project");
    const auto modelPath = root / "project/model.fbx";
    const uint32 skeleton = Skeleton(modelPath);
    Import(modelPath, skeleton);
    const auto sidecar = root / "project/model.humanoidrig.json";
    ASSERT_TRUE(fs::exists(sidecar));

    // Back-dated, so that a rewrite shows in the modification time.
    const auto stamped = fs::last_write_time(sidecar) - std::chrono::hours(1);
    fs::last_write_time(sidecar, stamped);
    Import(modelPath, skeleton);
    EXPECT_EQ(fs::last_write_time(sidecar), stamped) << "PostLoad rewrote an unchanged humanoid sidecar";
}

TEST_F(RuntimeHumanoidProfileTest, ModelPostLoadRetainsInstalledProfileIdentity)
{
    const auto profilePath = Stage(root / "runtime");
    Mount("editor", root / "runtime");
    const auto expected = assets->ResolveAssetGuid(profilePath, "editor");
    ASSERT_FALSE(expected.IsNull());
    const auto modelPath = root / "project/model.fbx";
    Import(modelPath, Skeleton(modelPath));
    Animation::HumanoidRig rig(GUID(), root / "project/model.humanoidrig.json");
    ASSERT_TRUE(rig.Load());
    EXPECT_EQ(rig.ProfileRef(), expected);
}

TEST_F(RuntimeHumanoidProfileTest, PackagedManifestRetainsCollectedProfileIdentity)
{
    Stage(root / "runtime");
    Mount("editor", root / "runtime");
    AssetCollector collector(*assets);
    AssetManifest manifest;
    ASSERT_TRUE(collector.CollectRuntimeHumanoidProfile(manifest));
    ASSERT_TRUE(collector.CollectRuntimeHumanoidProfile(manifest));
    ASSERT_EQ(manifest.entries.size(), 1u);
    const auto& entry = manifest.entries.front();
    EXPECT_EQ(entry.outputPath.generic_string(), "Assets/SkeletonProfiles/HumanoidStandard.profile.json");
    EXPECT_EQ(entry.type, AssetType::SkeletonProfile);
    const auto packageRoot = root / "export/Assets";
    fs::create_directories((root / "export" / entry.outputPath).parent_path());
    fs::copy_file(entry.sourcePath, root / "export" / entry.outputPath);
    AssetDatabase::AssetStore_TextJsonl store(nullptr);
    AssetDatabase::AssetRecord row;
    row.guid = entry.guid;
    row.path = relativeProfile;
    row.type = entry.type;
    ASSERT_TRUE(store.UpsertAsset(row, nullptr));
    ASSERT_TRUE(store.SaveToFile(packageRoot / ".assetmanifest", nullptr));
    EXPECT_FALSE(BuildPlayerEditorSourceDesc(packageRoot, packageRoot));
    AssetManager packaged;
    ASSERT_TRUE(packaged.Initialize());
    ASSERT_TRUE(packaged.RegisterSource(MakePackageMount("project", packageRoot)));
    const auto profile = LoadRuntimeHumanoidProfile(packaged);
    ASSERT_TRUE(profile);
    EXPECT_EQ(profile->GetGUID(), entry.guid);
    ModelAsset model(GUID::Generate(), packageRoot / "model.fbx");
    model.SetSkeletonIdForTest(Skeleton(model.GetPath()));
    {
        AssetManager::ScopedThreadAssetManager context(&packaged);
        model.PostLoad();
    }
    Animation::HumanoidRig rig(GUID(), packageRoot / "model.humanoidrig.json");
    ASSERT_TRUE(rig.Load());
    EXPECT_EQ(rig.ProfileRef(), entry.guid);
    packaged.Shutdown();
}

TEST_F(RuntimeHumanoidProfileTest, InstalledDefaultDoesNotAdoptProjectShadowOrOverwriteAuthoredRig)
{
    Stage(root / "project");
    Stage(root / "runtime");
    Mount("editor", root / "runtime");
    const auto profile = LoadRuntimeHumanoidProfile(*assets);
    ASSERT_TRUE(profile);
    EXPECT_EQ(profile->GetGUID(), assets->ResolveAssetGuid(root / "runtime" / relativeProfile, "editor"));
    Animation::HumanoidRig original(GUID(), root / "project/model.humanoidrig.json");
    ASSERT_TRUE(Animation::AutoImportHumanoidRig(
        *Renderer::SkeletonStore::Instance().Get(Skeleton(root / "project/model.fbx")), reference, original));
    original.SetProfileRef(GUID::Generate());
    original.SetAutoGenerated(false);
    ASSERT_TRUE(original.SaveToPath(original.GetPath()));
    Vector<uint8> before;
    ASSERT_TRUE(original.SaveToData(before));
    const auto modelPath = root / "project/model.fbx";
    Import(modelPath, Skeleton(modelPath));
    Animation::HumanoidRig after(GUID(), original.GetPath());
    ASSERT_TRUE(after.Load());
    Vector<uint8> actual;
    ASSERT_TRUE(after.SaveToData(actual));
    EXPECT_EQ(actual, before);
}

TEST_F(RuntimeHumanoidProfileTest, AnimationFailureIsBoundedAndProfileCreationRearmsBootstrap)
{
    ECS::World world(nullptr);
    const auto actor = Actor(world);
    Renderer::RenderServices rendering;
    Renderer::AnimationSystem system(&rendering);
    std::vector<std::string> lines;
    TestLog::ScopedEngineLogCapture capture(&lines);
    Logger::Log::Info("capture control");
    for (int i = 0; i < 8; ++i)
    {
        system.Update(world, .016f);
        world.ProcessCommands();
    }
    Logger::Log::Flush();
    ASSERT_EQ(TestLog::CountLinesContaining(lines, "capture control"), 1u);
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "profile unavailable"), 1u);
    EXPECT_FALSE(world.HasComponent<Components::HumanoidRetargeterComponent>(actor));
    assets->GetEventDispatcher().DispatchEvent(AssetEvents::AssetModified(
        GUID::Generate(), AssetType::Texture, "unrelated.png"));
    system.Update(world, .016f);
    world.ProcessCommands();
    Logger::Log::Flush();
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "profile unavailable"), 1u);
    const auto profilePath = root / "project" / relativeProfile;
    auto write = assets->ExpectWrite(profilePath);
    Stage(root / "project");
    write.Report(profilePath);
    const auto snapshot = world.SerializeWorld();
    system.Update(world, .016f);
    world.ProcessCommands();
    Logger::Log::Flush();
    EXPECT_TRUE(world.HasComponent<Components::HumanoidRetargeterComponent>(actor))
        << TestLog::FirstLineContaining(lines, "unresolved") << TestLog::FirstLineContaining(lines, "failed");
    // Play snapshots remove auto-added components. Success must not be cached per pair.
    world.DeserializeWorld(snapshot);
    ASSERT_TRUE(world.IsValid(actor));
    ASSERT_FALSE(world.HasComponent<Components::HumanoidRetargeterComponent>(actor));
    system.Update(world, .016f);
    world.ProcessCommands();
    EXPECT_TRUE(world.HasComponent<Components::HumanoidRetargeterComponent>(actor));
}

TEST_F(RuntimeHumanoidProfileTest, MountChangeRearmsFailedPair)
{
    ECS::World world(nullptr);
    const auto actor = Actor(world);
    Renderer::RenderServices rendering;
    Renderer::AnimationSystem system(&rendering);
    system.Update(world, .016f);
    world.ProcessCommands();
    Stage(root / "runtime");
    Mount("editor", root / "runtime");
    system.Update(world, .016f);
    world.ProcessCommands();
    EXPECT_TRUE(world.HasComponent<Components::HumanoidRetargeterComponent>(actor));
}
TEST_F(RuntimeHumanoidProfileTest, WorldResetRearmsFailedPairWithoutAssetEvent)
{
    ECS::World world(nullptr);
    const auto actor = Actor(world);
    const auto snapshot = world.SerializeWorld();
    Renderer::RenderServices rendering;
    Renderer::AnimationSystem system(&rendering);
    system.Update(world, .016f);
    world.ProcessCommands();
    Stage(root / "project");
    world.DeserializeWorld(snapshot);
    ASSERT_TRUE(world.IsValid(actor));
    system.Update(world, .016f);
    world.ProcessCommands();
    EXPECT_TRUE(world.HasComponent<Components::HumanoidRetargeterComponent>(actor));
}

TEST_F(RuntimeHumanoidProfileTest, MissingRuntimeProfileCannotEnterExportManifest)
{
    AssetCollector collector(*assets);
    AssetManifest manifest;
    EXPECT_FALSE(collector.CollectRuntimeHumanoidProfile(manifest));
    EXPECT_TRUE(manifest.entries.empty());
}

TEST_F(RuntimeHumanoidProfileTest, MissingSourceRigIsBoundedAndReportedRepairRearms)
{
    Stage(root / "project");
    ECS::World world(nullptr);
    const auto actor = Actor(world);
    const auto sidecar = root / "project/source.humanoidrig.json";
    const auto saved = root / "source-rig.saved";
    fs::rename(sidecar, saved);
    Renderer::RenderServices rendering;
    Renderer::AnimationSystem system(&rendering);
    std::vector<std::string> lines;
    TestLog::ScopedEngineLogCapture capture(&lines);
    for (int i = 0; i < 4; ++i)
    {
        system.Update(world, .016f);
        world.ProcessCommands();
    }
    Logger::Log::Flush();
    const auto failures = TestLog::CountLinesContaining(lines, "source rig unresolved");
    ASSERT_GT(failures, 0u);
    for (int i = 0; i < 8; ++i)
    {
        system.Update(world, .016f);
        world.ProcessCommands();
    }
    Logger::Log::Flush();
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "source rig unresolved"), failures);
    auto write = assets->ExpectWrite(sidecar);
    fs::rename(saved, sidecar);
    write.Report(sidecar);
    system.Update(world, .016f);
    world.ProcessCommands();
    EXPECT_TRUE(world.HasComponent<Components::HumanoidRetargeterComponent>(actor));
}

TEST_F(RuntimeHumanoidProfileTest, AnotherWorldRearmsSameSkeletonAndClipPair)
{
    ECS::World first(nullptr);
    const auto actor = Actor(first);
    ECS::World next(nullptr);
    const auto nextActor = next.Create(
                                   *first.GetComponent<Components::AnimatorRef>(actor),
                                   *first.GetComponent<Components::SkeletonRef>(actor))
                               .GetHandle();
    Renderer::RenderServices rendering;
    Renderer::AnimationSystem system(&rendering);
    system.Update(first, .016f);
    first.ProcessCommands();
    Stage(root / "project");
    system.Update(next, .016f);
    next.ProcessCommands();
    EXPECT_TRUE(next.HasComponent<Components::HumanoidRetargeterComponent>(nextActor));
}

TEST_F(RuntimeHumanoidProfileTest, ExportRejectsProjectProfileCollisionWithoutOverwritingItsEntry)
{
    const auto authoredPath = Stage(root / "project");
    const auto authoredGuid = assets->ResolveAssetGuid(authoredPath, "project");
    Stage(root / "runtime");
    Mount("editor", root / "runtime");
    AssetCollector collector(*assets);
    auto manifest = collector.CollectAllAssets();
    ASSERT_EQ(manifest.entries.size(), 1u);
    const auto originalPath = manifest.entries.front().sourcePath;
    EXPECT_FALSE(collector.CollectRuntimeHumanoidProfile(manifest));
    ASSERT_EQ(manifest.entries.size(), 1u);
    EXPECT_EQ(manifest.entries.front().guid, authoredGuid);
    EXPECT_EQ(manifest.entries.front().sourcePath, originalPath);
}

TEST_F(RuntimeHumanoidProfileTest, ColdProfileDoesNotParkTheOnlyModelWorker)
{
    const auto profilePath = Stage(root / "runtime");
    auto pool = std::make_unique<JobSystem::WorkStealingThreadPool>(1);
    auto workerAssets = std::make_unique<AssetManager>();
    ASSERT_TRUE(workerAssets->Initialize(pool.get()));
    AssetSourceDesc source;
    source.Alias = "editor";
    source.Root = root / "runtime";
    source.RequiresScan = false;
    source.RegisterFileWatcher = false;
    ASSERT_TRUE(workerAssets->RegisterSource(source));
    const auto profileGuid = workerAssets->ResolveAssetGuid(profilePath, "editor");
    ASSERT_FALSE(profileGuid.IsNull());
    ASSERT_FALSE(workerAssets->GetAsset(profileGuid));
    const auto modelPath = root / "runtime/model.fbx";
    auto model = std::make_shared<ModelAsset>(GUID::Generate(), modelPath);
    model->SetSkeletonIdForTest(Skeleton(modelPath));
    auto completed = std::make_shared<std::promise<void>>();
    auto completion = completed->get_future();
    auto task = pool->Submit([model, manager = workerAssets.get(), completed]()
                             {
        AssetManager::ScopedThreadAssetManager context(manager);
        model->PostLoad();
        completed->set_value(); }, JobSystem::JobPriority::Background);
    // An external future wait cannot run queued jobs and mask pool starvation.
    const auto result = completion.wait_for(std::chrono::seconds(5));
    if (result != std::future_status::ready)
    {
        // Keep the blocked worker's captures alive on a failing implementation;
        // joining the wedged pool would hide the assertion behind a hung test.
        (void)workerAssets.release();
        (void)pool.release();
    }
    ASSERT_EQ(result, std::future_status::ready)
        << "model PostLoad parked the only decoder while waiting for its profile decode";
    Animation::HumanoidRig rig(GUID(), root / "runtime/model.humanoidrig.json");
    ASSERT_TRUE(rig.Load());
    EXPECT_EQ(rig.ProfileRef(), profileGuid);
    workerAssets->Shutdown();
}

TEST_F(RuntimeHumanoidProfileTest, ProfileSnapshotIsPrivateAndSurvivesResidentUnload)
{
    const auto path = Stage(root / "project");
    const auto guid = assets->ResolveAssetGuid(path, "project");
    auto snapshot = LoadRuntimeHumanoidProfile(*assets);
    ASSERT_TRUE(snapshot);
    EXPECT_FALSE(assets->GetAsset(guid));
    auto resident = std::dynamic_pointer_cast<Animation::SkeletonProfile>(
        assets->LoadAssetAsync(guid, AssetLoadPriority::High).get());
    ASSERT_TRUE(resident);
    auto second = LoadRuntimeHumanoidProfile(*assets);
    ASSERT_TRUE(second);
    EXPECT_NE(second.get(), resident.get());
    const auto boneCount = snapshot->Bones().size();
    ASSERT_GT(boneCount, 0u);
    resident->Unload();
    EXPECT_TRUE(resident->Bones().empty());
    EXPECT_EQ(snapshot->Bones().size(), boneCount);
    EXPECT_EQ(second->Bones().size(), boneCount);
    EXPECT_EQ(snapshot->GetGUID(), guid);
    EXPECT_EQ(second->GetGUID(), guid);
}
} // namespace
