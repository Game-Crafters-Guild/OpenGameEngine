// The two editor surfaces that answer "show me everything": the smart folder's
// registry collection and the global asset search. Both used to name the asset
// types they were willing to look at, so a type added to the enum after those
// lists were written was invisible in the editor while being perfectly well
// registered — no error, no empty state, just an asset that could not be found.
//
// The probes are one type that was on both lists (Material) and one that was on
// neither (TerrainHeightmap). The Material probe is load-bearing: it keeps the
// registry collection non-empty, so the smart folder's filesystem fallback —
// which finds every file on disk regardless of type — stays disarmed and cannot
// make a failing registry path look like a passing one.

#include <gtest/gtest.h>

#include "AssetCore/AssetTypes.h"
#include "Assets/AssetRegistry.h"
#include "Assets/AssetSearchProvider.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "UI/SmartFolder/SmartFolderFilterEvaluator.h"

#include "../TestTempDir.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

using namespace GameEngine;

namespace
{

constexpr const char* kMaterialProbeName = "surfaceprobe_material";
constexpr const char* kTerrainProbeName = "surfaceprobe_terrain";
constexpr const char* kProbeQuery = "surfaceprobe_";

void WriteAssetSurfaceProbeFile(const std::filesystem::path& p)
{
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open()) << p.string();
    out << "DUMMY";
}

bool ContainsFileNamed(const std::vector<std::filesystem::path>& paths, const std::string& fileName)
{
    for (const auto& p : paths)
    {
        if (p.filename().generic_string() == fileName)
            return true;
    }
    return false;
}

const SearchResultItem* FindByLabel(const std::vector<SearchResultItem>& items, const std::string& label)
{
    for (const auto& item : items)
    {
        if (item.Label == label)
            return &item;
    }
    return nullptr;
}

class AssetSurfaceTypeCoverage : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Root = TestUtils::MakeUniqueTempDirectory("ge_asset_surface_types");
        std::error_code ec;
        std::filesystem::remove_all(m_Root, ec);
        m_AssetsRoot = m_Root / "Assets";
        std::filesystem::create_directories(m_AssetsRoot, ec);

        WriteAssetSurfaceProbeFile(MaterialProbePath());
        WriteAssetSurfaceProbeFile(TerrainProbePath());

        m_Pool = std::make_unique<JobSystem::WorkStealingThreadPool>(2);
        ASSERT_TRUE(m_Registry.Initialize(m_AssetsRoot, m_Pool.get()));
        auto scan = m_Registry.ScanDirectoryAsync(m_AssetsRoot, true);
        (void)scan.get();

        // Instrument check: neither surface can be measured unless the registry
        // itself typed the probes, and a registry that reports Unknown for both
        // would make the assertions below pass for the wrong reason.
        ASSERT_EQ(RegisteredType(MaterialProbePath()), AssetType::Material);
        ASSERT_EQ(RegisteredType(TerrainProbePath()), AssetType::TerrainHeightmap);
    }

    void TearDown() override
    {
        m_Registry.Shutdown();
        m_Pool.reset();
        std::error_code ec;
        std::filesystem::remove_all(m_Root, ec);
    }

    std::filesystem::path MaterialProbePath() const
    {
        return m_AssetsRoot / (std::string(kMaterialProbeName) + ".mat");
    }

    std::filesystem::path TerrainProbePath() const
    {
        return m_AssetsRoot / (std::string(kTerrainProbeName) + ".r16");
    }

    AssetType RegisteredType(const std::filesystem::path& path)
    {
        AssetMetadata md{};
        if (!m_Registry.TryGetAssetMetadata(path, md))
            return AssetType::Unknown;
        return md.Type;
    }

    std::filesystem::path m_Root;
    std::filesystem::path m_AssetsRoot;
    std::unique_ptr<JobSystem::WorkStealingThreadPool> m_Pool;
    AssetRegistry m_Registry;
};

TEST_F(AssetSurfaceTypeCoverage, SmartFolderCollectsAssetsOfEveryRegisteredType)
{
    SmartFolder folder;
    folder.Id = "surfaceprobe";
    folder.Name = "Surface Probe";
    folder.Filters.push_back({SmartFolderFilter::Type::NameContains, kProbeQuery});

    SmartFolderFilterEvaluator evaluator;
    const std::vector<std::filesystem::path> matches =
        evaluator.Evaluate(folder, m_AssetsRoot, &m_Registry);

    EXPECT_TRUE(ContainsFileNamed(matches, std::string(kMaterialProbeName) + ".mat"));
    EXPECT_TRUE(ContainsFileNamed(matches, std::string(kTerrainProbeName) + ".r16"))
        << "a TerrainHeightmap asset the registry knows about never reaches a smart folder";
}

TEST_F(AssetSurfaceTypeCoverage, AssetSearchReturnsAssetsOfEveryRegisteredType)
{
    AssetSearchProvider provider(&m_Registry);

    std::vector<SearchResultItem> results;
    bool complete = false;
    provider.BeginSearch(kProbeQuery, [&](std::vector<SearchResultItem> batch, bool isComplete) {
        results.insert(results.end(), batch.begin(), batch.end());
        complete = complete || isComplete;
    });
    ASSERT_TRUE(complete);

    EXPECT_NE(FindByLabel(results, kMaterialProbeName), nullptr);

    const SearchResultItem* terrain = FindByLabel(results, kTerrainProbeName);
    ASSERT_NE(terrain, nullptr)
        << "a TerrainHeightmap asset the registry knows about is unreachable from asset search";
    EXPECT_EQ(terrain->TypeKey, AssetTypeToString(AssetType::TerrainHeightmap));
}

} // namespace
