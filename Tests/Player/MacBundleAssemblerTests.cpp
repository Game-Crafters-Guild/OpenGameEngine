#include "Core/Application.h"
#include "Engine/Build/BuildPipeline.h"
#include "Engine/Build/MacBundleAssembler.h"
#include "Scripting/PathResolver.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{

void WriteFixture(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << "fixture";
}

void WriteFixture(const std::filesystem::path& path, const std::string& content)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << content;
}

// A CoreCLR export on macOS: the SDK's Player.app template (its Contents/Resources/Managed is the
// Player's own runtime-only set, staged for running the Player in place), the editor bundle whose
// engine managed directory the game's scripts compiled against, and the scripts compile output.
struct MacManagedExportFixture
{
    std::filesystem::path Template;
    std::filesystem::path EditorManaged;
    std::filesystem::path Scratch;
    std::filesystem::path Bundle;
    GameEngine::BuildSettings Settings;

    explicit MacManagedExportFixture(const std::filesystem::path& root)
    {
        Template = root / "SDK/templates/Player.app";
        WriteFixture(Template / "Contents/MacOS/Player");
        WriteFixture(Template / "Contents/Frameworks/libEngine.dylib");
        for (const char* name : {"GameEngine.CoreBridge.dll", "GameEngine.HotReload.dll",
                                 "GameEngine.Scripting.ABI.dll", "GameEngine.ECS.ABI.dll"})
            WriteFixture(Template / "Contents/Resources/Managed" / name, "template-copy");

        const std::filesystem::path editorMacOS = root / "Editor.app/Contents/MacOS";
        std::filesystem::create_directories(editorMacOS);
        EditorManaged = root / "Editor.app/Contents/Resources/Managed";
        for (const char* name :
             {"GameEngine.CoreBridge.dll", "GameEngine.HotReload.dll", "GameEngine.HotReload.runtimeconfig.json",
              "GameEngine.Scripting.Runtime.dll", "GameEngine.Scripting.ABI.dll", "GameEngine.ECS.ABI.dll",
              "GameEngine.Input.ABI.dll", "GameEngine.Editor.Scripting.ABI.dll", "GameEngine.Editor.Managed.dll"})
            WriteFixture(EditorManaged / name, "editor-copy");
        WriteFixture(EditorManaged / "GameEngine.Scripting.Runtime.deps.json", R"({"targets":{".NETCoreApp,Version=v9.0":{
            "GameEngine.Scripting.Runtime/1.0.0":{"runtime":{"GameEngine.Scripting.Runtime.dll":{}}},
            "GameEngine.Scripting.ABI/1.0.0":{"runtime":{"GameEngine.Scripting.ABI.dll":{}}},
            "GameEngine.ECS.ABI/1.0.0":{"runtime":{"GameEngine.ECS.ABI.dll":{}}}}}})");

        Scratch = root / "Scratch";
        WriteFixture(Scratch / "Managed/GameEngine.Scripts.dll", "user-scripts");

        Bundle = root / "Out/Game.app";
        Settings.projectRoot = root / "Project"; // no native C++ user scripts
        Settings.runtimeDepsPath = editorMacOS;
        Settings.playerConfig.gameName = "Game";
    }

    bool Export(std::vector<std::string>& errors) const
    {
        std::vector<std::string> warnings;
        return GameEngine::MacBundleAssembler::PrepareBundle(Template, Bundle, errors) &&
               GameEngine::MacBundleAssembler::InjectRuntime(Settings, Bundle, Scratch, {}, errors, warnings);
    }
};

std::string ReadFixture(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Package C, C++ and Objective-C++ sources and headers. The template layout check
// (Apps/Player/Tests/VerifyMacPlayerTemplateLayout.cmake) matches the same set.
constexpr std::array<std::string_view, 8> kPackageSourceExtensions = {".c", ".cc", ".cpp", ".cxx",
                                                                     ".mm", ".h", ".hpp", ".inl"};

// Files only an engine developer's tree holds: package native modules of Editor kind, package
// sources and headers, and the marker naming the checkout the packages were staged from.
bool IsDevelopmentOnlyFile(const std::filesystem::path& path)
{
    const std::string name = path.filename().string();
    const std::string extension = path.extension().string();
    return name.find(".Editor.") != std::string::npos || name == "EnginePackageAuthoringRoot.txt" ||
           std::ranges::find(kPackageSourceExtensions, std::string_view(extension)) != kPackageSourceExtensions.end();
}

} // namespace

TEST(MacBundleAssembler, RemovesRuntimeArtifactsFromPreviouslyLaunchedTemplate)
{
    namespace fs = std::filesystem;
    GameEngine::TestUtils::ScopedTempDir temp(GameEngine::TestUtils::MakeUniqueTempDirectory("ge_mac_bundle"));
    const fs::path source = temp.Path() / "Player.app";
    const fs::path destination = temp.Path() / "Game.app";
    for (const char* path : {"game.log", "Tex/texture.ktx2", ".Cache/Shaders/cached.spv"})
        WriteFixture(source / "Contents/MacOS" / path);
    WriteFixture(source / "Contents/Resources/Assets/keep.png");
    WriteFixture(source / "Contents/Frameworks/libvulkan.dylib");
    fs::create_symlink("libvulkan.dylib", source / "Contents/Frameworks/libvulkan.1.dylib");
    WriteFixture(destination / "Contents/MacOS/old-build.txt");

    std::vector<std::string> errors;
    ASSERT_TRUE(GameEngine::MacBundleAssembler::PrepareBundle(source, destination, errors));
    EXPECT_TRUE(errors.empty());
    EXPECT_FALSE(fs::exists(destination / "Contents/MacOS/game.log"));
    EXPECT_FALSE(fs::exists(destination / "Contents/MacOS/Tex"));
    EXPECT_FALSE(fs::exists(destination / "Contents/MacOS/.Cache"));
    EXPECT_FALSE(fs::exists(destination / "Contents/MacOS/old-build.txt"));
    EXPECT_TRUE(fs::exists(destination / "Contents/Resources/Assets/keep.png"));
    EXPECT_TRUE(fs::is_symlink(destination / "Contents/Frameworks/libvulkan.1.dylib"));
    EXPECT_TRUE(fs::exists(source / "Contents/MacOS/game.log")) << "the template was modified";
}

// The Player's own build stages every engine package beside its executable for development runs
// (ge_stage_packages): the packages' sources, their editor-only modules and the marker naming the
// build machine's checkout. A game carries the packages it uses under its content root,
// Contents/Resources/Packages with packages.index, staged by the build pipeline; the bundle it ships
// holds nothing of the development copy in Contents/MacOS.
TEST(MacBundleAssembler, LeavesThePlayersDevelopmentPackagesOut)
{
    namespace fs = std::filesystem;
    GameEngine::TestUtils::ScopedTempDir temp(GameEngine::TestUtils::MakeUniqueTempDirectory("ge_mac_bundle_pkgs"));
    const fs::path source = temp.Path() / "Player.app";
    const fs::path destination = temp.Path() / "Game.app";
    for (const char* path : {"Player", "Packages/EnginePackageAuthoringRoot.txt", "Packages/git-vcs/package.json",
                             "Packages/git-vcs/Binaries/Editor/macos-arm64-0123abcd/libGitVcs.Editor.so",
                             "Packages/eztree/Assets/Shaders/eztree_wind.vert",
                             "Packages/eztree/Binaries/Runtime/macos-arm64-0123abcd/libEztree.so"})
        WriteFixture(source / "Contents/MacOS" / path);
    for (const std::string_view extension : kPackageSourceExtensions)
        WriteFixture(source / "Contents/MacOS/Packages/git-vcs/Editor" / ("GitIntegration" + std::string(extension)));
    WriteFixture(source / "Contents/Resources/Assets/keep.png");

    std::vector<std::string> errors;
    ASSERT_TRUE(GameEngine::MacBundleAssembler::PrepareBundle(source, destination, errors));
    EXPECT_TRUE(errors.empty());
    EXPECT_FALSE(fs::exists(destination / "Contents/MacOS/Packages"));
    EXPECT_TRUE(fs::exists(destination / "Contents/MacOS/Player"));
    EXPECT_TRUE(fs::exists(destination / "Contents/Resources/Assets/keep.png"));
    for (const auto& entry : fs::recursive_directory_iterator(destination))
        EXPECT_FALSE(IsDevelopmentOnlyFile(entry.path())) << entry.path().lexically_relative(destination);
    EXPECT_TRUE(fs::exists(source / "Contents/MacOS/Packages/EnginePackageAuthoringRoot.txt"))
        << "the template was modified";
}

// An exported game's content goes to the content root the Player resolves from Contents/MacOS.
// codesign --deep treats everything under Contents/MacOS as code: there a mesh LOD cache
// (Assets/.lod, a directory whose name reads as a nested bundle) fails the signature, and the
// export is never promoted. In Contents/Resources the same content is sealed as resources and the
// signature verifies (Codesign runs codesign --verify --deep --strict after signing).
TEST(MacBundleAssembler, ContentRootContentPassesCodesign)
{
    namespace fs = std::filesystem;
    static constexpr const char* kInfoPlist =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<plist version=\"1.0\"><dict>"
        "<key>CFBundleExecutable</key><string>Player</string>"
        "<key>CFBundleIdentifier</key><string>com.gameengine.codesignfixture</string>"
        "<key>CFBundlePackageType</key><string>APPL</string>"
        "</dict></plist>\n";
    GameEngine::TestUtils::ScopedTempDir temp(GameEngine::TestUtils::MakeUniqueTempDirectory("ge_mac_bundle_sign"));
    const fs::path bundle = temp.Path() / "Game.app";
    const fs::path macOSDir = bundle / "Contents/MacOS";
    fs::create_directories(macOSDir);
    std::ofstream(bundle / "Contents/Info.plist") << kInfoPlist;
    // Any Mach-O executable stands in for the Player; Codesign re-signs it ad hoc.
    fs::copy_file("/usr/bin/true", macOSDir / "Player");

    const fs::path contentRoot = GameEngine::PathUtils::InstallContentRootFor(macOSDir);
    for (const char* path : {"game.config", "Assets/.assetmanifest", "Assets/.lod/model.gelod",
                             "Assets/Models/model.glb", "Packages/packages.index"})
        WriteFixture(contentRoot / path);

    std::vector<std::string> errors;
    EXPECT_TRUE(GameEngine::MacBundleAssembler::Codesign(bundle, std::string(), errors));
    for (const std::string& error : errors)
        ADD_FAILURE() << error;
}

// #2685: a CoreCLR export ships every engine assembly the packaged Player loads, from the editor's
// engine managed directory, in the directory the Player resolves them from. The template's own
// managed set lacks GameEngine.Scripting.Runtime, the assembly ManagedSystemBridge resolves beside
// CoreBridge to tick C# GameSystems.
TEST(MacBundleAssembler, CoreClrExportStagesEveryManagedAssemblyThePlayerLoads)
{
    namespace fs = std::filesystem;
    GameEngine::TestUtils::ScopedTempDir temp(GameEngine::TestUtils::MakeUniqueTempDirectory("ge_mac_bundle_clr"));
    const MacManagedExportFixture fixture(temp.Path());

    std::vector<std::string> errors;
    EXPECT_TRUE(fixture.Export(errors));
    for (const std::string& error : errors)
        ADD_FAILURE() << error;

    const fs::path playerManaged =
        GameEngine::ScriptingPaths::ResolveEngineManagedDirectoryFrom(fixture.Bundle / "Contents/MacOS");
    EXPECT_EQ(playerManaged, fixture.Bundle / "Contents/Resources/Managed");
    for (const char* name : {"GameEngine.Scripts.dll", "GameEngine.CoreBridge.dll", "GameEngine.HotReload.dll",
                             "GameEngine.HotReload.runtimeconfig.json", "GameEngine.Scripting.Runtime.dll",
                             "GameEngine.Scripting.ABI.dll", "GameEngine.ECS.ABI.dll", "GameEngine.Input.ABI.dll"})
        EXPECT_TRUE(fs::exists(playerManaged / name)) << name << " is not where the packaged Player loads it";
    for (const char* name : {"GameEngine.Editor.Scripting.ABI.dll", "GameEngine.Editor.Managed.dll"})
        EXPECT_FALSE(fs::exists(playerManaged / name)) << "editor-only assembly shipped: " << name;
    EXPECT_EQ(ReadFixture(playerManaged / "GameEngine.CoreBridge.dll"), "editor-copy")
        << "the engine assemblies must be the set the scripts compiled against, not the template's";
}

// An engine assembly the Player loads that the editor does not have fails the export by name,
// instead of a package whose C# GameSystems never tick.
TEST(MacBundleAssembler, CoreClrExportWithoutScriptingRuntimeFailsNamingIt)
{
    GameEngine::TestUtils::ScopedTempDir temp(GameEngine::TestUtils::MakeUniqueTempDirectory("ge_mac_bundle_clr_missing"));
    const MacManagedExportFixture fixture(temp.Path());
    std::filesystem::remove(fixture.EditorManaged / "GameEngine.Scripting.Runtime.dll");

    std::vector<std::string> errors;
    EXPECT_FALSE(fixture.Export(errors));
    ASSERT_FALSE(errors.empty());
    // The error names the directory the export stages from, the editor's engine managed directory, and
    // only the assembly it lacks: the rest of the set is there.
    const std::string& error = errors.back();
    EXPECT_NE(error.find("'" + fixture.EditorManaged.string() + "'"), std::string::npos) << error;
    EXPECT_NE(error.find("GameEngine.Scripting.Runtime.dll"), std::string::npos) << error;
    EXPECT_EQ(error.find("GameEngine.CoreBridge.dll"), std::string::npos) << error;
}

// A game without C# ships no managed assemblies: the template's set does not ride along.
TEST(MacBundleAssembler, ExportWithoutManagedScriptsShipsNoManagedAssemblies)
{
    namespace fs = std::filesystem;
    GameEngine::TestUtils::ScopedTempDir temp(GameEngine::TestUtils::MakeUniqueTempDirectory("ge_mac_bundle_noclr"));
    const MacManagedExportFixture fixture(temp.Path());
    fs::remove_all(fixture.Scratch / "Managed");

    std::vector<std::string> errors;
    EXPECT_TRUE(fixture.Export(errors));
    for (const std::string& error : errors)
        ADD_FAILURE() << error;
    EXPECT_FALSE(fs::exists(fixture.Bundle / "Contents/Resources/Managed"));
    EXPECT_TRUE(fs::exists(fixture.Bundle / "Contents/Frameworks/libEngine.dylib"));
}
