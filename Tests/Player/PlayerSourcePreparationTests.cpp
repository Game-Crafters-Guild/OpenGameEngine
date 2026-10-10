#include "Engine/Build/PlayerSourcePreparation.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>

namespace fs = std::filesystem;
using GameEngine::CollectCustomPlayerSources;
using GameEngine::PreparePlayerSources;

namespace
{
std::string Read(const fs::path& file)
{
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void Write(const fs::path& file, const std::string& text)
{
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
}

using Snapshot = std::map<std::string, std::pair<std::string, fs::file_time_type>>;
Snapshot Capture(const fs::path& root)
{
    Snapshot result;
    for (const auto& entry : fs::recursive_directory_iterator(root))
        if (entry.is_regular_file())
            result.emplace(entry.path().lexically_relative(root).generic_string(),
                           std::make_pair(Read(entry.path()), entry.last_write_time()));
    return result;
}

class PlayerSourcePreparation : public testing::Test
{
protected:
    fs::path Root, Assets, Templates, Generated;
    std::vector<fs::path> Sources;
    std::string Error;

    void SetUp() override
    {
        static std::atomic<unsigned> sequence = 0;
        Root = fs::temp_directory_path() / ("ge_player_sources_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
            std::to_string(sequence++));
        ASSERT_TRUE(fs::create_directory(Root));
        Assets = Root / "Project/Assets";
        Templates = Root / "SDK/templates/Player";
        Generated = Root / "Project/.Build/Player/Source";
        Write(Assets / "Scripts/Native/Gameplay.cpp", "#error must only compile in the native module\n");
        Write(Templates / "DesktopSources.txt", "# Desktop only\nmain.cpp\nPlayerApplication.cpp\nPlayerApplication.h\n");
        Write(Templates / "main.cpp", "#include \"PlayerApplication.h\"\nint main() { return Run(); }\n");
        Write(Templates / "PlayerApplication.h", "int Run();\n");
        Write(Templates / "PlayerApplication.cpp", "int Run() { return 0; }\n");
        Write(Templates / "WebMain.cpp", "#error Web only\n");
        Write(Templates / "WebDistLoader.cpp", "#include <emscripten/wget.h>\n");
    }
    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(Root, ec);
    }
    bool Prepare()
    {
        return PreparePlayerSources(Templates, Generated, CollectCustomPlayerSources(Assets), Sources, Error);
    }
};
}

TEST_F(PlayerSourcePreparation, FirstExportWritesOnlyGeneratedDesktopSources)
{
    const auto before = Capture(Assets);
    ASSERT_TRUE(Prepare()) << Error;
    ASSERT_EQ(Sources.size(), 3u);
    EXPECT_EQ(Sources[0], Generated / "main.cpp");
    EXPECT_EQ(Sources[1], Generated / "PlayerApplication.cpp");
    EXPECT_EQ(Sources[2], Generated / "PlayerApplication.h");
    EXPECT_FALSE(fs::exists(Assets / "Source"));
    EXPECT_FALSE(fs::exists(Generated / "WebDistLoader.cpp"));
    EXPECT_FALSE(fs::exists(Generated / "WebMain.cpp"));
    EXPECT_EQ(Capture(Assets), before);
    EXPECT_TRUE(CollectCustomPlayerSources(Assets).empty());
}

TEST_F(PlayerSourcePreparation, RepeatedExportRefreshesChangedAndNewCompanionsWithoutAssetWrites)
{
    ASSERT_TRUE(Prepare()) << Error;
    const auto before = Capture(Assets);
    const auto unchangedTime = fs::last_write_time(Generated / "PlayerApplication.cpp");
    Write(Templates / "main.cpp", "#include \"NewCompanion.h\"\nint main() { return Value; }\n");
    Write(Templates / "NewCompanion.h", "constexpr int Value = 0;\n");
    Write(Templates / "DesktopSources.txt", "main.cpp\nPlayerApplication.cpp\nPlayerApplication.h\nNewCompanion.h\n");
    // A stale build artifact is not discovered back into either executable or module.
    Write(Generated / "WebDistLoader.cpp", "#error stale browser template\n");
    ASSERT_TRUE(Prepare()) << Error;
    ASSERT_EQ(Sources.size(), 4u);
    EXPECT_EQ(Read(Generated / "main.cpp"), Read(Templates / "main.cpp"));
    EXPECT_EQ(Read(Generated / "NewCompanion.h"), Read(Templates / "NewCompanion.h"));
    EXPECT_EQ(fs::last_write_time(Generated / "PlayerApplication.cpp"), unchangedTime);
    EXPECT_EQ(Capture(Assets), before);
    EXPECT_TRUE(CollectCustomPlayerSources(Assets).empty());
}

TEST_F(PlayerSourcePreparation, CustomApplicationAndLegacyCopiesAreNeverRefreshed)
{
    Write(Assets / "Source/main.cpp", "// deliberately customized legacy entry point\n");
    Write(Assets / "Source/PlayerApplication.cpp", "// custom application\n");
    Write(Assets / "Source/PlayerHdrOptions.h", "// user-owned companion\n");
    Write(Assets / "Source/WebDistLoader.cpp", "// ambiguous old copy remains user-owned\n");
    const auto before = Capture(Assets);
    ASSERT_TRUE(Prepare()) << Error;
    EXPECT_EQ(Sources, CollectCustomPlayerSources(Assets));
    EXPECT_FALSE(fs::exists(Generated));
    EXPECT_EQ(Capture(Assets), before);
    ASSERT_TRUE(Prepare()) << Error;
    EXPECT_EQ(Capture(Assets), before);
    EXPECT_FALSE(fs::exists(Assets / "Source/PlayerApplication.h"));
}

TEST_F(PlayerSourcePreparation, ExplicitCustomPathsOutsideAssetsRemainSupportedAndDeduplicated)
{
    const auto custom = Root / "MyApplication/main.cpp";
    Write(custom, "// independent application\n");
    ASSERT_TRUE(PreparePlayerSources(Root / "NoSDK", Generated,
        {custom, custom.parent_path() / "./main.cpp"}, Sources, Error)) << Error;
    ASSERT_EQ(Sources.size(), 1u);
    EXPECT_EQ(Sources.front(), custom);
    EXPECT_EQ(Read(custom), "// independent application\n");
    EXPECT_FALSE(fs::exists(Generated));
}

TEST_F(PlayerSourcePreparation, CustomHelpersAreAddedButGameplaySourcesStayInTheModule)
{
    Write(Assets / "Source/Helpers/Utility.cpp", "// executable helper\n");
    Write(Assets / "Source/Helpers/Utility.h", "// executable helper header\n");
    Write(Assets / "Source/notes.txt", "not a source\n");
    const auto before = Capture(Assets);
    const auto custom = CollectCustomPlayerSources(Assets);
    ASSERT_EQ(custom.size(), 2u);
    ASSERT_TRUE(Prepare()) << Error;
    EXPECT_EQ(Sources.size(), 5u);
    EXPECT_EQ(Capture(Assets), before);
    for (const auto& source : Sources)
        EXPECT_NE(source.filename(), "Gameplay.cpp");
}

TEST_F(PlayerSourcePreparation, MissingManifestFailsBeforeWritingGeneratedSources)
{
    ASSERT_TRUE(fs::remove(Templates / "DesktopSources.txt"));
    EXPECT_FALSE(Prepare());
    EXPECT_NE(Error.find("matching Editor SDK"), std::string::npos);
    EXPECT_FALSE(fs::exists(Generated));
}

TEST_F(PlayerSourcePreparation, MissingCompanionFailsBeforeAnyCopy)
{
    Write(Templates / "DesktopSources.txt", "main.cpp\nMissing.h\n");
    EXPECT_FALSE(Prepare());
    EXPECT_NE(Error.find("Missing.h"), std::string::npos);
    EXPECT_FALSE(fs::exists(Generated));
}

TEST_F(PlayerSourcePreparation, UnsafeManifestEntriesAreRejectedBeforeAnyCopy)
{
    for (const char* entry : {"../escape.cpp", "/escape.cpp", "C:/escape.cpp",
                              "folder/../escape.cpp", "folder\\escape.cpp", "./main.cpp", "main.cpp;other.cpp"})
    {
        SCOPED_TRACE(entry);
        Write(Templates / "DesktopSources.txt", std::string("main.cpp\n") + entry + "\n");
        EXPECT_FALSE(Prepare());
        EXPECT_NE(Error.find("Invalid desktop Player source"), std::string::npos);
        EXPECT_FALSE(fs::exists(Generated));
    }
}

TEST_F(PlayerSourcePreparation, ManifestSupportsWhitespaceCommentsNestedFilesAndStableDeduplication)
{
    Write(Templates / "Companions/Options.h", "// nested companion\n");
    Write(Templates / "DesktopSources.txt", " \r\n # comment\r\n main.cpp \r\nmain.cpp\nCompanions/Options.h\n");
    ASSERT_TRUE(Prepare()) << Error;
    ASSERT_EQ(Sources.size(), 2u);
    EXPECT_EQ(Sources[0], Generated / "main.cpp");
    EXPECT_EQ(Sources[1], Generated / "Companions/Options.h");
}

TEST_F(PlayerSourcePreparation, EmptyManifestCannotProduceAnEmptyExecutable)
{
    Write(Templates / "DesktopSources.txt", "# no sources\n\n");
    EXPECT_FALSE(Prepare());
    EXPECT_NE(Error.find("nonempty"), std::string::npos);
    EXPECT_FALSE(fs::exists(Generated));
}
