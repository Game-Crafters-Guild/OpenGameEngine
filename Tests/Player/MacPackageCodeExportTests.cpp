// A macOS export of a project whose package carries native code, through BuildPipeline::Execute:
// the package's runtime module and the project's own native scripts ship in the bundle's
// Contents/Frameworks, signed with it, where the packaged Player's loads reach them; the package's
// Editor module never ships.

#include "Assets/Packages/PackageCodeModules.h"
#include "Assets/Packages/PackageResolver.h"
#include "Assets/RuntimeHumanoidProfile.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "Engine/Build/BuildPipeline.h"
#include "NativeScripting/BuildCacheRecord.h"
#include "NativeScripting/NativeBuildConfig.h"
#include "NativeScripting/NativeScriptManager.h"
#include "NativeScripting/SdkManifest.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace GameEngine;
namespace fs = std::filesystem;
namespace ns = GameEngine::NativeScripting;

namespace
{

constexpr std::string_view kPackageName = "probe-pack";
constexpr std::string_view kRuntimeModule = "ProbePack";
// Where the editor linked its modules against the SDK on the build machine; the export must not
// ship it as an rpath.
constexpr std::string_view kBuildMachineRpath = "/BuildMachine/Editor.app/Contents/MacOS/SDK/lib/Release";

void WriteFile(const fs::path& path, std::string_view bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(output.good()) << path;
}

std::string RunAndCapture(const std::string& command)
{
    std::string output;
    FILE* pipe = popen((command + " 2>&1").c_str(), "r");
    if (!pipe)
        return output;
    std::array<char, 512> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
        output += buffer.data();
    pclose(pipe);
    return output;
}

std::string Quoted(const fs::path& path)
{
    return "'" + path.string() + "'";
}

bool IsMachO(const fs::path& path)
{
    std::uint32_t magic = 0;
    std::ifstream(path, std::ios::binary).read(reinterpret_cast<char*>(&magic), sizeof(magic));
    return magic == 0xfeedfacfu || magic == 0xcffaedfeu || magic == 0xcafebabeu || magic == 0xbebafecau;
}

// What the editor leaves after building a native module: the module under a content-addressed
// shadow-copy name and its build record naming it, stamped with the engine it was built against.
// The image is a real dylib carrying the build machine's SDK rpath, like the editor's builds.
void WriteEditorBuild(const fs::path& buildDir, const fs::path& dylib, const std::string& abiDigest,
                      const fs::path& image = GE_MAC_BUNDLE_FIXTURE_DYLIB)
{
    fs::create_directories(dylib.parent_path());
    fs::copy_file(image, dylib, fs::copy_options::overwrite_existing);
    fs::permissions(dylib, fs::perms::owner_write, fs::perm_options::add);
    RunAndCapture("install_name_tool -add_rpath '" + std::string(kBuildMachineRpath) + "' " + Quoted(dylib));
    ASSERT_TRUE(ns::WriteBuildCacheRecord(buildDir, ns::BuildCacheRecord{"digest", dylib.string(), abiDigest}));
}

// The shipping engine's identity as the export computes it from the staged SDK, for a module
// compiled with `defines`.
std::string EngineAbiDigest(const fs::path& sdk, const std::vector<std::string>& defines)
{
    ns::NativeBuildConfig config;
    std::string error;
    EXPECT_TRUE(ns::LoadSdkManifest(sdk, config, error)) << error;
    ns::AppendPackageDefines(config, defines);
    return ns::ComputeRuntimeEngineAbiDigest(std::move(config));
}

// Counts the Player's loads that reach a module image: ImageMapBegin fires right before the
// load, after the manager found the record and the image it names. The fixture dylib exports no
// user-module entry points, so each load itself fails.
int ModuleMapBegins(const fs::path& moduleRoot, const std::string& moduleName)
{
    int mapBegins = 0;
    ns::NativeScriptManager manager;
    if (!manager.Initialize(nullptr))
        return -1;
    ns::NativeScriptManager::ModuleImageObserver observer;
    observer.ImageMapBegin = [&mapBegins] { ++mapBegins; };
    observer.ImageMapEnd = [](std::uint64_t, std::uint64_t) {};
    manager.SetModuleImageObserver(std::move(observer));
    EXPECT_FALSE(manager.LoadPrebuiltUserModule(moduleRoot, moduleName));
    manager.Shutdown();
    return mapBegins;
}

class MacPackageCodeExportTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        // The SDK the editor ships: the Player.app template and the native scripting manifest.
        const fs::path templateApp = Sdk / "templates/Player.app/Contents";
        WriteFile(templateApp / "Info.plist",
                  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict>"
                  "<key>CFBundleExecutable</key><string>Player</string>"
                  "<key>CFBundleIdentifier</key><string>com.gameengine.player</string>"
                  "<key>CFBundlePackageType</key><string>APPL</string></dict></plist>\n");
        fs::create_directories(templateApp / "MacOS");
        fs::copy_file("/usr/bin/true", templateApp / "MacOS/Player");
        WriteFile(Sdk / "nativescripting/manifest.txt", "includedir=include\nimportlib=lib/libEngine.dylib\n");
        WriteFile(Sdk / "include/Engine.h", "#pragma once\n");
        WriteFile(Sdk / "lib/libEngine.dylib", "engine");

        // Opaque mandatory dependencies of every export.
        WriteFile(Assets / "Fonts/Roboto-Regular.ttf", "TTFDATA");
        WriteFile(Assets / kRuntimeHumanoidProfilePath, "{}");

        // A code-only package with a runtime module and an Editor module.
        WriteFile(Project / "Packages/manifest.json",
                  "{\"dependencies\": {\"" + std::string(kPackageName) + "\": \"embedded\"}}");
        WriteFile(Package / "package.json",
                  "{\"name\": \"" + std::string(kPackageName) + "\", \"version\": \"1.0.0\", \"assets\": \"\"}");
        WriteFile(Package / "Native/ProbeModule.cpp", "int ProbeModule() { return 0; }\n");
        WriteFile(Package / "Editor/ProbeEditorModule.cpp", "int ProbeEditorModule() { return 0; }\n");

        // The editor's builds of both package modules and of the project's native scripts.
        const PackageResolution resolution = PackageResolver::Resolve(Project);
        bool runtimeModuleFound = false;
        for (const PackageCodeModule& module : CollectPackageCodeModules(resolution, /*editorContext=*/false))
        {
            ASSERT_EQ(module.AssemblyName, kRuntimeModule) << "only the runtime module ships";
            WriteEditorBuild(module.CacheDir / "NativeScripts/build",
                             module.CacheDir / "NativeScripts/active/libProbePack_1a2b3c4d.dylib",
                             EngineAbiDigest(Sdk, module.Defines));
            runtimeModuleFound = true;
        }
        ASSERT_TRUE(runtimeModuleFound);
        WriteEditorBuild(Package / ".Cache/NativeScripts/build-editor",
                         Package / ".Cache/NativeScripts/active-editor/libProbePack.Editor_5e6f7a8b.dylib",
                         EngineAbiDigest(Sdk, CollectAllPackageDefines(resolution)));
        WriteProjectScriptsBuild(GE_MAC_BUNDLE_FIXTURE_DYLIB);

        ApplicationConfig config;
        config.WorkspaceDirectory = Project.string();
        config.AssetDirectory = Assets.string();
        config.AssetDatabaseFile = (Project / "AssetDatabase.assetdb").string();
        config.AssetDatabaseCacheDirectory = (Project / ".Cache/AssetDatabase").string();
        ASSERT_TRUE(Engine.Initialize(config));
        Engine.GetAssetManager().WaitForStartupScan();
    }

    void TearDown() override
    {
        Engine.Shutdown();
        fs::current_path(PreviousDirectory);
    }

    // The editor's build of the project's native scripts, a copy of `image`.
    void WriteProjectScriptsBuild(const fs::path& image)
    {
        WriteEditorBuild(Project / ".Cache/NativeScripts/build",
                         Project / ".Cache/NativeScripts/active/libUserScripts_0c0d0e0f.dylib",
                         EngineAbiDigest(Sdk, CollectAllPackageDefines(PackageResolver::Resolve(Project))), image);
    }

    bool Export(BuildProgress& progress)
    {
        BuildSettings build;
        build.platformName = "Mac";
        build.projectRoot = Project;
        build.editorSDKPath = Sdk;
        build.outputDirectory = Output;
        build.compileScripts = false;
        build.buildConfiguration = "Release";
        build.playerConfig.gameName = "PackageCodeProbe";
        BuildPipeline pipeline(nullptr);
        return pipeline.Execute(build, [&progress](const BuildProgress& current) { progress = current; });
    }

    TestUtils::ScopedTempDir Temporary{TestUtils::MakeUniqueTempDirectory("mac_package_code_export")};
    const fs::path PreviousDirectory = fs::current_path();
    const fs::path Sdk = Temporary.Path() / "SDK";
    const fs::path Project = Temporary.Path() / "Project";
    const fs::path Assets = Project / "Assets";
    const fs::path Package = Project / "Packages" / kPackageName;
    const fs::path Output = Temporary.Path() / "Export";
    const fs::path Bundle = Output / "PackageCodeProbe.app";
    EngineCore Engine;
};

} // namespace

TEST_F(MacPackageCodeExportTest, PackageRuntimeModuleShipsInFrameworksWhereThePlayerLoadsIt)
{
    BuildProgress progress;
    ASSERT_TRUE(Export(progress)) << progress.statusMessage << '\n' << testing::PrintToString(progress.errors);
    for (const std::string& warning : progress.warnings)
        EXPECT_EQ(warning.find("Package code modules"), std::string::npos) << warning;

    const fs::path contentRoot = PathUtils::InstallContentRootFor(Bundle / "Contents/MacOS");
    const fs::path packageModule = Bundle / "Contents/Frameworks/Packages" / kPackageName / "libProbePack.dylib";
    ASSERT_TRUE(fs::exists(packageModule)) << "the package's runtime module is not in Contents/Frameworks";
    EXPECT_EQ(ModuleMapBegins(contentRoot / "Packages" / kPackageName, std::string(kRuntimeModule)), 1)
        << "the packaged Player's load of package '" << kPackageName << "' does not reach its module";
    EXPECT_EQ(ModuleMapBegins(contentRoot, "UserScripts"), 1)
        << "the packaged Player's load of the project's native scripts does not reach them";

    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(Bundle))
    {
        const fs::path relative = entry.path().lexically_relative(Bundle);
        EXPECT_EQ(relative.filename().string().find(".Editor"), std::string::npos)
            << "an Editor module shipped: " << relative;
        if (entry.is_regular_file() && relative.string().starts_with("Contents/Resources"))
            EXPECT_FALSE(IsMachO(entry.path())) << "code outside the bundle's code locations: " << relative;
    }
}

TEST_F(MacPackageCodeExportTest, ShippedModulesAreSignedWithTheBundleAndResolveOnlyInsideIt)
{
    BuildProgress progress;
    ASSERT_TRUE(Export(progress)) << progress.statusMessage << '\n' << testing::PrintToString(progress.errors);

    ASSERT_TRUE(fs::is_directory(Bundle / "Contents/Frameworks")) << "the export moved no module into Contents/Frameworks";
    std::vector<fs::path> modules;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(Bundle / "Contents/Frameworks"))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".dylib")
            modules.push_back(entry.path());
    }
    ASSERT_EQ(modules.size(), 2u) << "expected the package's runtime module and the project's native scripts";
    for (const fs::path& module : modules)
    {
        SCOPED_TRACE(module.lexically_relative(Bundle).string());
        // codesign signs code in Contents/Frameworks as part of the bundle, replacing the
        // linker's own ad-hoc signature; an image it skipped keeps the linker's.
        const std::string signature = RunAndCapture("codesign -dv " + Quoted(module));
        EXPECT_NE(signature.find("Signature="), std::string::npos) << signature;
        EXPECT_EQ(signature.find("linker-signed"), std::string::npos) << signature;
        const std::string loadCommands = RunAndCapture("otool -l " + Quoted(module));
        EXPECT_NE(loadCommands.find("path @executable_path/../Frameworks "), std::string::npos) << loadCommands;
        EXPECT_EQ(loadCommands.find(kBuildMachineRpath), std::string::npos) << loadCommands;
    }
}

TEST_F(MacPackageCodeExportTest, UniversalModuleShipsWithItsRpathsConfinedInEverySlice)
{
    WriteProjectScriptsBuild(GE_MAC_BUNDLE_UNIVERSAL_FIXTURE_DYLIB);
    BuildProgress progress;
    ASSERT_TRUE(Export(progress)) << progress.statusMessage << '\n' << testing::PrintToString(progress.errors);

    const fs::path module = Bundle / "Contents/Frameworks/libUserScripts.dylib";
    const std::string architectures = RunAndCapture("lipo -archs " + Quoted(module));
    ASSERT_NE(architectures.find("arm64"), std::string::npos) << architectures;
    ASSERT_NE(architectures.find("x86_64"), std::string::npos) << architectures;
    for (const char* architecture : {"arm64", "x86_64"})
    {
        SCOPED_TRACE(architecture);
        const std::string loadCommands =
            RunAndCapture("otool -arch " + std::string(architecture) + " -l " + Quoted(module));
        EXPECT_NE(loadCommands.find("path @executable_path/../Frameworks "), std::string::npos) << loadCommands;
        EXPECT_EQ(loadCommands.find(kBuildMachineRpath), std::string::npos) << loadCommands;
    }
}
