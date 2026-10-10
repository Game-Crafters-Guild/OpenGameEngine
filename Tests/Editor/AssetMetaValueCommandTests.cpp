// The undoable import-setting write, driven over a live registry.
//
// The command is built from the path the inspector is showing, but it has to
// survive the asset moving underneath it: a rename between the edit and the
// undo leaves the old path answering to nothing, and an undo aimed there would
// silently do nothing while the user watched the value stay changed. So the
// command keys on the asset's GUID and asks the registry where that asset is
// each time it writes.
//
// What the write does with the path is the inspector's business (store write
// plus a recook) and is not re-tested here; these tests record the path and the
// value the command hands over. The registry's own kv persistence is covered by
// the asset-database suites.

#include <gtest/gtest.h>

#include "UndoRedo/SetAssetMetaValueCommand.h"

#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"

#include "../TestTempDir.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace GameEngine;
using GameEngine::TestUtils::MakeUniqueTempDirectory;

namespace
{

constexpr const char* kMetaKey = "assets.texture.usage";

struct RecordedWrite
{
    std::filesystem::path Path;
    std::string Value;
};

// The registry hands back its own spelling of a path — forward slashes, and
// case-folded where the filesystem is — so the tests compare paths the way the
// registry keys them rather than the way the test wrote them.
std::string AsRegistrySpelling(const std::filesystem::path& path)
{
    std::string text = path.generic_string();
    for (char& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

} // namespace

class AssetMetaValueCommandTests : public ::testing::Test
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

        s_Root = MakeUniqueTempDirectory("asset_meta_value_command");
        std::filesystem::create_directories(s_Root);

        AssetSourceDesc source{};
        source.Alias = "assetmetacommand";
        source.Root = s_Root;
        ASSERT_TRUE(engine.GetAssetManager().RegisterSource(source));
    }

    static void TearDownTestSuite()
    {
        std::error_code ec;
        std::filesystem::remove_all(s_Root, ec);
    }

    static AssetRegistry& Registry()
    {
        return EngineCore::GetInstance().GetAssetManager().GetRegistry();
    }

    // A file the registry hands out a GUID for. Content is irrelevant: the
    // command reads and writes metadata and never loads the asset.
    static std::filesystem::path WriteAsset(const std::string& name)
    {
        const std::filesystem::path path = s_Root / name;
        std::ofstream out(path, std::ios::binary);
        out << "not a real texture";
        out.close();
        const GUID guid = EngineCore::GetInstance().GetAssetManager().ResolveAssetGuid(path);
        EXPECT_FALSE(guid.IsNull()) << "the registry did not take " << path.string();
        return path;
    }

    static std::filesystem::path s_Root;
};

std::filesystem::path AssetMetaValueCommandTests::s_Root;

TEST_F(AssetMetaValueCommandTests, UndoAfterARenameWritesToTheAssetAtItsNewPath)
{
    const std::filesystem::path original = WriteAsset("renamed_between_do_and_undo.png");

    std::vector<RecordedWrite> writes;
    Editor::SetAssetMetaValueCommand command(
        original, kMetaKey, "packed",
        [&writes](const std::filesystem::path& path, const std::string& value)
        { writes.push_back({path, value}); },
        {});

    command.Do();
    ASSERT_EQ(writes.size(), 1u);
    EXPECT_EQ(AsRegistrySpelling(writes.back().Path), AsRegistrySpelling(original));
    EXPECT_EQ(writes.back().Value, "packed");

    const std::filesystem::path renamed = s_Root / "renamed_between_do_and_undo_now_this.png";
    std::error_code ec;
    std::filesystem::rename(original, renamed, ec);
    ASSERT_FALSE(ec) << ec.message();
    ASSERT_TRUE(Registry().TryRenameAssetPath(original, renamed));

    command.Undo();
    ASSERT_EQ(writes.size(), 2u);
    EXPECT_EQ(AsRegistrySpelling(writes.back().Path), AsRegistrySpelling(renamed))
        << "the undo has to follow the asset, not write to the path it was built from";
    // The asset carried no value for this key, and the documented undo for that
    // is the empty string: every reader of these keys treats it as absent.
    EXPECT_EQ(writes.back().Value, std::string());
}

TEST_F(AssetMetaValueCommandTests, RedoWritesTheValueTheCommandWroteNotTheOneUndoRestored)
{
    const std::filesystem::path asset = WriteAsset("redo_writes_its_own_value.png");

    std::vector<RecordedWrite> writes;
    Editor::SetAssetMetaValueCommand command(
        asset, kMetaKey, "packed",
        [&writes](const std::filesystem::path& path, const std::string& value)
        { writes.push_back({path, value}); },
        {});

    command.Do();
    command.Undo();
    command.Redo();

    ASSERT_EQ(writes.size(), 3u);
    EXPECT_EQ(writes[0].Value, "packed");
    EXPECT_EQ(writes[1].Value, std::string());
    EXPECT_EQ(writes[2].Value, "packed");
}

TEST_F(AssetMetaValueCommandTests, AnAssetDeletedBetweenDoAndUndoTakesNoWrite)
{
    const std::filesystem::path asset = WriteAsset("deleted_between_do_and_undo.png");

    std::vector<RecordedWrite> writes;
    Editor::SetAssetMetaValueCommand command(
        asset, kMetaKey, "packed",
        [&writes](const std::filesystem::path& path, const std::string& value)
        { writes.push_back({path, value}); },
        {});

    command.Do();
    ASSERT_EQ(writes.size(), 1u);

    ASSERT_TRUE(Registry().TryUnregisterAssetByPath(asset));
    command.Undo();
    EXPECT_EQ(writes.size(), 1u) << "with the asset gone there is no path to write to";
}
