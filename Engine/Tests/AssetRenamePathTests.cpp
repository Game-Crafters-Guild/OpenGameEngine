// AssetManager::RenameAssetPath moves an asset's file binding after the caller
// moved the file: the registry keeps the GUID under the new path and a resident
// instance follows, so a reload reads the new file instead of the vanished one.
#include <gtest/gtest.h>

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "AssetCore/GUID.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

namespace
{

using namespace GameEngine;

std::filesystem::path MakeUniqueTempDir()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = std::filesystem::temp_directory_path() / ("ge_asset_rename_" + std::to_string(stamp));
    std::filesystem::create_directories(dir);
    return dir;
}

// BinaryAsset (.bin) is the simplest device-free registered asset type.
std::filesystem::path WriteBinaryAsset(const std::filesystem::path& dir, const char* name)
{
    const std::filesystem::path path = dir / name;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << "ge-asset-rename-payload";
    return path;
}

SharedPtr<Asset> LoadSynchronously(AssetManager& assets, const GUID& guid)
{
    std::atomic<bool> done{false};
    SharedPtr<Asset> loaded;
    const AssetLoadHandle handle = assets.LoadAsset(
        guid,
        [&](Result<SharedPtr<Asset>, AssetError> result)
        {
            if (result.IsOk())
                loaded = result.Value();
            done.store(true, std::memory_order_release);
        },
        AssetLoadPriority::High);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return loaded;
}

class AssetRenamePathTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Dir = MakeUniqueTempDir();
        m_Pool = std::make_unique<JobSystem::WorkStealingThreadPool>(2);
        m_Assets = std::make_unique<AssetManager>();
        ASSERT_TRUE(m_Assets->Initialize(m_Pool.get()));
    }

    void TearDown() override
    {
        m_Assets.reset();
        m_Pool.reset();
        std::error_code ec;
        std::filesystem::remove_all(m_Dir, ec);
    }

    void RegisterSource()
    {
        AssetSourceDesc source;
        source.Alias = "rename";
        source.Root = m_Dir;
        source.DerivedIdentity = true;
        source.RequiresScan = true;
        ASSERT_TRUE(m_Assets->RegisterSource(source));
        m_Assets->WaitForStartupScan("rename");
    }

    std::filesystem::path m_Dir;
    std::unique_ptr<JobSystem::WorkStealingThreadPool> m_Pool;
    std::unique_ptr<AssetManager> m_Assets;
};

} // namespace

TEST_F(AssetRenamePathTests, ResidentAssetFollowsItsRenamedFile)
{
    const std::filesystem::path oldPath = WriteBinaryAsset(m_Dir, "before.bin");
    RegisterSource();
    const GUID guid = m_Assets->ResolveAssetGuid(oldPath, "rename");
    ASSERT_FALSE(guid.IsNull());
    SharedPtr<Asset> asset = LoadSynchronously(*m_Assets, guid);
    ASSERT_NE(asset, nullptr);
    // Paths come back in the registry's normalized form (case-folded on
    // case-insensitive volumes), so compare by identity of the file they name.
    ASSERT_TRUE(std::filesystem::equivalent(asset->GetPath(), oldPath));

    const std::filesystem::path newPath = m_Dir / "after.bin";
    std::filesystem::rename(oldPath, newPath);
    ASSERT_TRUE(m_Assets->RenameAssetPath(oldPath, newPath));

    EXPECT_EQ(m_Assets->GetRegistry().GetAssetGUID(newPath), guid);
    EXPECT_TRUE(m_Assets->GetRegistry().GetAssetGUID(oldPath).IsNull());
    EXPECT_TRUE(std::filesystem::equivalent(asset->GetPath(), newPath))
        << "the resident instance must read the moved file on reload; it names " << asset->GetPath();
    EXPECT_EQ(m_Assets->ReloadAssetNow(guid), ReloadOutcome::Reloaded);
}

TEST_F(AssetRenamePathTests, UnregisteredPathIsRefused)
{
    RegisterSource();
    const std::filesystem::path stray = WriteBinaryAsset(m_Dir, "stray.bin");
    const std::filesystem::path moved = m_Dir / "moved.bin";
    std::filesystem::rename(stray, moved);
    EXPECT_FALSE(m_Assets->RenameAssetPath(stray, moved));
}
