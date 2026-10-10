// The project material warm-up's lifetime and project-switch contract, driven
// through the service's own pump with an injected pool and clock. The material
// system is never initialized, so a warm-up batch compiles nothing; the
// world-pass keyword resolver, which the pump calls once per submitted batch,
// counts the batches.

#include <gtest/gtest.h>

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/ImportedMaterialCache.h"
#include "Assets/ModelAsset.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MaterialPrewarmVariants.h"
#include "Engine/Rendering/ModelMaterialBridge.h"
#include "Engine/Rendering/ProjectMaterialPrewarmService.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TestTempDir.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace
{
using Clock = std::chrono::steady_clock;

struct TestProject
{
    fs::path Root;
    std::unique_ptr<AssetManager> Assets;
};

class ProjectMaterialPrewarmServiceTest : public ::testing::Test
{
protected:
    void TearDown() override
    {
        for (TestProject* project : {&m_ProjectA, &m_ProjectB})
        {
            if (project->Assets)
                project->Assets->Shutdown();
            project->Assets.reset();
            std::error_code ec;
            if (!project->Root.empty())
                fs::remove_all(project->Root, ec);
        }
    }

    // A project whose one model carries a derived-cache record of
    // `recordedMaterials` materials, plus `unrecordedModels` models without one.
    void MakeProject(TestProject& project, const char* name, int recordedMaterials, int unrecordedModels = 0)
    {
        project.Root = TestUtils::MakeUniqueTempDirectory(name);
        std::error_code ec;
        fs::create_directories(project.Root / "Assets" / "Models", ec);
        const fs::path modelPath = project.Root / "Assets" / "Models" / "recorded.glb";
        std::ofstream(modelPath, std::ios::binary) << "model bytes";
        for (int i = 0; i < unrecordedModels; ++i)
            std::ofstream(project.Root / "Assets" / "Models" / ("unrecorded" + std::to_string(i) + ".glb"),
                          std::ios::binary)
                << "model bytes";

        project.Assets = std::make_unique<AssetManager>();
        ASSERT_TRUE(project.Assets->Initialize(project.Root / "Assets", &m_Pool,
                                               project.Root / "AssetDatabase.assetdb",
                                               project.Root / ".Cache" / "AssetDatabase"));
        project.Assets->WaitForStartupScan();
        AssetRegistry& registry = project.Assets->GetRegistry();
        AssetMetadata model;
        ASSERT_TRUE(registry.TryGetAssetMetadata(modelPath, model));
        ASSERT_EQ(model.Type, AssetType::Model);

        Vector<ImportedMaterialData> materials(static_cast<size_t>(recordedMaterials));
        for (int i = 0; i < recordedMaterials; ++i)
        {
            materials[i].Name = std::string(name) + "_material" + std::to_string(i);
            materials[i].DiffuseColor[3] = 1.0f;
        }
        const auto record = ImportedMaterialCacheFile(registry, modelPath, model.Guid);
        ASSERT_TRUE(record.has_value());
        ASSERT_EQ(WriteImportedMaterialCache(*record, materials), ImportedMaterialCacheWrite::Written);
    }

    void Update(ProjectMaterialPrewarmService& service, const TestProject& project)
    {
        service.Update(*project.Assets, m_Materials, m_Pool, project.Root, m_Now, m_Resolver);
    }

    // Pumps frames until `done` holds. The injected clock moves a second per
    // frame, so every idle delay has elapsed by the next one; the real sleep
    // lets the listing task run.
    bool PumpUntil(ProjectMaterialPrewarmService& service, const TestProject& project,
                   const std::function<bool()>& done)
    {
        for (int frame = 0; frame < 2000; ++frame)
        {
            Update(service, project);
            if (done())
                return true;
            m_Now += 1s;
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    void PumpFrames(ProjectMaterialPrewarmService& service, const TestProject& project, int frames)
    {
        for (int frame = 0; frame < frames; ++frame)
        {
            Update(service, project);
            m_Now += 1s;
            std::this_thread::sleep_for(1ms);
        }
    }

    JobSystem::WorkStealingThreadPool m_Pool{2};
    MaterialSystem m_Materials;
    int m_WarmedBatches = 0;
    std::function<Rendering::MaterialKeyword()> m_Resolver = [this] {
        ++m_WarmedBatches;
        return Rendering::MaterialKeyword::None;
    };
    Clock::time_point m_Now = Clock::now();
    TestProject m_ProjectA;
    TestProject m_ProjectB;
};
} // namespace

// Opening another project mid-warm-up drops what is left of the first project's
// queue: afterwards only the new project's materials are warmed.
TEST_F(ProjectMaterialPrewarmServiceTest, ProjectSwitchWarmsOnlyTheNewProject)
{
    MakeProject(m_ProjectA, "ge_prewarm_switch_a", 3);
    MakeProject(m_ProjectB, "ge_prewarm_switch_b", 2);

    ProjectMaterialPrewarmService service;
    ASSERT_TRUE(PumpUntil(service, m_ProjectA, [this] { return m_WarmedBatches == 1; }));
    ASSERT_TRUE(PumpUntil(service, m_ProjectB, [this] { return m_WarmedBatches == 3; }));
    PumpFrames(service, m_ProjectB, 200);
    EXPECT_EQ(m_WarmedBatches, 3) << "the first project's remaining materials were warmed after the switch";
}

// A listing taken for the project that was open when it started is discarded when
// the project changes before its result is read, so the closed project's
// materials never reach the queue.
TEST_F(ProjectMaterialPrewarmServiceTest, ListingOfAClosedProjectIsDiscarded)
{
    MakeProject(m_ProjectA, "ge_prewarm_epoch_a", 3);
    MakeProject(m_ProjectB, "ge_prewarm_epoch_b", 0);
    const auto startListing = [this](ProjectMaterialPrewarmService& service) {
        Update(service, m_ProjectA);  // the project opens: an idle delay starts
        m_Now += 3s;
        Update(service, m_ProjectA);  // the delay has passed: the listing is submitted
        std::this_thread::sleep_for(200ms);
    };

    // Control: with no switch, the same steps list and warm the project.
    {
        ProjectMaterialPrewarmService control;
        startListing(control);
        ASSERT_TRUE(PumpUntil(control, m_ProjectA, [this] { return m_WarmedBatches == 3; }));
    }

    m_WarmedBatches = 0;
    ProjectMaterialPrewarmService service;
    startListing(service);
    PumpFrames(service, m_ProjectB, 200);
    EXPECT_EQ(m_WarmedBatches, 0) << "the closed project's listing reached the queue";
}

// Destroying the service waits for a listing in flight: the listing reads the
// asset registry, which the engine destroys right after the renderer.
TEST_F(ProjectMaterialPrewarmServiceTest, DestructionWaitsForTheListingInFlight)
{
    MakeProject(m_ProjectA, "ge_prewarm_join", 0, 3000);

    // The listing's own work, timed on this thread: the instrument below only
    // discriminates when a listing takes much longer than a destructor that
    // does not wait.
    const AssetRegistry& registry = m_ProjectA.Assets->GetRegistry();
    const auto calibrationStart = Clock::now();
    for (const AssetIndexRecord& row : registry.GetAssetIndexSnapshot())
    {
        if (const auto record = ImportedMaterialCacheFile(registry, row.Path, row.Guid))
            (void)ReadImportedMaterialCache(*record);
    }
    const auto listingCost = Clock::now() - calibrationStart;
    ASSERT_GE(listingCost, 20ms) << "a listing is too fast for this instrument; add models";

    auto service = std::make_unique<ProjectMaterialPrewarmService>();
    Update(*service, m_ProjectA);
    m_Now += 3s;
    Update(*service, m_ProjectA);  // submits the listing
    const auto destroyStart = Clock::now();
    service.reset();
    const auto destroyCost = Clock::now() - destroyStart;
    EXPECT_GE(destroyCost, listingCost / 10)
        << "the destructor returned while the listing was still reading the registry";
}

// A deleted model's record is deleted with it, so records do not outlive the
// models they describe.
TEST_F(ProjectMaterialPrewarmServiceTest, DeletedModelTakesItsRecordWithIt)
{
    MakeProject(m_ProjectA, "ge_prewarm_delete", 1);
    AssetRegistry& registry = m_ProjectA.Assets->GetRegistry();
    const fs::path modelPath = m_ProjectA.Root / "Assets" / "Models" / "recorded.glb";
    const GUID modelGuid = registry.GetAssetGUID(modelPath);
    const auto record = ImportedMaterialCacheFile(registry, modelPath, modelGuid);
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(fs::exists(*record));

    ProjectMaterialPrewarmService service;
    Update(service, m_ProjectA);
    m_ProjectA.Assets->GetEventDispatcher().DispatchEvent(
        AssetEvent(AssetEventType::AssetDestroyed, modelGuid, AssetType::Model, modelPath.string()));
    EXPECT_FALSE(fs::exists(*record));
}

// Recorded FBX materials pass through the same bridge and variant enumerator as
// the project's warm-up. A cache round trip must not restore vertex-color depth
// variants that the live import deliberately excludes.
TEST(ProjectMaterialPrewarm, CachedVertexColorPolicyMatchesLiveImportVariants)
{
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_prewarm_cached_vertex_color");
    const fs::path file = root / "model.json";
    Vector<ImportedMaterialData> imported(2);
    imported[0].IgnoresVertexColor = true;
    imported[1].IgnoresVertexColor = false;
    for (auto& material : imported)
    {
        material.AlphaMode = AlphaMode::Mask;
        material.DiffuseTexture = "__embedded:0";
    }
    ASSERT_EQ(WriteImportedMaterialCache(file, imported), ImportedMaterialCacheWrite::Written);
    const auto cached = ReadImportedMaterialCache(file);
    ASSERT_TRUE(cached.has_value());
    ASSERT_EQ(cached->size(), imported.size());
    const GUID model = GUID::Generate();
    for (uint32 index = 0; index < imported.size(); ++index)
    {
        SCOPED_TRACE(index);
        const auto live = ModelMaterialBridge::Convert(model, index, imported[index]).document;
        const auto reopened = ModelMaterialBridge::Convert(model, index, (*cached)[index]).document;
        EXPECT_EQ(reopened.ignoreVertexColor, live.ignoreVertexColor);
        const auto liveKeys = MaterialPrewarmVariantKeys(DeriveBaseVariantKey(live),
            Rendering::MaterialKeyword::ForwardPlus, false, live.alphaMode, live.ignoreVertexColor);
        const auto cachedKeys = MaterialPrewarmVariantKeys(DeriveBaseVariantKey(reopened),
            Rendering::MaterialKeyword::ForwardPlus, false, reopened.alphaMode, reopened.ignoreVertexColor);
        EXPECT_EQ(cachedKeys, liveKeys);
        const auto withColor = std::count_if(cachedKeys.begin(), cachedKeys.end(), [](const auto& key) {
            return Rendering::HasFlag(key.vertexFlags, Rendering::VertexAttributeFlags::HasColor);
        });
        if (imported[index].IgnoresVertexColor)
            EXPECT_EQ(withColor, 0);
        else
            EXPECT_GT(withColor, 0) << "the control must exercise vertex-color depth variants";
    }
    std::error_code ec;
    fs::remove_all(root, ec);
}
