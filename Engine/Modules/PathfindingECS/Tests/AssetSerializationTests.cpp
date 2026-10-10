#include "Assets/NavGridAsset.h"
#include "Assets/NavMeshAsset.h"
#include "Pathfinding/NavCacheManager.h"
#include "AssetCore/GUID.h"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Pathfinding;

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class AssetSerializationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_TempDir = std::filesystem::temp_directory_path() / "ge_nav_test";
        std::filesystem::create_directories(m_TempDir);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_TempDir, ec);
    }

    std::filesystem::path m_TempDir;

    // Helper: write raw bytes to a file
    static void WriteBytes(const std::filesystem::path& path, const void* data, size_t size)
    {
        std::ofstream file(path, std::ios::binary);
        file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    }

    // Helper: write a string to a file
    static void WriteString(const std::filesystem::path& path, const std::string& content)
    {
        std::ofstream file(path, std::ios::binary);
        file << content;
    }
};

// ===========================================================================
// NavGridAsset Tests
// ===========================================================================

TEST_F(AssetSerializationTest, NavGrid_SaveAndLoadRoundTrip)
{
    GridSettings settings;
    settings.Type = GridType::Hex;
    settings.CellSize = 2.5f;
    settings.OriginX = 10.0f;
    settings.OriginY = -5.0f;
    settings.OriginZ = 3.0f;
    settings.Width = 4;
    settings.Depth = 3;
    settings.MaxSlope = 30.0f;
    settings.MaxStepHeight = 0.8f;

    const uint32 cellCount = settings.Width * settings.Depth;
    std::vector<float32> costs(cellCount);
    std::vector<uint8> blocked(cellCount);
    for (uint32 i = 0; i < cellCount; ++i)
    {
        costs[i] = static_cast<float32>(i) * 1.5f;
        blocked[i] = (i % 3 == 0) ? 1 : 0;
    }

    auto filePath = m_TempDir / "roundtrip.navgrid";
    ASSERT_TRUE(NavGridAsset::SaveToFile(filePath, settings, costs.data(), blocked.data(), cellCount));

    GUID guid = GUID::Generate();
    NavGridAsset asset(guid, filePath);
    ASSERT_TRUE(asset.Load());

    const auto& loaded = asset.GetGridSettings();
    EXPECT_EQ(static_cast<uint8>(loaded.Type), static_cast<uint8>(GridType::Hex));
    EXPECT_FLOAT_EQ(loaded.CellSize, 2.5f);
    EXPECT_FLOAT_EQ(loaded.OriginX, 10.0f);
    EXPECT_FLOAT_EQ(loaded.OriginY, -5.0f);
    EXPECT_FLOAT_EQ(loaded.OriginZ, 3.0f);
    EXPECT_EQ(loaded.Width, 4u);
    EXPECT_EQ(loaded.Depth, 3u);
    EXPECT_FLOAT_EQ(loaded.MaxSlope, 30.0f);
    EXPECT_FLOAT_EQ(loaded.MaxStepHeight, 0.8f);

    EXPECT_EQ(asset.GetCosts().size(), cellCount);
    EXPECT_EQ(asset.GetBlocked().size(), cellCount);
    for (uint32 i = 0; i < cellCount; ++i)
    {
        EXPECT_FLOAT_EQ(asset.GetCosts()[i], costs[i]) << "cost mismatch at " << i;
        EXPECT_EQ(asset.GetBlocked()[i], blocked[i]) << "blocked mismatch at " << i;
    }
}

TEST_F(AssetSerializationTest, NavGrid_TruncatedFileFailsGracefully)
{
    // Write a valid header but truncate mid-costs-array
    GridSettings settings;
    settings.Width = 8;
    settings.Depth = 8;
    const uint32 cellCount = settings.Width * settings.Depth;
    std::vector<float32> costs(cellCount, 1.0f);
    std::vector<uint8> blocked(cellCount, 0);

    auto validPath = m_TempDir / "valid_tmp.navgrid";
    ASSERT_TRUE(NavGridAsset::SaveToFile(validPath, settings, costs.data(), blocked.data(), cellCount));

    // Read back and truncate
    std::ifstream validFile(validPath, std::ios::binary | std::ios::ate);
    auto fullSize = validFile.tellg();
    validFile.seekg(0);
    std::vector<char> fullData(static_cast<size_t>(fullSize));
    validFile.read(fullData.data(), fullSize);
    validFile.close();

    // Truncate partway through the costs array (header is 41 bytes, costs start there)
    size_t truncatedSize = 41 + 8; // only 8 bytes of costs instead of 64*4=256
    auto truncPath = m_TempDir / "truncated.navgrid";
    WriteBytes(truncPath, fullData.data(), truncatedSize);

    GUID guid = GUID::Generate();
    NavGridAsset asset(guid, truncPath);
    EXPECT_FALSE(asset.Load());
}

TEST_F(AssetSerializationTest, NavGrid_WrongMagicFails)
{
    // Write a file with wrong magic but otherwise plausible data
    auto filePath = m_TempDir / "wrongmagic.navgrid";
    std::ofstream file(filePath, std::ios::binary);
    file.write("XXXX", 4); // wrong magic
    uint32 version = 1;
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    // Pad to minimum header size
    std::vector<uint8> padding(128, 0);
    file.write(reinterpret_cast<const char*>(padding.data()), padding.size());
    file.close();

    GUID guid = GUID::Generate();
    NavGridAsset asset(guid, filePath);
    EXPECT_FALSE(asset.Load());
}

TEST_F(AssetSerializationTest, NavGrid_VersionMismatchFails)
{
    auto filePath = m_TempDir / "badversion.navgrid";
    std::ofstream file(filePath, std::ios::binary);
    file.write("NGRD", 4); // correct magic
    uint32 version = 99;
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    std::vector<uint8> padding(128, 0);
    file.write(reinterpret_cast<const char*>(padding.data()), padding.size());
    file.close();

    GUID guid = GUID::Generate();
    NavGridAsset asset(guid, filePath);
    EXPECT_FALSE(asset.Load());
}

TEST_F(AssetSerializationTest, NavGrid_ZeroDimensionsFails)
{
    // Build a header with Width=0 by manually writing the binary format
    auto filePath = m_TempDir / "zerodim.navgrid";
    std::ofstream file(filePath, std::ios::binary);
    file.write("NGRD", 4);
    uint32 version = 1;
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    uint8 gridType = 0;
    file.write(reinterpret_cast<const char*>(&gridType), 1);
    float32 cellSize = 1.0f;
    file.write(reinterpret_cast<const char*>(&cellSize), sizeof(cellSize));
    float32 originX = 0.0f, originY = 0.0f, originZ = 0.0f;
    file.write(reinterpret_cast<const char*>(&originX), sizeof(originX));
    file.write(reinterpret_cast<const char*>(&originY), sizeof(originY));
    file.write(reinterpret_cast<const char*>(&originZ), sizeof(originZ));
    uint32 width = 0, depth = 10;
    file.write(reinterpret_cast<const char*>(&width), sizeof(width));
    file.write(reinterpret_cast<const char*>(&depth), sizeof(depth));
    float32 maxSlope = 45.0f, maxStep = 0.4f;
    file.write(reinterpret_cast<const char*>(&maxSlope), sizeof(maxSlope));
    file.write(reinterpret_cast<const char*>(&maxStep), sizeof(maxStep));
    file.close();

    GUID guid = GUID::Generate();
    NavGridAsset asset(guid, filePath);
    EXPECT_FALSE(asset.Load());
}

TEST_F(AssetSerializationTest, NavGrid_EmptyFileFailsGracefully)
{
    auto filePath = m_TempDir / "empty.navgrid";
    std::ofstream file(filePath, std::ios::binary);
    file.close();

    GUID guid = GUID::Generate();
    NavGridAsset asset(guid, filePath);
    EXPECT_FALSE(asset.Load());
}

// ===========================================================================
// NavMeshAsset Tests
// ===========================================================================

TEST_F(AssetSerializationTest, NavMesh_SaveAndLoadJsonRoundTrip)
{
    NavMeshSettings settings;
    settings.CellSize = 0.5f;
    settings.CellHeight = 0.3f;
    settings.AgentRadius = 0.8f;
    settings.AgentHeight = 1.8f;
    settings.AgentMaxClimb = 0.5f;
    settings.AgentMaxSlope = 60.0f;
    settings.RegionMinSize = 4.0f;
    settings.RegionMergeSize = 10.0f;
    settings.EdgeMaxLen = 8.0f;
    settings.EdgeMaxError = 2.0f;
    settings.DetailSampleDist = 3.0f;
    settings.DetailSampleMaxError = 0.5f;
    settings.VertsPerPoly = 4;

    std::vector<GUID> guids;
    guids.push_back(GUID::Generate());
    guids.push_back(GUID::Generate());
    guids.push_back(GUID::Generate());

    auto filePath = m_TempDir / "roundtrip.navmesh";
    ASSERT_TRUE(NavMeshAsset::SaveToFile(filePath, settings, guids));

    GUID assetGuid = GUID::Generate();
    NavMeshAsset asset(assetGuid, filePath);
    ASSERT_TRUE(asset.Load());

    const auto& loaded = asset.GetSettings();
    EXPECT_FLOAT_EQ(loaded.CellSize, 0.5f);
    EXPECT_FLOAT_EQ(loaded.CellHeight, 0.3f);
    EXPECT_FLOAT_EQ(loaded.AgentRadius, 0.8f);
    EXPECT_FLOAT_EQ(loaded.AgentHeight, 1.8f);
    EXPECT_FLOAT_EQ(loaded.AgentMaxClimb, 0.5f);
    EXPECT_FLOAT_EQ(loaded.AgentMaxSlope, 60.0f);
    EXPECT_FLOAT_EQ(loaded.RegionMinSize, 4.0f);
    EXPECT_FLOAT_EQ(loaded.RegionMergeSize, 10.0f);
    EXPECT_FLOAT_EQ(loaded.EdgeMaxLen, 8.0f);
    EXPECT_FLOAT_EQ(loaded.EdgeMaxError, 2.0f);
    EXPECT_FLOAT_EQ(loaded.DetailSampleDist, 3.0f);
    EXPECT_FLOAT_EQ(loaded.DetailSampleMaxError, 0.5f);
    EXPECT_EQ(loaded.VertsPerPoly, 4);

    const auto& loadedGuids = asset.GetSourceGeometryGUIDs();
    ASSERT_EQ(loadedGuids.size(), guids.size());
    for (size_t i = 0; i < guids.size(); ++i)
    {
        EXPECT_EQ(loadedGuids[i], guids[i]) << "GUID mismatch at index " << i;
    }
}

TEST_F(AssetSerializationTest, NavMesh_MalformedJsonFails)
{
    auto filePath = m_TempDir / "malformed.navmesh";
    WriteString(filePath, "{ this is not valid json !!!");

    GUID guid = GUID::Generate();
    NavMeshAsset asset(guid, filePath);
    EXPECT_FALSE(asset.Load());
}

TEST_F(AssetSerializationTest, NavMesh_MissingSettingsKeyFails)
{
    auto filePath = m_TempDir / "nosettings.navmesh";
    WriteString(filePath, R"({"version": 1, "sourceGeometry": []})");

    GUID guid = GUID::Generate();
    NavMeshAsset asset(guid, filePath);
    EXPECT_FALSE(asset.Load());
}

// ===========================================================================
// NavCacheManager Tests
// ===========================================================================

TEST_F(AssetSerializationTest, GridCache_SaveAndLoad)
{
    NavCacheManager::SetProjectRoot(m_TempDir);

    GUID guid = GUID::Generate();
    const uint8 sourceHash[] = {0xAA, 0xBB, 0xCC, 0xDD};
    const uint32 hashSize = sizeof(sourceHash);

    const uint32 kCellCount = 16;
    std::vector<float32> heights(kCellCount);
    for (uint32 i = 0; i < kCellCount; ++i)
        heights[i] = static_cast<float32>(i) * 0.25f;

    ASSERT_TRUE(NavCacheManager::SaveGridCache(guid, sourceHash, hashSize, heights.data(), kCellCount));

    std::vector<float32> loaded;
    ASSERT_TRUE(NavCacheManager::LoadGridCache(guid, sourceHash, hashSize, loaded));

    ASSERT_EQ(loaded.size(), kCellCount);
    for (uint32 i = 0; i < kCellCount; ++i)
    {
        EXPECT_FLOAT_EQ(loaded[i], heights[i]) << "height mismatch at " << i;
    }
}

TEST_F(AssetSerializationTest, GridCache_HashMismatchReturnsFalse)
{
    NavCacheManager::SetProjectRoot(m_TempDir);

    GUID guid = GUID::Generate();
    const uint8 saveHash[] = {0x01, 0x02, 0x03, 0x04};
    const uint8 loadHash[] = {0xFF, 0xFE, 0xFD, 0xFC};
    const uint32 hashSize = sizeof(saveHash);

    const uint32 kCellCount = 4;
    std::vector<float32> heights(kCellCount, 1.0f);

    ASSERT_TRUE(NavCacheManager::SaveGridCache(guid, saveHash, hashSize, heights.data(), kCellCount));

    std::vector<float32> loaded;
    EXPECT_FALSE(NavCacheManager::LoadGridCache(guid, loadHash, hashSize, loaded));
}

TEST_F(AssetSerializationTest, NavMeshCache_SaveAndLoad)
{
    NavCacheManager::SetProjectRoot(m_TempDir);

    GUID guid = GUID::Generate();
    const uint8 sourceHash[] = {0x11, 0x22, 0x33};
    const uint32 hashSize = sizeof(sourceHash);

    std::vector<uint8> blob(128);
    for (size_t i = 0; i < blob.size(); ++i)
        blob[i] = static_cast<uint8>(i & 0xFF);

    ASSERT_TRUE(NavCacheManager::SaveNavMeshCache(guid, sourceHash, hashSize, blob));

    std::vector<uint8> loaded;
    ASSERT_TRUE(NavCacheManager::LoadNavMeshCache(guid, sourceHash, hashSize, loaded));

    ASSERT_EQ(loaded.size(), blob.size());
    EXPECT_EQ(loaded, blob);
}

TEST_F(AssetSerializationTest, NavMeshCache_CorruptMagicReturnsFalse)
{
    NavCacheManager::SetProjectRoot(m_TempDir);

    GUID guid = GUID::Generate();
    const uint8 sourceHash[] = {0x11, 0x22, 0x33};
    const uint32 hashSize = sizeof(sourceHash);
    std::vector<uint8> blob(64, 0xAB);

    ASSERT_TRUE(NavCacheManager::SaveNavMeshCache(guid, sourceHash, hashSize, blob));

    // Corrupt the magic bytes in the cache file
    auto cachePath = m_TempDir / ".Cache" / "Navigation" / (guid.ToString() + ".navmeshcache");
    {
        std::fstream file(cachePath, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(file.is_open());
        file.write("XXXX", 4); // overwrite magic
    }

    std::vector<uint8> loaded;
    EXPECT_FALSE(NavCacheManager::LoadNavMeshCache(guid, sourceHash, hashSize, loaded));
}

TEST_F(AssetSerializationTest, ComputeFileContentHash_Deterministic)
{
    auto filePath = m_TempDir / "hashtest.bin";
    const std::string content = "Hello, navigation world!";
    WriteString(filePath, content);

    uint64 hash1 = NavCacheManager::ComputeFileContentHash(filePath);
    uint64 hash2 = NavCacheManager::ComputeFileContentHash(filePath);

    EXPECT_NE(hash1, 0u);
    EXPECT_EQ(hash1, hash2);
}
