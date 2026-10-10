#include <gtest/gtest.h>
#include "AssetCore/Asset.h"
#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace GameEngine;

namespace {

// An asset whose entire payload is the file's bytes, so "the failed reload left
// the asset alone" is observable in the payload and not merely inferred from
// the state flag.
class TextAsset : public Asset
{
  public:
    TextAsset(const GUID& guid, const std::filesystem::path& path)
        : Asset(guid, AssetType::Unknown, path) {}

    bool Load() override
    {
        std::ifstream file(GetPath(), std::ios::binary);
        if (!file)
        {
            SetState(AssetState::Failed);
            return false;
        }
        Text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        SetState(AssetState::Loaded);
        return true;
    }

    bool LoadFromData(const Vector<uint8>& data) override
    {
        ++LoadFromDataCount;
        if (!AcceptsData)
        {
            SetState(AssetState::Failed);
            return false;
        }
        Text.assign(reinterpret_cast<const char*>(data.data()), data.size());
        SetState(AssetState::Loaded);
        return true;
    }

    void Unload() override
    {
        ++UnloadCount;
        Text.clear();
        SetState(AssetState::Unloaded);
    }

    std::string Text;
    int LoadFromDataCount = 0;
    int UnloadCount = 0;
    bool AcceptsData = true;
};

// The shape UIStyleAsset has: a rejected reload keeps the payload, so the asset
// is still Loaded afterwards and nothing else takes it out of the polling set.
class KeepsPayloadOnRejectAsset : public TextAsset
{
  public:
    using TextAsset::TextAsset;

  protected:
    bool ReloadFromData(const Vector<uint8>& data) override
    {
        return AcceptsData && TextAsset::ReloadFromData(data);
    }
};

class AssetReloadFileGuardTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Dir = std::filesystem::temp_directory_path() /
                ("asset-reload-guard-" + GUID::Generate().ToString());
        std::filesystem::create_directories(m_Dir);
        m_File = m_Dir / "asset.txt";
        m_Stamp = std::filesystem::file_time_type::clock::now() - std::chrono::hours(1);
        Write("first");
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_Dir, ec);
    }

    void Write(const char* text)
    {
        std::ofstream file(m_File, std::ios::binary | std::ios::trunc);
        file << text;
        file.close();
        // Advance the timestamp explicitly: the reload bookkeeping is keyed on
        // last-write-time and two writes in one test can land in the same
        // filesystem tick.
        m_Stamp += std::chrono::seconds(1);
        std::filesystem::last_write_time(m_File, m_Stamp);
    }

    std::filesystem::path m_Dir;
    std::filesystem::path m_File;
    std::filesystem::file_time_type m_Stamp;
};

} // namespace

// The failure this guards: a watcher fires while the file is gone (deleted,
// renamed, its directory removed) and the reload strips a payload the running
// engine is still using.
TEST_F(AssetReloadFileGuardTest, MissingFileLeavesTheAssetLoadedAndRecoversLater)
{
    TextAsset asset(GUID::Generate(), m_File);
    ASSERT_TRUE(asset.Load());

    std::filesystem::remove(m_File);
    EXPECT_EQ(asset.Reload(), ReloadOutcome::Failed);
    EXPECT_TRUE(asset.IsLoaded());
    EXPECT_EQ(asset.Text, "first");
    EXPECT_EQ(asset.UnloadCount, 0);
    EXPECT_EQ(asset.LoadFromDataCount, 0);

    Write("second");
    ASSERT_EQ(asset.Reload(), ReloadOutcome::Reloaded);
    EXPECT_EQ(asset.Text, "second");
}

// An asset that survives a rejected reload stays Loaded, which is exactly the
// state NeedsReload() polls. Without recording the bytes it turned down, the
// hot-reload scan would offer the same broken file every poll for the rest of
// the session: a read, a parse and an error line each time, forever.
TEST_F(AssetReloadFileGuardTest, RejectedBytesAreNotOfferedAgainUntilTheFileChanges)
{
    KeepsPayloadOnRejectAsset asset(GUID::Generate(), m_File);
    ASSERT_TRUE(asset.Load());

    Write("rejected");
    asset.AcceptsData = false;
    EXPECT_EQ(asset.Reload(), ReloadOutcome::Failed);
    EXPECT_TRUE(asset.IsLoaded());
    EXPECT_EQ(asset.Text, "first");
    EXPECT_FALSE(asset.NeedsReload());

    // A later write is a new question and gets asked again.
    Write("fixed");
    asset.AcceptsData = true;
    EXPECT_TRUE(asset.NeedsReload());
    ASSERT_EQ(asset.Reload(), ReloadOutcome::Reloaded);
    EXPECT_EQ(asset.Text, "fixed");
    EXPECT_FALSE(asset.NeedsReload());
}

// A save that truncates before it writes puts the file through zero length, and
// that is the state a watcher reports. Reloading from it replaces the payload
// with nothing — and for a type that keeps its payload through a rejected
// parse, the half-written bytes that follow are then correctly turned down over
// an already-empty payload, so the emptiness lasts until the save completes.
TEST_F(AssetReloadFileGuardTest, EmptyReadIsNotAPayload)
{
    TextAsset asset(GUID::Generate(), m_File);
    ASSERT_TRUE(asset.Load());

    Write("");
    EXPECT_EQ(asset.Reload(), ReloadOutcome::Deferred);
    EXPECT_TRUE(asset.IsLoaded());
    EXPECT_EQ(asset.Text, "first");
    EXPECT_EQ(asset.UnloadCount, 0);
    EXPECT_EQ(asset.LoadFromDataCount, 0);

    // The emptiness was judged, so the scan does not re-offer it every poll.
    EXPECT_FALSE(asset.NeedsReload());

    // The writing half of the save is a change of its own and still arrives.
    Write("second");
    EXPECT_TRUE(asset.NeedsReload());
    ASSERT_EQ(asset.Reload(), ReloadOutcome::Reloaded);
    EXPECT_EQ(asset.Text, "second");
}

// The default ReloadFromData unloads before it decodes, so it cannot keep the
// previous payload when the new bytes are rejected. Types whose consumers hold
// handles into the payload override ReloadFromData to commit a candidate.
TEST_F(AssetReloadFileGuardTest, RejectedBytesUnloadUnderTheDefaultReload)
{
    TextAsset asset(GUID::Generate(), m_File);
    ASSERT_TRUE(asset.Load());

    asset.AcceptsData = false;
    EXPECT_EQ(asset.Reload(), ReloadOutcome::Failed);
    EXPECT_FALSE(asset.IsLoaded());
    EXPECT_EQ(asset.UnloadCount, 1);
    EXPECT_EQ(asset.LoadFromDataCount, 1);
}
