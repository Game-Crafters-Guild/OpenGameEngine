// A raw heightmap's grid edit, over a live registry and the terrain service's re-decode lane: one
// undo step per edit, the stored value back on undo, one re-decode per write, and none for a value
// the store already holds (a re-decode of a real DEM is hundreds of megabytes).

#include <gtest/gtest.h>

#include "Terrain/RawHeightmapSettingsCommit.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"
#include "TerrainECS/RawHeightmap.h"
#include "TerrainECS/TerrainService.h"
#include "UndoRedo/UndoRedoService.h"

#include "../TestTempDir.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

using namespace GameEngine;

namespace
{

class RawHeightmapSettingsCommitTests : public ::testing::Test
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
        if (!TerrainECS::TerrainService::IsInitialized())
            TerrainECS::TerrainService::Initialize();

        s_Root = TestUtils::MakeUniqueTempDirectory("raw_heightmap_settings_commit");
        std::filesystem::create_directories(s_Root);
        AssetSourceDesc source{};
        source.Alias = "rawheightmapcommit";
        source.Root = s_Root;
        source.AuthoritativeDbFile = s_Root / "AssetDatabase.assetdb"; // the store the grid settings live in
        ASSERT_TRUE(engine.GetAssetManager().RegisterSource(source));
    }

    static void TearDownTestSuite()
    {
        std::error_code ec;
        std::filesystem::remove_all(s_Root, ec);
    }

    static std::filesystem::path WriteHeightmap(const std::string& name, GUID& outGuid)
    {
        const std::filesystem::path path = s_Root / name;
        std::ofstream out(path, std::ios::binary);
        const float samples[8] = {};
        out.write(reinterpret_cast<const char*>(samples), sizeof(samples));
        out.close();
        outGuid = EngineCore::GetInstance().GetAssetManager().ResolveAssetGuid(path);
        EXPECT_FALSE(outGuid.IsNull());
        return path;
    }

    static std::string Stored(const std::filesystem::path& path)
    {
        std::string text;
        EngineCore::GetInstance().GetAssetManager().GetRegistry().TryGetMetaValue(
            path, TerrainECS::kRawHeightmapWidthKey, text);
        return text;
    }

    static std::size_t ReDecodesRequested()
    {
        return TerrainECS::TerrainService::Get().TakePendingAssetInvalidations().size();
    }

    static std::filesystem::path s_Root;
};

std::filesystem::path RawHeightmapSettingsCommitTests::s_Root;

TEST_F(RawHeightmapSettingsCommitTests, AnEditIsOneUndoStepAndEachWriteReDecodesOnce)
{
    GUID guid;
    const std::filesystem::path path = WriteHeightmap("undo_grid.r32", guid);
    Editor::UndoRedoService undo;
    (void)ReDecodesRequested();

    CommitRawHeightmapSampleCount(&undo, path, guid, TerrainECS::kRawHeightmapWidthKey, 4, {});
    EXPECT_EQ(Stored(path), "4");
    EXPECT_EQ(undo.GetUndoCount(), 1u);
    EXPECT_EQ(ReDecodesRequested(), 1u);

    undo.Undo();
    uint32 count = 7;
    EXPECT_TRUE(TerrainECS::ParseRawHeightmapSampleCount(TerrainECS::kRawHeightmapWidthKey, Stored(path), count)
                    .empty());
    EXPECT_EQ(count, 0u) << "the undo must put the grid back to unset";
    EXPECT_EQ(ReDecodesRequested(), 1u);
}

TEST_F(RawHeightmapSettingsCommitTests, AValueTheStoreHoldsIsNotAnEdit)
{
    GUID guid;
    const std::filesystem::path path = WriteHeightmap("same_grid.r32", guid);
    Editor::UndoRedoService undo;
    CommitRawHeightmapSampleCount(&undo, path, guid, TerrainECS::kRawHeightmapWidthKey, 4, {});
    (void)ReDecodesRequested();

    CommitRawHeightmapSampleCount(&undo, path, guid, TerrainECS::kRawHeightmapWidthKey, 4, {});
    EXPECT_EQ(undo.GetUndoCount(), 1u);
    EXPECT_EQ(ReDecodesRequested(), 0u) << "retyping the stored value re-decoded the heightmap";
}

} // namespace
