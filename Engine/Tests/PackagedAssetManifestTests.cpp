#include "Assets/AssetManager.h"
#include "Assets/AuthoredLodImport.h"
#include "Assets/ModelAssetSettings.h"
#include "Assets/RuntimeHumanoidProfile.h"
#include "Core/Engine.h"
#include "Engine/Build/BuildPipeline.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

using namespace GameEngine;
namespace fs = std::filesystem;
namespace
{
void WriteFile(const fs::path& path, std::string_view bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(output.good()) << path;
}
struct ExpectedSetting
{
    const char* Asset;
    const char* Key;
    std::string Value;
};
class PackagedAssetManifestTest : public testing::Test
{
protected:
    void SetUp() override
    {
        WriteFile(Assets / "Model.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        WriteFile(Assets / "Low.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        WriteFile(Assets / "Image.svg", R"(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="16"><rect width="32" height="16" fill="red"/></svg>)");
        const std::array<unsigned char, 46> wave = {
            'R','I','F','F',38,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,
            1,0,1,0,0x40,0x1f,0,0,0x80,0x3e,0,0,2,0,16,0,'d','a','t','a',2,0,0,0,0,0};
        WriteFile(Assets / "Sound.wav", {reinterpret_cast<const char*>(wave.data()), wave.size()});
        // Opaque mandatory dependencies: this export test does not render or play.
        WriteFile(Assets / "Fonts/Roboto-Regular.ttf", "TTFDATA");
        WriteFile(Assets / kRuntimeHumanoidProfilePath, "{}");
        // The public prebuilt-runtime route stages this header without launching it.
        std::array<char, 20> elf{};
        elf[0] = 0x7f; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
        elf[4] = 2; elf[5] = 1; elf[18] = 62;
        WriteFile(Runtime / "Player", {elf.data(), elf.size()});
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
    TestUtils::ScopedTempDir Temporary{TestUtils::MakeUniqueTempDirectory("packaged_asset_manifest")};
    const fs::path PreviousDirectory = fs::current_path();
    const fs::path Project = Temporary.Path() / "Project";
    const fs::path Assets = Project / "Assets";
    const fs::path Runtime = Temporary.Path() / "Runtime";
    const fs::path Output = Temporary.Path() / "Export";
    EngineCore Engine;
};
} // namespace

TEST_F(PackagedAssetManifestTest, RuntimeSettingsSurviveExportAndPlayerManifestMount)
{
    auto& registry = Engine.GetAssetManager().GetRegistry();
    const auto lowGuid = registry.GetAssetGUID(Assets / "Low.obj");
    ASSERT_FALSE(lowGuid.IsNull());
    const auto slot = EncodeLodSlotValue("Low.obj", lowGuid);
    const std::vector<ExpectedSetting> settings = {
        {"Sound.wav", "audio.loadPolicy", "Stream"},
        {"Sound.wav", "audio.allowVirtualization", "false"},
        {"Model.obj", "assets.lod.slot1", slot},
        {"Model.obj", "assets.lod.slot2", slot},
        {"Model.obj", "assets.lod.slot3", slot},
        {"Model.obj", "assets.model.rigKind", "none"},
        {"Image.svg", "assets.texture.svgRasterSize", "64"},
        {"Model.obj", "assets.lod.generate", "false"},
        {"Image.svg", "assets.texture.filter", "point"},
    };
    for (const auto& setting : settings)
        ASSERT_TRUE(registry.SetMetaValue(Assets / setting.Asset, setting.Key, setting.Value)) << setting.Key;
    BuildSettings build;
    build.platformName = "Linux";
    build.projectRoot = Project;
    build.prebuiltPlayerDirectory = Runtime;
    build.outputDirectory = Output;
    build.compileScripts = false;
    build.buildConfiguration = "Release";
    build.playerConfig.gameName = "ManifestProbe";
    BuildPipeline pipeline(nullptr);
    BuildProgress progress;
    ASSERT_TRUE(pipeline.Execute(build, [&progress](const BuildProgress& current) { progress = current; }))
        << progress.statusMessage << '\n' << testing::PrintToString(progress.errors);
    ASSERT_TRUE(fs::exists(Output / "Assets/.assetmanifest"));
    AssetManager playerAssets;
    ASSERT_TRUE(playerAssets.Initialize(Output / "Assets", nullptr,
                                         Temporary.Path() / "UserData/AssetDatabase.assetdb",
                                         Temporary.Path() / "UserData/.Cache/AssetDatabase"));
    const auto& playerRegistry = playerAssets.GetRegistry();
    EXPECT_FALSE(playerRegistry.AcceptsDerivedRecords(Output / "Assets/Model.obj"));
    for (const auto& setting : settings)
    {
        SCOPED_TRACE(setting.Key);
        std::string value;
        EXPECT_TRUE(playerRegistry.TryGetMetaValue(Output / "Assets" / setting.Asset, setting.Key, value));
        EXPECT_EQ(value, setting.Value);
    }
    EXPECT_EQ(ModelAssetSettings::Load(playerRegistry, Output / "Assets/Model.obj").RigKind,
              ModelRigKind::NotHumanoid);
    playerAssets.Shutdown();
}