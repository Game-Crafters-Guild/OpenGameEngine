#include <gtest/gtest.h>
#include "AssetCore/Asset.h"
#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include <filesystem>
#include <fstream>

using namespace GameEngine;

namespace {

// Test asset that counts how many times each lifecycle hook fires.
// Used to verify Asset::Reload() invokes PostLoad on success and that
// the default PostLoad implementation is a safe no-op for assets that
// do not override it.
class CountingAsset : public Asset
{
  public:
    CountingAsset(const GUID& guid, const std::filesystem::path& path)
        : Asset(guid, AssetType::Unknown, path) {}

    bool Load() override
    {
        ++LoadCount;
        SetState(AssetState::Loaded);
        return ShouldLoadSucceed;
    }

    bool LoadFromData(const Vector<uint8>& /*data*/) override
    {
        ++LoadCount;
        SetState(AssetState::Loaded);
        return ShouldLoadSucceed;
    }

    void Unload() override
    {
        ++UnloadCount;
        SetState(AssetState::Unloaded);
    }

    void PostLoad() override
    {
        ++PostLoadCount;
    }

    int LoadCount = 0;
    int UnloadCount = 0;
    int PostLoadCount = 0;
    bool ShouldLoadSucceed = true;
};

// Asset that does not override PostLoad; verifies the default no-op
// is callable and does not throw.
class DefaultPostLoadAsset : public Asset
{
  public:
    DefaultPostLoadAsset(const GUID& guid, const std::filesystem::path& path)
        : Asset(guid, AssetType::Unknown, path) {}

    bool Load() override
    {
        SetState(AssetState::Loaded);
        return true;
    }

    bool LoadFromData(const Vector<uint8>& /*data*/) override
    {
        SetState(AssetState::Loaded);
        return true;
    }

    void Unload() override { SetState(AssetState::Unloaded); }
};

class AssetPostLoadHookTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_TestDir = std::filesystem::temp_directory_path() / "asset_post_load_hook_test";
        std::filesystem::create_directories(m_TestDir);
        m_TestFile = m_TestDir / "asset.bin";

        // Reload() relies on Exists() returning true and on the file's
        // last_write_time being updateable. Create a stub file.
        std::ofstream f(m_TestFile, std::ios::binary);
        f << "asset-bytes";
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_TestDir, ec);
    }

    std::filesystem::path m_TestDir;
    std::filesystem::path m_TestFile;
};

} // namespace

TEST_F(AssetPostLoadHookTest, ReloadInvokesPostLoadOnSuccess)
{
    GUID guid = GUID::Generate();
    CountingAsset asset(guid, m_TestFile);

    ASSERT_TRUE(asset.Load());
    // PostLoad is not auto-invoked here because we called Load directly.
    // Bring the asset to a Loaded baseline before exercising Reload.
    EXPECT_EQ(asset.LoadCount, 1);
    EXPECT_EQ(asset.PostLoadCount, 0);

    ASSERT_EQ(asset.Reload(), ReloadOutcome::Reloaded);
    // Reload should run Unload, LoadFromData (via the override), and then PostLoad.
    EXPECT_EQ(asset.UnloadCount, 1);
    EXPECT_EQ(asset.LoadCount, 2);
    EXPECT_EQ(asset.PostLoadCount, 1);

    ASSERT_EQ(asset.Reload(), ReloadOutcome::Reloaded);
    EXPECT_EQ(asset.UnloadCount, 2);
    EXPECT_EQ(asset.LoadCount, 3);
    EXPECT_EQ(asset.PostLoadCount, 2);
}

TEST_F(AssetPostLoadHookTest, ReloadDoesNotInvokePostLoadOnFailure)
{
    GUID guid = GUID::Generate();
    CountingAsset asset(guid, m_TestFile);
    ASSERT_TRUE(asset.Load());

    asset.ShouldLoadSucceed = false;
    EXPECT_EQ(asset.Reload(), ReloadOutcome::Failed);

    EXPECT_EQ(asset.UnloadCount, 1);
    EXPECT_EQ(asset.LoadCount, 2);
    EXPECT_EQ(asset.PostLoadCount, 0); // PostLoad is gated on Load success
}

TEST_F(AssetPostLoadHookTest, DefaultPostLoadIsNoOpAndSafeToInvoke)
{
    GUID guid = GUID::Generate();
    DefaultPostLoadAsset asset(guid, m_TestFile);

    // Direct invocation must not throw on a Loaded asset.
    ASSERT_TRUE(asset.Load());
    EXPECT_NO_THROW(asset.PostLoad());

    // Reload also calls the default PostLoad; must not throw.
    EXPECT_NO_THROW(asset.Reload());
}
