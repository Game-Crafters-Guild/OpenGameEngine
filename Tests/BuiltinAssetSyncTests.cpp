#include "Startup/BuiltinAssetSync.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace GameEngine::Editor::Startup;
namespace fs = std::filesystem;

namespace
{
void WriteFile(const fs::path& path, const std::string& content)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

std::string ReadFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// One editor launch's pass over its mirror: refresh the shipped shaders, then prune
// against the manifest beside that mirror.
bool SyncShaderMirror(const fs::path& install, const fs::path& mirror, std::string* outError)
{
    std::vector<std::string> shipped;
    BuiltinSyncStats stats;
    return RefreshOwnedTree(install, mirror, {.RelativeRoot = "Shaders", .AssetKind = "shader"},
                            shipped, stats, outError) &&
           PruneStaleOwnedFiles(mirror, BuiltinManifestPathFor(mirror), shipped, stats, outError);
}

class BuiltinAssetSyncTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        m_Root = fs::temp_directory_path() / "GameEngineTests" / "BuiltinAssetSync" / info->name();
        fs::remove_all(m_Root);
        m_Src = m_Root / "install";
        m_Dst = m_Root / "user";
        m_Manifest = m_Root / "EditorAssets.builtin-manifest";
        fs::create_directories(m_Src);
        fs::create_directories(m_Dst);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_Root, ec);
    }

    fs::path m_Root;
    fs::path m_Src;
    fs::path m_Dst;
    fs::path m_Manifest;
};
} // namespace

TEST_F(BuiltinAssetSyncTest, CopyMissingSeedsButNeverOverwrites)
{
    WriteFile(m_Src / "UI/theme.css", "shipped");
    WriteFile(m_Dst / "UI/theme.css", "user-edited");
    WriteFile(m_Src / "UI/new.css", "new-default");

    std::string err;
    ASSERT_TRUE(CopyMissingFilesRecursive(m_Src, m_Dst, &err)) << err;

    EXPECT_EQ(ReadFile(m_Dst / "UI/theme.css"), "user-edited");
    EXPECT_EQ(ReadFile(m_Dst / "UI/new.css"), "new-default");
}

TEST_F(BuiltinAssetSyncTest, RefreshCopiesNewOverwritesChangedSkipsIdentical)
{
    WriteFile(m_Src / "Shaders/a.glsl", "v2");
    WriteFile(m_Src / "Shaders/Includes/b.glsl", "same");
    WriteFile(m_Src / "Shaders/c.glsl", "brand-new");
    WriteFile(m_Dst / "Shaders/a.glsl", "v1");
    WriteFile(m_Dst / "Shaders/Includes/b.glsl", "same");
    WriteFile(m_Dst / "Shaders/user-extra.glsl", "keep-me");

    std::string err;
    std::vector<std::string> shipped;
    BuiltinSyncStats stats;
    ASSERT_TRUE(RefreshOwnedTree(m_Src, m_Dst, {.RelativeRoot = "Shaders", .AssetKind = "shader"},
                                 shipped, stats, &err))
        << err;

    EXPECT_EQ(ReadFile(m_Dst / "Shaders/a.glsl"), "v2");
    EXPECT_EQ(ReadFile(m_Dst / "Shaders/c.glsl"), "brand-new");
    EXPECT_EQ(ReadFile(m_Dst / "Shaders/user-extra.glsl"), "keep-me");
    EXPECT_EQ(stats.Copied, 2u);
    EXPECT_EQ(stats.SkippedIdentical, 1u);
    EXPECT_EQ(shipped.size(), 3u);
    EXPECT_NE(std::find(shipped.begin(), shipped.end(), "Shaders/Includes/b.glsl"), shipped.end());
}

TEST_F(BuiltinAssetSyncTest, NonRecursiveExtensionFilteredRefresh)
{
    WriteFile(m_Src / "RenderPipelines/main.rendergraph", "rg");
    WriteFile(m_Src / "RenderPipelines/notes.txt", "not-shipped-kind");
    WriteFile(m_Src / "RenderPipelines/Nested/deep.rendergraph", "nested-ignored");

    std::string err;
    std::vector<std::string> shipped;
    BuiltinSyncStats stats;
    ASSERT_TRUE(RefreshOwnedTree(m_Src, m_Dst,
                                 {.RelativeRoot = "RenderPipelines",
                                  .AssetKind = "render pipeline",
                                  .Recursive = false,
                                  .Extensions = {".rendergraph", ".renderpipeline"}},
                                 shipped, stats, &err))
        << err;

    EXPECT_TRUE(fs::exists(m_Dst / "RenderPipelines/main.rendergraph"));
    EXPECT_FALSE(fs::exists(m_Dst / "RenderPipelines/notes.txt"));
    EXPECT_FALSE(fs::exists(m_Dst / "RenderPipelines/Nested/deep.rendergraph"));
    EXPECT_EQ(shipped, (std::vector<std::string>{"RenderPipelines/main.rendergraph"}));
}

TEST_F(BuiltinAssetSyncTest, PruneRemovesOnlyFormerlyShippedFiles)
{
    WriteFile(m_Dst / "UI/removed.css", "stale-builtin");
    WriteFile(m_Dst / "UI/kept.css", "still-shipped");
    WriteFile(m_Dst / "UI/user-custom.css", "user-created");
    WriteFile(m_Manifest, "# header\nUI/removed.css\nUI/kept.css\n");

    std::string err;
    BuiltinSyncStats stats;
    ASSERT_TRUE(PruneStaleOwnedFiles(m_Dst, m_Manifest, {"UI/kept.css"}, stats, &err)) << err;

    EXPECT_FALSE(fs::exists(m_Dst / "UI/removed.css"));
    EXPECT_TRUE(fs::exists(m_Dst / "UI/kept.css"));
    EXPECT_TRUE(fs::exists(m_Dst / "UI/user-custom.css"));
    EXPECT_EQ(stats.RemovedStale, 1u);

    const std::string manifest = ReadFile(m_Manifest);
    EXPECT_NE(manifest.find("UI/kept.css"), std::string::npos);
    EXPECT_EQ(manifest.find("UI/removed.css"), std::string::npos);
}

TEST_F(BuiltinAssetSyncTest, PruneWithoutManifestDeletesNothingAndWritesManifest)
{
    WriteFile(m_Dst / "UI/anything.css", "present");

    std::string err;
    BuiltinSyncStats stats;
    ASSERT_TRUE(PruneStaleOwnedFiles(m_Dst, m_Manifest, {"UI/shipped.css"}, stats, &err)) << err;

    EXPECT_TRUE(fs::exists(m_Dst / "UI/anything.css"));
    EXPECT_EQ(stats.RemovedStale, 0u);
    EXPECT_NE(ReadFile(m_Manifest).find("UI/shipped.css"), std::string::npos);
}

TEST_F(BuiltinAssetSyncTest, PruneIgnoresUnsafeManifestEntries)
{
    WriteFile(m_Root / "outside.txt", "outside-dst");
    WriteFile(m_Dst / "UI/inside.css", "inside");
    WriteFile(m_Manifest, "../outside.txt\nC:/Windows/system.ini\nUI/inside.css\n");

    std::string err;
    BuiltinSyncStats stats;
    ASSERT_TRUE(PruneStaleOwnedFiles(m_Dst, m_Manifest, {}, stats, &err)) << err;

    EXPECT_TRUE(fs::exists(m_Root / "outside.txt"));
    EXPECT_FALSE(fs::exists(m_Dst / "UI/inside.css")); // safe entry, no longer shipped
    EXPECT_EQ(stats.RemovedStale, 1u);
}

TEST_F(BuiltinAssetSyncTest, PruneShippedSetMatchingIsCaseInsensitive)
{
    WriteFile(m_Dst / "UI/Theme.css", "shipped-with-case-change");
    WriteFile(m_Manifest, "UI/Theme.css\n");

    std::string err;
    BuiltinSyncStats stats;
    ASSERT_TRUE(PruneStaleOwnedFiles(m_Dst, m_Manifest, {"ui/theme.css"}, stats, &err)) << err;

    EXPECT_TRUE(fs::exists(m_Dst / "UI/Theme.css"));
    EXPECT_EQ(stats.RemovedStale, 0u);
}

TEST_F(BuiltinAssetSyncTest, SecondFullPassIsAllIdenticalAndPrunesNothing)
{
    WriteFile(m_Src / "Shaders/a.glsl", "a");
    WriteFile(m_Src / "UI/theme.css", "t");

    const auto runPass = [&](BuiltinSyncStats& stats) {
        std::string err;
        std::vector<std::string> shipped;
        bool ok = CopyMissingFilesRecursive(m_Src, m_Dst, &err) &&
                  RefreshOwnedTree(m_Src, m_Dst, {.RelativeRoot = "Shaders", .AssetKind = "shader"},
                                   shipped, stats, &err) &&
                  RefreshOwnedTree(m_Src, m_Dst, {.RelativeRoot = "UI", .AssetKind = "UI asset"},
                                   shipped, stats, &err) &&
                  PruneStaleOwnedFiles(m_Dst, m_Manifest, shipped, stats, &err);
        EXPECT_TRUE(ok) << err;
    };

    BuiltinSyncStats first;
    runPass(first);
    EXPECT_EQ(first.SkippedIdentical, 2u); // CopyMissing seeded them; refresh sees identical copies
    EXPECT_EQ(first.RemovedStale, 0u);

    BuiltinSyncStats second;
    runPass(second);
    EXPECT_EQ(second.Copied, 0u);
    EXPECT_EQ(second.SkippedIdentical, 2u);
    EXPECT_EQ(second.RemovedStale, 0u);

    // Simulate an upgrade that drops a builtin: it must be pruned from the user copy.
    fs::remove(m_Src / "UI/theme.css");
    BuiltinSyncStats third;
    runPass(third);
    EXPECT_EQ(third.RemovedStale, 1u);
    EXPECT_FALSE(fs::exists(m_Dst / "UI/theme.css"));
}

TEST_F(BuiltinAssetSyncTest, EachMirrorPrunesAgainstItsOwnManifest)
{
    // The default mirror, <userData>/EditorAssets, keeps its manifest where it always was.
    EXPECT_EQ(BuiltinManifestPathFor(m_Root / "EditorAssets"), m_Root / "EditorAssets.builtin-manifest");
    // An override written with a trailing separator names the same mirror.
    EXPECT_EQ(BuiltinManifestPathFor(m_Root / "IsolatedAssets" / ""), m_Root / "IsolatedAssets.builtin-manifest");

    // Two editors share one user data root; the second points GE_EDITOR_ASSETS_ROOT at a mirror
    // of its own and runs a build that never shipped retired.glsl.
    const fs::path installA = m_Root / "installA";
    const fs::path installB = m_Root / "installB";
    const fs::path mirrorA = m_Root / "EditorAssets";
    const fs::path mirrorB = m_Root / "IsolatedAssets";
    WriteFile(installA / "Shaders/kept.glsl", "kept");
    WriteFile(installA / "Shaders/retired.glsl", "retired");
    WriteFile(installB / "Shaders/kept.glsl", "kept");
    WriteFile(mirrorB / "Shaders/retired.glsl", "user-created");

    std::string err;
    ASSERT_TRUE(SyncShaderMirror(installA, mirrorA, &err)) << err;
    ASSERT_TRUE(SyncShaderMirror(installB, mirrorB, &err)) << err;
    EXPECT_TRUE(fs::exists(mirrorB / "Shaders/retired.glsl")); // A shipped it, B never did

    // A's next build drops retired.glsl: A's copy is pruned, whatever B's launch recorded.
    fs::remove(installA / "Shaders/retired.glsl");
    ASSERT_TRUE(SyncShaderMirror(installA, mirrorA, &err)) << err;
    EXPECT_FALSE(fs::exists(mirrorA / "Shaders/retired.glsl"));
}
