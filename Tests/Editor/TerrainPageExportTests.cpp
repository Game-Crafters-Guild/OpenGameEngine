// The export of a terrain heightmap's cooked pages: one terrain container per shipped heightmap,
// cooked in the project's TerrainPages cache, over a live registry as the build pipeline runs it.

#include <gtest/gtest.h>

#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "Engine/Build/AssetCollector.h"
#include "Engine/Build/TerrainPageExport.h"
#include "PageStreaming/PageStoreReader.h"
#include "PageStreaming/TerrainPageContainer.h"
#include "Types/PathUtils.h"

#include "../TestTempDir.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

using namespace GameEngine;

namespace
{

class TerrainPageExportTests : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
        if (!s_Root.empty())
            return; // registered on the first run of the suite (--gtest_repeat)
        s_Root = TestUtils::MakeUniqueTempDirectory("terrain_page_export");
        std::filesystem::create_directories(s_Root / "Assets");
        AssetSourceDesc source{};
        source.Alias = "terrainpageexport";
        source.Root = s_Root / "Assets";
        source.AuthoritativeDbFile = s_Root / "Assets" / "AssetDatabase.assetdb";
        ASSERT_TRUE(engine.GetAssetManager().RegisterSource(source));
    }

    // The source stays registered for the process (an asset source cannot be re-registered under its
    // alias, and --gtest_repeat runs the suite again); each test writes its own files under it. The
    // folder is removed once, after the last repeat, by TerrainPageExportFolderEnvironment.

    static std::filesystem::path s_Root;
    friend class TerrainPageExportFolderEnvironment;
};

std::filesystem::path TerrainPageExportTests::s_Root;

// Global environments tear down once, after the last --gtest_repeat iteration.
class TerrainPageExportFolderEnvironment : public ::testing::Environment
{
  public:
    void TearDown() override
    {
        if (TerrainPageExportTests::s_Root.empty())
            return;
        std::error_code ec;
        std::filesystem::remove_all(TerrainPageExportTests::s_Root, ec);
        EXPECT_FALSE(ec) << "could not remove " << TerrainPageExportTests::s_Root << ": " << ec.message();
    }
};

const ::testing::Environment* const s_TerrainPageExportFolderEnvironment =
    ::testing::AddGlobalTestEnvironment(new TerrainPageExportFolderEnvironment);

TEST_F(TerrainPageExportTests, EachShippedHeightmapShipsOneContainerOfItsCookedPages)
{
    // A square 257 x 257 .r32: no import grid needed.
    const std::filesystem::path heightmap = s_Root / "Assets" / "hills.r32";
    {
        std::vector<float> samples(257 * 257);
        for (std::size_t i = 0; i < samples.size(); ++i)
            samples[i] = 100.0f + 20.0f * std::sin(0.05f * float(i % 257)) * std::cos(0.07f * float(i / 257));
        std::ofstream(heightmap, std::ios::binary)
            .write(reinterpret_cast<const char*>(samples.data()), std::streamsize(samples.size() * sizeof(float)));
    }
    AssetManager& assets = EngineCore::GetInstance().GetAssetManager();
    const GUID guid = assets.ResolveAssetGuid(heightmap);
    ASSERT_FALSE(guid.IsNull());

    AssetManifest manifest;
    AssetManifestEntry entry;
    entry.guid = guid;
    entry.type = AssetType::TerrainHeightmap;
    entry.sourcePath = heightmap;
    manifest.entries.push_back(entry);

    const std::filesystem::path content = s_Root / "Package";
    const TerrainPageExportResult result =
        ExportTerrainPages(s_Root, content, manifest, assets.GetRegistry(), &EngineCore::GetInstance().GetJobSystem(),
                           nullptr);
    ASSERT_TRUE(result.Warnings.empty()) << result.Warnings.front();
    ASSERT_EQ(result.Containers.size(), 1u);
    // Where the Player opens it: under the asset root of a package whose executable sits in `content`.
    EXPECT_EQ(result.Containers.front(),
              PathUtils::InstallAssetsRootFor(content) / "Cooked" / "Terrain" / (guid.ToString() + ".geterrain"));

    PageStreaming::PageStoreReader reader;
    ASSERT_EQ(PageStreaming::OpenTerrainContainerField(result.Containers.front(), PageStreaming::PageFieldKind::Height,
                                                       reader),
              "");
    EXPECT_EQ(reader.Layout().Header.SamplesX, 257u);
    std::size_t stores = 0;
    for (const auto& file : std::filesystem::directory_iterator(s_Root / ".Cache" / "TerrainPages"))
        stores += file.path().extension() == ".gepage" ? 1u : 0u;
    EXPECT_EQ(stores, 1u) << "the store is cooked once, in the project's cache";
}

TEST_F(TerrainPageExportTests, ACancelledExportCooksNothingAndSaysSo)
{
    const std::filesystem::path heightmap = s_Root / "Assets" / "cancelled.r32";
    {
        std::vector<float> samples(129 * 129, 5.0f);
        std::ofstream(heightmap, std::ios::binary)
            .write(reinterpret_cast<const char*>(samples.data()), std::streamsize(samples.size() * sizeof(float)));
    }
    AssetManager& assets = EngineCore::GetInstance().GetAssetManager();
    AssetManifest manifest;
    AssetManifestEntry entry;
    entry.guid = assets.ResolveAssetGuid(heightmap);
    entry.type = AssetType::TerrainHeightmap;
    entry.sourcePath = heightmap;
    manifest.entries.push_back(entry);

    const std::atomic<bool> cancelled{true};
    const TerrainPageExportResult result = ExportTerrainPages(s_Root, s_Root / "CancelledPackage", manifest,
                                                              assets.GetRegistry(), nullptr, &cancelled);
    EXPECT_TRUE(result.Containers.empty());
    ASSERT_EQ(result.Warnings.size(), 1u);
    EXPECT_NE(result.Warnings.front().find("cancelled"), std::string::npos);
}

} // namespace
