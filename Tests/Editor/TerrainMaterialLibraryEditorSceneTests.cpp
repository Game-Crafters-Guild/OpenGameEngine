// The editor-shaped repro for "a bound .terrainmatlib never reaches pixels": the REAL
// ComposedIsland-mintgate scene (byte-copied from the authoring project, with its three real
// library files) loaded through the real scene loader into a world the REAL
// TerrainExtractionSystem extracts against a real Vulkan device — the whole chain the live
// editor runs, in one process, ending at the exact bytes UploadTerrainParamsArray put in the
// mapped table SSBO.
//
// The in-process unit suites cover every LINK of this chain (schema round-trip, cache resolve,
// record authoring, SSBO mirror) and all pass; the live editor still shades the island from the
// built-in palette whenever the library is edited. This fixture exists to hold the WHOLE chain
// in one hand, including the one condition no unit fixture exercised: the island's four
// materials all carry a bound albedo TEXTURE, and in the editor those textures RESOLVE to real
// bindless indices. The fixture registers real (tiny) PNGs under the scene's texture GUIDs so
// that condition holds here too.

#include <gtest/gtest.h>

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/TerrainMaterialLibraryAsset.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/Systems/TransformHierarchySystem.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/Device.h"
#include "Scene/SceneIO.h"
#include "TerrainECS/Scene/TerrainSceneSchemas.h"
#include "TerrainECS/Systems/TerrainExtractionSystem.h"
#include "TerrainECS/TerrainMaterialLibraryCache.h"
#include "TerrainECS/TerrainRenderFeature.h"
#include "TerrainECS/TerrainService.h"
#include "SplineECS/SplineService.h"
#include "Terrain/TerrainMaterialRecord.h"
#include "Terrain/TerrainRoleMaterials.h"
#include "Terrain/TerrainTypes.h"
#include "UndoRedo/UndoRedoService.h"

#include "StagedTestPaths.h"
#include "TestDeviceHelper.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace
{

using GameEngine::EngineCore;
using GameEngine::GUID;
namespace Components = GameEngine::Components;
namespace Terrain = GameEngine::Terrain;
namespace TerrainECS = GameEngine::TerrainECS;
namespace Roles = GameEngine::Editor::TerrainRoleMaterials;
namespace fs = std::filesystem;

// The island terrain's bound library and the authored grass tint inside it (slot 0). These are
// the scene's own values — asserting anything else would test a different scene than the one
// that fails live.
constexpr const char* kIslandLibraryGuid = "ba6105ed-ae50-4b04-b823-7435abd065ba";
constexpr float kAuthoredGrassR = 1.0f;
constexpr float kAuthoredGrassG = 0.05f;
constexpr float kAuthoredGrassB = 0.05f;
// The island block: sizeX = 512 — unique among the scene's three terrains, so it identifies
// the island's params entry without depending on extraction order.
constexpr float kIslandSizeX = 512.0f;

struct FixtureAsset
{
    const char* File;  // name inside the staged fixture directory
    const char* Guid;  // the GUID the scene/library text binds
    GameEngine::AssetType Type;
};

// The scene's four layer textures (grass/rock/dirt/snow) resolve to these GUIDs in the
// authoring project; the fixture provides a tiny PNG under each so the editor condition
// "the albedo texture RESOLVES" holds. The three libraries carry the scene's real bytes.
constexpr FixtureAsset kFixtureAssets[] = {
    {"ComposedIsland-mintgate Materials.terrainmatlib", kIslandLibraryGuid,
     GameEngine::AssetType::TerrainMaterialLibrary},
    {"ComposedIsland-mintgate Materials1.terrainmatlib", "f59c0ef6-1242-4bf3-8759-1f598c4db505",
     GameEngine::AssetType::TerrainMaterialLibrary},
    {"ComposedIsland-mintgate Materials2.terrainmatlib", "142eaf4c-b91a-47ab-ab27-7ee4f019ce42",
     GameEngine::AssetType::TerrainMaterialLibrary},
    {"layer0.png", "f78498e7-2643-4d6e-a345-d96c7bede21e", GameEngine::AssetType::Texture},
    {"layer1.png", "f0d2ab62-d04a-4d09-be27-b0ea9573dc68", GameEngine::AssetType::Texture},
    {"layer2.png", "98356aeb-6cbe-48f4-a62a-f8622ab5cb35", GameEngine::AssetType::Texture},
    {"layer3.png", "7305db25-159f-44dc-bd58-2699954a00b4", GameEngine::AssetType::Texture},
};

class TerrainMaterialLibraryEditorScene : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            GameEngine::ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
    }

    void SetUp() override
    {
        EngineCore& engine = EngineCore::GetInstance();
        ASSERT_TRUE(engine.IsInitialized());

        const fs::path staged = GameEngine::TestPaths::StagedRoot() / "Engine" / "Tests" /
                                "Fixtures" / "TerrainMatlibScene";
        if (!fs::exists(staged / "ComposedIsland-mintgate.scene"))
            GTEST_SKIP() << "TerrainMatlibScene fixture not staged (StageTestAssets): "
                         << staged.string();

        // Copies live inside the asset root: a file outside it cannot register under the
        // project identity the scene text binds.
        m_Dir = engine.GetAssetManager().GetAssetRoot() /
                ("ge_matlib_editor_scene_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::error_code ec;
        fs::create_directories(m_Dir, ec);
        ASSERT_FALSE(ec) << m_Dir.string();

        m_ScenePath = m_Dir / "ComposedIsland-mintgate.scene";
        fs::copy_file(staged / "ComposedIsland-mintgate.scene", m_ScenePath, ec);
        ASSERT_FALSE(ec);

        // Register every fixture asset under the EXACT GUID the scene text binds — the state a
        // completed project scan leaves behind in the editor (the scan itself is
        // registration-order plumbing already covered by the consumption suite's scan-race
        // test; the GUID<->file joins here are the editor's).
        for (const FixtureAsset& asset : kFixtureAssets)
        {
            const fs::path dst = m_Dir / asset.File;
            fs::copy_file(staged / asset.File, dst, ec);
            ASSERT_FALSE(ec) << dst.string();

            GameEngine::AssetMetadata md{};
            md.Guid = GUID(asset.Guid);
            md.Path = dst;
            md.Type = asset.Type;
            md.Name = dst.stem().string();
            md.Extension = dst.extension().string();
            ASSERT_TRUE(engine.GetAssetManager().GetRegistry().RegisterAssetMetadata(md))
                << asset.File;
            m_Registered.push_back(md.Guid);
        }
    }

    void TearDown() override
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (engine.IsInitialized())
        {
            for (const GUID& guid : m_Registered)
                engine.GetAssetManager().UnregisterLoadedAsset(guid);
            std::error_code ec;
            fs::remove_all(m_Dir, ec);
            for (int i = 0; i < 5; ++i)
                engine.GetAssetManager().Update();
            return;
        }
        std::error_code ec;
        fs::remove_all(m_Dir, ec);
    }

    fs::path m_Dir;
    fs::path m_ScenePath;
    std::vector<GUID> m_Registered;
};

// Releases the service-side terrain data the extraction ticks provisioned, so the process-wide
// TerrainService carries nothing of this test into later suites. The fixture world has no
// terrain OnRemove hooks (it is not an editor document world), so this is explicit.
void ReleaseProvisionedTerrains(GameEngine::ECS::World& world,
                                TerrainECS::TerrainRenderFeature& feature,
                                TerrainECS::TerrainService& service)
{
    world.Query<GameEngine::ECS::Write<Components::Terrain>>().Each(
        [&](Components::Terrain& terrain)
        {
            if (terrain.TerrainDataHandle != 0 || terrain.TerrainDataGeneration != 0)
            {
                TerrainECS::TerrainHandle h{terrain.TerrainDataHandle,
                                            terrain.TerrainDataGeneration};
                feature.ReleaseTerrainResources(h);
                service.DestroyTerrain(h);
                terrain.TerrainDataHandle = 0;
                terrain.TerrainDataGeneration = 0;
            }
            if (terrain.TiledTerrainHandle != 0 || terrain.TiledTerrainGeneration != 0)
            {
                service.DestroyTiledTerrain(TerrainECS::TiledTerrainHandle{
                    terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration});
                terrain.TiledTerrainHandle = 0;
                terrain.TiledTerrainGeneration = 0;
            }
        });
}

// The exact bytes extraction last uploaded: the island's params entry (WorldSizeX identifies it),
// then the grass record its LayerRole[0] addresses in the material table.
void ReadIslandGrassRecord(GameEngine::Rendering::IDevice& device,
                           TerrainECS::TerrainRenderFeature& feature,
                           Terrain::TerrainMaterialRecord& grass)
{
    const GameEngine::uint32 slot = feature.GetLastTerrainParamsSlot();
    const GameEngine::uint32 paramsCount = feature.GetTerrainParamsCount(slot);
    const GameEngine::uint32 materialCount = feature.GetTerrainMaterialCount(slot);
    ASSERT_GT(paramsCount, 0u) << "extraction produced no terrain params at all";
    ASSERT_GT(materialCount, 0u) << "extraction produced no material table at all";

    std::vector<Terrain::TerrainGPUParams> params(paramsCount);
    {
        const auto ssbo = feature.GetTerrainParamsSSBO(slot);
        ASSERT_TRUE(ssbo.IsValid());
        const void* mapped = device.MapBuffer(ssbo);
        ASSERT_NE(mapped, nullptr);
        std::memcpy(params.data(), mapped, paramsCount * sizeof(Terrain::TerrainGPUParams));
        device.UnmapBuffer(ssbo);
    }
    std::vector<Terrain::TerrainMaterialRecord> table(materialCount);
    {
        const auto ssbo = feature.GetTerrainMaterialTableSSBO(slot);
        ASSERT_TRUE(ssbo.IsValid());
        const void* mapped = device.MapBuffer(ssbo);
        ASSERT_NE(mapped, nullptr);
        std::memcpy(table.data(), mapped, materialCount * sizeof(Terrain::TerrainMaterialRecord));
        device.UnmapBuffer(ssbo);
    }

    const Terrain::TerrainGPUParams* island = nullptr;
    for (const auto& p : params)
        if (p.WorldSizeX == kIslandSizeX)
            island = &p;
    ASSERT_NE(island, nullptr) << "extraction never produced the island's params entry";

    const GameEngine::uint32 grassIndex = island->LayerRole[0];
    ASSERT_LT(grassIndex, materialCount);
    grass = table[grassIndex];
}

// THE editor-shaped assertion: after the real scene load and real extraction ticks, the grass
// record the island's params entry addresses must carry the library's authored albedo. This is
// the first fixture in the tree in which "the library's content" and "the bytes the surface
// shades from" can disagree the way the live editor's do.
TEST_F(TerrainMaterialLibraryEditorScene, ExtractionCarriesTheLibrarysAuthoredGrassTint)
{
    EngineCore& engine = EngineCore::GetInstance();

    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    {
        GameEngine::Engine::Renderer::RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        auto& feature = rs.EnsureFeature<TerrainECS::TerrainRenderFeature>();
        ASSERT_TRUE(feature.Initialize(device.get()));

        // EngineCore stands these up only on the EnableRenderingLoop (editor) path; this
        // headless process initializes the same set the editor holds when it loads this scene.
        // Left initialized: they are process-global and other suites share them.
        if (!TerrainECS::TerrainService::IsInitialized())
            TerrainECS::TerrainService::Initialize();
        if (!GameEngine::SplineECS::SplineService::IsInitialized())
            GameEngine::SplineECS::SplineService::Initialize();
        auto* service = TerrainECS::TerrainService::TryGet();
        ASSERT_NE(service, nullptr);

        // The real load path over the real file.
        GameEngine::Scene::EnsureTerrainSceneSchemasRegistered();
        GameEngine::ECS::World world;
        ASSERT_TRUE(GameEngine::Scene::LoadSceneFromFile(
            world, m_ScenePath, GameEngine::Scene::LoadOptions{GameEngine::Scene::LoadMode::Replace}))
            << GameEngine::Scene::GetLastSceneIOError().message;

        // Chain link 0, asserted before any extraction: the loaded island component carries the
        // scene's library binding. If THIS fails the break is in the loader, not downstream.
        bool sawIsland = false;
        GUID islandGuid{};
        world.Query<GameEngine::ECS::Read<Components::Terrain>>().Each(
            [&](const Components::Terrain& terrain)
            {
                if (terrain.SizeX == kIslandSizeX)
                {
                    sawIsland = true;
                    islandGuid = terrain.MaterialLibraryGuid.ToGuid();
                }
            });
        ASSERT_TRUE(sawIsland) << "the scene's island terrain did not load";
        ASSERT_EQ(islandGuid, GUID(kIslandLibraryGuid))
            << "the island lost its materialLibrary binding in the load";

        // The real systems, in the editor's order: world transforms, then terrain extraction.
        GameEngine::Engine::Renderer::TransformHierarchySystem transforms;
        TerrainECS::TerrainExtractionSystem extraction(&rs);

        // Pump until the library cache resolves (the asset load it requests is asynchronous),
        // then a few settle frames so the table was rebuilt FROM the resolved library. Bounded:
        // a chain that never resolves falls through with resolved=false and the record
        // assertions below say which half died.
        bool resolved = false;
        int settleFrames = 0;
        for (int frame = 0; frame < 600; ++frame)
        {
            engine.GetAssetManager().Update();
            transforms.Update(world, 0.016f);
            extraction.Update(world, 0.016f);
            if (!resolved && feature.MaterialLibraries().Get(GUID(kIslandLibraryGuid), {}) != nullptr)
                resolved = true;
            if (resolved && ++settleFrames >= 5)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        EXPECT_TRUE(resolved)
            << "the bound library never resolved through the extraction-side cache";

        Terrain::TerrainMaterialRecord grass{};
        ASSERT_NO_FATAL_FAILURE(ReadIslandGrassRecord(*device, feature, grass));

        // The editor condition, verified rather than assumed: the grass material's albedo
        // texture resolved to a real bindless index here, exactly as it does live. If this
        // fails the fixture lost the editor-difference it exists to carry.
        ASSERT_NE(grass.AlbedoTex, Terrain::kTerrainMaterialUnboundTexture)
            << "the fixture's grass albedo texture did not resolve — this fixture no longer "
               "reproduces the editor's conditions";

        // What the library authored is what the surface must shade from. cbt_surface multiplies
        // the sampled albedo by this tint, so the authored red reaches pixels iff it is here.
        EXPECT_FLOAT_EQ(grass.AlbedoR, kAuthoredGrassR)
            << "the library's authored grass tint never reached the uploaded record";
        EXPECT_FLOAT_EQ(grass.AlbedoG, kAuthoredGrassG);
        EXPECT_FLOAT_EQ(grass.AlbedoB, kAuthoredGrassB);

        ReleaseProvisionedTerrains(world, feature, *service);
        rs.Shutdown();
    }
    device->Shutdown();
}

// The upstream half in isolation, against the same real fixture: scene file -> component ->
// cache -> parsed entries. Green here + red above convicts the record-authoring step; red here
// means the break is upstream of authoring and the test above cannot localize it alone.
TEST_F(TerrainMaterialLibraryEditorScene, TheBoundLibraryResolvesToItsAuthoredEntries)
{
    EngineCore& engine = EngineCore::GetInstance();
    TerrainECS::TerrainMaterialLibraryCache cache;

    const std::vector<GameEngine::TerrainMaterialEntry>* entries = nullptr;
    for (int frame = 0; frame < 600 && !entries; ++frame)
    {
        entries = cache.Get(GUID(kIslandLibraryGuid), {});
        engine.GetAssetManager().Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_NE(entries, nullptr) << "the island's library never resolved from the real file";
    ASSERT_EQ(entries->size(), 4u);

    const GameEngine::TerrainMaterialEntry* grass = nullptr;
    for (const auto& e : *entries)
        if (e.SlotId == 0)
            grass = &e;
    ASSERT_NE(grass, nullptr);
    EXPECT_FLOAT_EQ(grass->AlbedoR, kAuthoredGrassR);
    EXPECT_FLOAT_EQ(grass->AlbedoG, kAuthoredGrassG);
    EXPECT_FLOAT_EQ(grass->AlbedoB, kAuthoredGrassB);
}

// The GUID the redirected-binding test writes into the scene in place of the island's library
// GUID: an identity the registry forwards to the library, the state a project is left in when a
// file's GUID is re-minted and the old one is kept as a redirect.
constexpr const char* kRedirectedIslandLibraryGuid = "ba6105ed-0000-4000-8000-00000000a11a";

// A registry redirect for the length of one test; the registry is process-wide.
class ScopedRedirect
{
public:
    ScopedRedirect(GameEngine::AssetRegistry& registry, const GUID& from, const GUID& to)
        : m_Registry(registry), m_From(from), m_Added(registry.AddRedirect(from, to))
    {
    }
    ScopedRedirect(const ScopedRedirect&) = delete;
    ScopedRedirect& operator=(const ScopedRedirect&) = delete;
    ~ScopedRedirect()
    {
        if (m_Added)
            m_Registry.RemoveRedirect(m_From);
    }
    bool Added() const { return m_Added; }

private:
    GameEngine::AssetRegistry& m_Registry;
    GUID m_From;
    bool m_Added = false;
};

// One editor frame of the chain, in the order EngineCore::Update runs it: reloads drain, then
// world transforms, then terrain extraction.
struct EditorFrame
{
    GameEngine::AssetManager& Assets;
    GameEngine::ECS::World& World;
    GameEngine::Engine::Renderer::TransformHierarchySystem& Transforms;
    TerrainECS::TerrainExtractionSystem& Extraction;
    GameEngine::Rendering::IDevice& Device;
    TerrainECS::TerrainRenderFeature& Feature;
};

// Ticks editor frames until the uploaded island grass tint is `r`, and returns the last record
// read. Bounded well past the file watcher's debounce and the timestamp poll interval, so a miss
// is a chain that never delivers rather than a slow one.
Terrain::TerrainMaterialRecord TickUntilIslandGrassR(const EditorFrame& frame, float r)
{
    Terrain::TerrainMaterialRecord grass{};
    for (int i = 0; i < 1500; ++i)
    {
        frame.Assets.Update();
        frame.Transforms.Update(frame.World, 0.016f);
        frame.Extraction.Update(frame.World, 0.016f);
        ReadIslandGrassRecord(frame.Device, frame.Feature, grass);
        if (::testing::Test::HasFatalFailure() || grass.AlbedoR == r)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return grass;
}

// Ticks editor frames until extraction's library cache has adopted `guid`'s parse. Until then the
// island shades from its legacy per-layer fields, whose grass tint is not a library value.
bool TickUntilLibraryResolves(const EditorFrame& frame, const GUID& guid)
{
    for (int i = 0; i < 1500; ++i)
    {
        frame.Assets.Update();
        frame.Transforms.Update(frame.World, 0.016f);
        frame.Extraction.Update(frame.World, 0.016f);
        if (frame.Feature.MaterialLibraries().Get(guid, {}) != nullptr)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

void SetGrassTintR(std::vector<GameEngine::TerrainMaterialEntry>& entries, float r)
{
    for (auto& e : entries)
        if (e.SlotId == 0)
            e.AlbedoR = r;
}

// Rewrites the scene so the island binds `replacement` where it bound its library GUID.
void RebindIslandLibrary(const fs::path& scenePath, const char* replacement)
{
    std::string text;
    {
        std::ifstream in(scenePath, std::ios::binary);
        ASSERT_TRUE(in.is_open()) << scenePath.string();
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::string bound = std::string("\"") + kIslandLibraryGuid + "\"";
    const std::size_t at = text.find(bound);
    ASSERT_NE(at, std::string::npos) << "the fixture scene no longer binds the island library";
    text.replace(at, bound.size(), std::string("\"") + replacement + "\"");
    std::ofstream out(scenePath, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open()) << scenePath.string();
    out << text;
}

// An edit in the library inspector, and its undo, must reach the bytes the surface shades from
// while the scene stays open, also for a terrain that binds its library through a REDIRECTED
// GUID. The asset manager resolves the redirect, and its reload event names the library by the
// GUID it resolved to; a cache filed only under the terrain's GUID never matches that event and
// serves the first parse for the rest of the session. Driven through the inspector's own write
// (EditLibraryUndoable), the real reload path (file watcher and timestamp poll, drained by
// AssetManager::Update) and the real extraction; no event is raised by hand.
TEST_F(TerrainMaterialLibraryEditorScene, AnEditAndItsUndoReachATerrainBoundThroughARedirectedGuid)
{
    EngineCore& engine = EngineCore::GetInstance();
    GameEngine::AssetManager& assets = engine.GetAssetManager();

    ASSERT_NO_FATAL_FAILURE(RebindIslandLibrary(m_ScenePath, kRedirectedIslandLibraryGuid));
    ScopedRedirect redirect(assets.GetRegistry(), GUID(kRedirectedIslandLibraryGuid),
                            GUID(kIslandLibraryGuid));
    ASSERT_TRUE(redirect.Added()) << "this process has no project store to hold a redirect";
    ASSERT_EQ(assets.GetRegistry().ResolveGuid(GUID(kRedirectedIslandLibraryGuid)),
              GUID(kIslandLibraryGuid));

    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    {
        GameEngine::Engine::Renderer::RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        auto& feature = rs.EnsureFeature<TerrainECS::TerrainRenderFeature>();
        ASSERT_TRUE(feature.Initialize(device.get()));
        if (!TerrainECS::TerrainService::IsInitialized())
            TerrainECS::TerrainService::Initialize();
        if (!GameEngine::SplineECS::SplineService::IsInitialized())
            GameEngine::SplineECS::SplineService::Initialize();
        auto* service = TerrainECS::TerrainService::TryGet();
        ASSERT_NE(service, nullptr);

        GameEngine::Scene::EnsureTerrainSceneSchemasRegistered();
        GameEngine::ECS::World world;
        ASSERT_TRUE(GameEngine::Scene::LoadSceneFromFile(
            world, m_ScenePath, GameEngine::Scene::LoadOptions{GameEngine::Scene::LoadMode::Replace}))
            << GameEngine::Scene::GetLastSceneIOError().message;

        GameEngine::Engine::Renderer::TransformHierarchySystem transforms;
        TerrainECS::TerrainExtractionSystem extraction(&rs);
        const EditorFrame frame{assets, world, transforms, extraction, *device, feature};

        ASSERT_TRUE(TickUntilLibraryResolves(frame, GUID(kRedirectedIslandLibraryGuid)))
            << "the library never resolved through the redirect at all";
        const Terrain::TerrainMaterialRecord authored = TickUntilIslandGrassR(frame, kAuthoredGrassR);
        ASSERT_FALSE(HasFatalFailure());
        ASSERT_FLOAT_EQ(authored.AlbedoR, kAuthoredGrassR)
            << "the resolved library's tint never reached the table";

        auto asset = assets.GetAsset(GUID(kIslandLibraryGuid));
        auto* library = dynamic_cast<GameEngine::TerrainMaterialLibraryAsset*>(asset.get());
        ASSERT_NE(library, nullptr);

        // The inspector's tint edit on the grass material (slot 0).
        constexpr float kEditedR = 0.25f;
        GameEngine::Editor::UndoRedoService undo;
        Roles::EditLibraryUndoable(library, &undo, "Change Terrain Material Tint", {},
                                   [](std::vector<GameEngine::TerrainMaterialEntry>& entries)
                                   { SetGrassTintR(entries, kEditedR); });

        const Terrain::TerrainMaterialRecord edited = TickUntilIslandGrassR(frame, kEditedR);
        ASSERT_FALSE(HasFatalFailure());
        EXPECT_FLOAT_EQ(edited.AlbedoR, kEditedR)
            << "the edit reached the library but never the table the surface shades from";

        undo.Undo();
        const Terrain::TerrainMaterialRecord undone = TickUntilIslandGrassR(frame, kAuthoredGrassR);
        ASSERT_FALSE(HasFatalFailure());
        EXPECT_FLOAT_EQ(undone.AlbedoR, kAuthoredGrassR)
            << "the undo restored the library but never the table the surface shades from";

        ReleaseProvisionedTerrains(world, feature, *service);
        rs.Shutdown();
    }
    device->Shutdown();
}

} // namespace
