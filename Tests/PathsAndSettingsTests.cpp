#include <gtest/gtest.h>

#include "Core/Application.h"
#include "Core/StandardPaths.h"
#include "Scripting/PathResolver.h"
#include "Rendering/Sky/SkyRenderer.h"
#include "TestTempDir.h"

#include <algorithm>
#include <filesystem>
#include <string>

namespace
{
// Save/chdir/restore, so a test can prove a path is independent of the process working
// directory. Same shape as Tests/Editor/OpenSceneResolutionTests.cpp's guard.
class ScopedCurrentPath
{
  public:
    explicit ScopedCurrentPath(const std::filesystem::path& newCurrentPath)
    {
        std::error_code ec;
        m_PreviousPath = std::filesystem::current_path(ec);
        if (ec)
            return;
        std::filesystem::current_path(newCurrentPath, ec);
        m_Active = !ec;
    }
    ~ScopedCurrentPath()
    {
        if (!m_Active)
            return;
        std::error_code ec;
        std::filesystem::current_path(m_PreviousPath, ec);
    }
    bool IsActive() const { return m_Active; }

  private:
    std::filesystem::path m_PreviousPath;
    bool m_Active{false};
};

static bool ContainsForbiddenChars(const std::string& s)
{
    for (unsigned char c : s)
    {
        if (c < 32)
            return true;
        switch (c)
        {
            case '<':
            case '>':
            case ':':
            case '"':
            case '/':
            case '\\':
            case '|':
            case '?':
            case '*': return true;
            default: break;
        }
    }
    return false;
}
} // namespace

TEST(Paths, SanitizeForFolderName_RemovesForbiddenChars)
{
    const std::string in = " My:Game*Name?  ";
    const std::string out = GameEngine::PathUtils::SanitizeForFolderName(in);
    EXPECT_FALSE(out.empty());
    EXPECT_FALSE(ContainsForbiddenChars(out));
}

TEST(Paths, StandardPaths_GameSavesRoot_IsStableAndSanitized)
{
    const std::string appName = "My:Game*Name?";
    const std::string safe = GameEngine::PathUtils::SanitizeForFolderName(appName);

    const auto p = GameEngine::StandardPaths::GameSavesRoot(appName);
    EXPECT_FALSE(p.empty());
    EXPECT_TRUE(p.is_absolute());

    // The sanitized name namespaces the saves directory, but WHICH segment it is
    // depends on the platform's convention: Windows ends the path at
    // "My Games/<app>", while the Unix layouts nest a saves segment under the
    // per-app data root. Assert the component, not the leaf — the leaf form is
    // covered by the platform-specific tests below.
    const bool namespacedByApp =
        std::any_of(p.begin(), p.end(), [&safe](const std::filesystem::path& part)
                    { return part.string() == safe; });
    EXPECT_TRUE(namespacedByApp) << p.string();

    // Whatever the layout, the raw name never reaches the filesystem.
    EXPECT_EQ(p.string().find(appName), std::string::npos);
}

// Staged engine content is anchored to the executable, never to the working directory.
// A cwd-relative (or cwd-walking) resolver works on a dev machine, because the build tree
// happens to contain a repo Assets/ above the exe, and fails the moment the app is shipped,
// relocated, or simply launched from elsewhere.
TEST(Paths, InstallAssetsRoot_IsAnchoredToTheExecutable)
{
    const auto exeDir = GameEngine::PathUtils::GetExecutableDirectory();
    ASSERT_FALSE(exeDir.empty());

    const auto root = GameEngine::PathUtils::GetInstallAssetsRoot();
    ASSERT_FALSE(root.empty());
    EXPECT_TRUE(root.is_absolute()) << root.string();

    // A test executable is never an app bundle, so its root is exactly <exe>/Assets on every
    // platform — whatever else another target happens to have staged beside the Tests tree.
    EXPECT_EQ(root, (exeDir / "Assets").lexically_normal());
}

// The layout rule on fabricated directory shapes, so the assertions do not depend on what
// this build happened to stage next to the test executable. This flat case is the
// discriminator: a rule that probes for a Resources/Assets sibling resolves to the stray
// directory here, the shape rule never looks at it.
TEST(Paths, InstallAssetsRootFor_FlatLayoutIgnoresAResourcesSibling)
{
    const GameEngine::TestUtils::ScopedTempDir tree(
        GameEngine::TestUtils::MakeUniqueTempDirectory("gameengine-install-root-flat"));
    const auto exeDir = tree.Path() / "Tests";
    ASSERT_TRUE(std::filesystem::create_directories(exeDir));
    ASSERT_TRUE(std::filesystem::create_directories(tree.Path() / "Resources" / "Assets"));

    EXPECT_EQ(GameEngine::PathUtils::InstallAssetsRootFor(exeDir), exeDir / "Assets");
}

// Callers spell a directory with or without a trailing separator; the rule reads the same
// shape either way, and an empty directory yields an empty root like the primitive does.
TEST(Paths, InstallAssetsRootFor_IgnoresATrailingSeparatorAndAnEmptyDirectoryYieldsEmpty)
{
    EXPECT_TRUE(GameEngine::PathUtils::InstallAssetsRootFor({}).empty());
#if defined(__APPLE__)
    const GameEngine::TestUtils::ScopedTempDir tree(
        GameEngine::TestUtils::MakeUniqueTempDirectory("gameengine-install-root-trailing"));
    const auto contentsDir = tree.Path() / "Editor.app" / "Contents";
    ASSERT_TRUE(std::filesystem::create_directories(contentsDir / "MacOS"));
    ASSERT_TRUE(std::filesystem::create_directories(contentsDir / "Resources" / "Assets"));
    const std::filesystem::path exeDirWithSlash((contentsDir / "MacOS").string() + "/");

    EXPECT_EQ(GameEngine::PathUtils::GetBundleResourcesDirectory(exeDirWithSlash), contentsDir / "Resources");
    EXPECT_EQ(GameEngine::PathUtils::InstallAssetsRootFor(exeDirWithSlash),
              contentsDir / "Resources" / "Assets");
#endif
}

#if defined(__APPLE__)
TEST(Paths, InstallAssetsRootFor_BundleUsesStagedResourcesAssets)
{
    const GameEngine::TestUtils::ScopedTempDir tree(
        GameEngine::TestUtils::MakeUniqueTempDirectory("gameengine-install-root-editor-bundle"));
    const auto contentsDir = tree.Path() / "Editor.app" / "Contents";
    ASSERT_TRUE(std::filesystem::create_directories(contentsDir / "MacOS"));
    ASSERT_TRUE(std::filesystem::create_directories(contentsDir / "Resources" / "Assets"));

    EXPECT_EQ(GameEngine::PathUtils::InstallAssetsRootFor(contentsDir / "MacOS"),
              contentsDir / "Resources" / "Assets");
}

// Every bundle (the Editor, the Player, and each game exported from the Player) keeps its
// data in Contents/Resources: codesign seals a file in Contents/MacOS as code and refuses an
// exported game whose content sits there. An Assets directory beside the executable is not
// a second layout the rule falls back to — the discriminator against a rule that probes for
// which of the two exists.
TEST(Paths, InstallAssetsRootFor_BundleIsResourcesAssetsWhateverSitsBesideTheExecutable)
{
    const GameEngine::TestUtils::ScopedTempDir tree(
        GameEngine::TestUtils::MakeUniqueTempDirectory("gameengine-install-root-player-bundle"));
    const auto contentsDir = tree.Path() / "Player.app" / "Contents";
    ASSERT_TRUE(std::filesystem::create_directories(contentsDir / "MacOS" / "Assets"));

    EXPECT_EQ(GameEngine::PathUtils::InstallAssetsRootFor(contentsDir / "MacOS"),
              contentsDir / "Resources" / "Assets");
}
#endif

// game.config, Assets/ and the packaged game's other staged folders share one root. Flat
// layouts keep it beside the executable, whatever stray Resources/ sits next to it.
TEST(Paths, InstallContentRootFor_FlatLayoutIsTheExecutableDirectory)
{
    const GameEngine::TestUtils::ScopedTempDir tree(
        GameEngine::TestUtils::MakeUniqueTempDirectory("gameengine-content-root-flat"));
    const auto exeDir = tree.Path() / "Game";
    ASSERT_TRUE(std::filesystem::create_directories(exeDir));
    ASSERT_TRUE(std::filesystem::create_directories(tree.Path() / "Resources"));

    EXPECT_EQ(GameEngine::PathUtils::InstallContentRootFor(exeDir), exeDir);
    EXPECT_EQ(GameEngine::PathUtils::InstallContentRootFor(exeDir.string() + "/"), exeDir);
    EXPECT_EQ(GameEngine::PathUtils::InstallAssetsRootFor(exeDir),
              GameEngine::PathUtils::InstallContentRootFor(exeDir) / "Assets");
    EXPECT_TRUE(GameEngine::PathUtils::InstallContentRootFor({}).empty());
}

// Inside a macOS bundle the content root is Contents/Resources by shape alone: an exported
// game is resolved by the Player from Contents/MacOS, and the exporter writes to the same
// rule, before anything is staged there.
TEST(Paths, InstallContentRootFor_BundleIsContentsResources)
{
    const GameEngine::TestUtils::ScopedTempDir tree(
        GameEngine::TestUtils::MakeUniqueTempDirectory("gameengine-content-root-bundle"));
    const auto contentsDir = tree.Path() / "My Game.app" / "Contents";
    ASSERT_TRUE(std::filesystem::create_directories(contentsDir / "MacOS"));

#if defined(__APPLE__)
    EXPECT_EQ(GameEngine::PathUtils::InstallContentRootFor(contentsDir / "MacOS"), contentsDir / "Resources");
    EXPECT_EQ(GameEngine::PathUtils::InstallContentRootFor((contentsDir / "MacOS").string() + "/"),
              contentsDir / "Resources");
#else
    EXPECT_EQ(GameEngine::PathUtils::InstallContentRootFor(contentsDir / "MacOS"), contentsDir / "MacOS");
#endif
}

TEST(Paths, InstallAssetsRoot_DoesNotDependOnWorkingDirectory)
{
    const auto before = GameEngine::PathUtils::GetInstallAssetsRoot();
    ASSERT_FALSE(before.empty());

    {
        ScopedCurrentPath cwdGuard(std::filesystem::temp_directory_path());
        ASSERT_TRUE(cwdGuard.IsActive());
        EXPECT_EQ(GameEngine::PathUtils::GetInstallAssetsRoot(), before)
            << "install assets root moved with the working directory";
    }

    EXPECT_EQ(GameEngine::PathUtils::GetInstallAssetsRoot(), before);
}

// The sky's moon art is loaded natively (outside the AssetDatabase), so it is the one place
// an engine texture path is assembled by hand. Pin both halves of that path: it is relative
// to the install assets root, and it does not re-spell the root's own "Assets" segment.
TEST(Paths, SkyMoonTexture_ResolvesUnderInstallAssetsRootNotWorkingDirectory)
{
    const std::filesystem::path relative(GameEngine::Rendering::kSkyMoonFullTextureRelativePath);
    EXPECT_TRUE(relative.is_relative()) << relative.string();
    EXPECT_NE(relative.filename().string(), "");
    EXPECT_TRUE(std::none_of(relative.begin(), relative.end(),
                             [](const std::filesystem::path& part)
                             { return part.string() == "Assets"; }))
        << "the install assets root already ends in Assets: " << relative.string();

    const auto root = GameEngine::PathUtils::GetInstallAssetsRoot();
    ASSERT_FALSE(root.empty());
    // make_preferred on BOTH sides of every comparison below: the root is spelled with the
    // native separator and the relative constant with '/', so an un-normalized join and a
    // normalized one are different path objects even when they name the same file.
    const auto resolved = (root / relative).make_preferred();
    EXPECT_TRUE(resolved.is_absolute()) << resolved.string();

    // The composition must land on a file a staging rule actually produced, not merely be a
    // well-formed string. This target stages the same asset the Editor and Player stage, so
    // deleting either the staging rule or the constant's directory shape turns this red.
    EXPECT_TRUE(std::filesystem::exists(resolved))
        << "no staged sky art at " << resolved.string()
        << " — the constant and the staging rule have diverged";

    ScopedCurrentPath cwdGuard(std::filesystem::temp_directory_path());
    ASSERT_TRUE(cwdGuard.IsActive());
    EXPECT_EQ((GameEngine::PathUtils::GetInstallAssetsRoot() / relative).make_preferred(), resolved)
        << "moon texture path followed the working directory";
}

// A macOS app bundle keeps Contents/MacOS code-only and stages everything else as a
// resource; the bundle primitive is structural, and every other layout is exe-relative.
TEST(Paths, BundleResourcesDirectory_ResolvesOnlyFromABundleExecutableDirectory)
{
    namespace fs = std::filesystem;
    const fs::path app = fs::temp_directory_path() / "GE_PathsBundleProbe" / "Probe.app";
    const fs::path bundleResources = GameEngine::PathUtils::GetBundleResourcesDirectory(app / "Contents" / "MacOS");
#if defined(__APPLE__)
    EXPECT_EQ(bundleResources, app / "Contents" / "Resources");
#else
    EXPECT_TRUE(bundleResources.empty()) << bundleResources;
#endif
    EXPECT_TRUE(GameEngine::PathUtils::GetBundleResourcesDirectory(fs::temp_directory_path() / "bin").empty());
    EXPECT_TRUE(GameEngine::PathUtils::GetBundleResourcesDirectory(app / "Contents").empty());
    EXPECT_TRUE(GameEngine::PathUtils::GetBundleResourcesDirectory({}).empty());
}

// The engine managed directory (the csproj EngineBinDir) is Contents/Resources/Managed
// inside a bundle that stages one, and the executable directory everywhere else.
TEST(Paths, EngineManagedDirectory_IsTheBundleManagedResourcesInsideAnAppBundle)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "GE_EngineManagedDirProbe";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path exeDir = root / "Probe.app" / "Contents" / "MacOS";
    const fs::path managedDir = root / "Probe.app" / "Contents" / "Resources" / "Managed";
    ASSERT_TRUE(fs::create_directories(exeDir, ec)) << ec.message();

    // A bundle without staged managed resources (scripting disabled) stays exe-relative.
    EXPECT_EQ(GameEngine::ScriptingPaths::ResolveEngineManagedDirectoryFrom(exeDir), exeDir);

    ASSERT_TRUE(fs::create_directories(managedDir, ec)) << ec.message();
#if defined(__APPLE__)
    EXPECT_EQ(GameEngine::ScriptingPaths::ResolveEngineManagedDirectoryFrom(exeDir), managedDir);
#else
    EXPECT_EQ(GameEngine::ScriptingPaths::ResolveEngineManagedDirectoryFrom(exeDir), exeDir);
#endif

    // A flat layout (Windows, Linux, macOS test binaries) is never redirected, even with a
    // sibling directory that happens to be called Managed.
    const fs::path flatDir = root / "bin";
    ASSERT_TRUE(fs::create_directories(flatDir / "Managed", ec)) << ec.message();
    EXPECT_EQ(GameEngine::ScriptingPaths::ResolveEngineManagedDirectoryFrom(flatDir), flatDir);

    fs::remove_all(root, ec);
}

#if defined(_WIN32)
TEST(Paths, StandardPaths_GameSavesRoot_UsesMyGamesConventionalSegment)
{
    const auto p = GameEngine::StandardPaths::GameSavesRoot("GameEngineTest");
    const std::string s = p.string();
    // Even if Documents falls back (rare), we still append "My Games".
    EXPECT_NE(s.find("My Games"), std::string::npos);
}
#endif

