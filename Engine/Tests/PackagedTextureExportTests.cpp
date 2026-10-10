#include "Assets/AssetManager.h"
#include "Assets/RuntimeHumanoidProfile.h"
#include "Assets/TextureAsset.h"
#include "Assets/TextureCook.h"
#include "Core/Engine.h"
#include "Engine/Build/BuildPipeline.h"
#include "Terrain/Heightfield.h"
#include "TestTempDir.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string_view>

#include <gtest/gtest.h>

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

std::string ImageBytes()
{
    std::string bytes(18, '\0');
    bytes[2] = 2; bytes[12] = 8; bytes[14] = 8; bytes[16] = 32; bytes[17] = 0x28;
    for (int i = 0; i < 64; ++i)
        bytes.append("\xB4\x78\x50\xFF", 4);
    return bytes;
}

class PackagedTextureExportTest : public testing::Test
{
protected:
    void SetUp() override
    {
        WriteFile(Assets / "Color.tga", ImageBytes());
        WriteFile(Assets / "Unclassified.tga", ImageBytes());
        // A terrain heightmap: no material binds it, and TerrainService opens its source
        // by path for samples deeper than the RGBA8 payload.
        WriteFile(Assets / "Height.tga", ImageBytes());
        WriteFile(Assets / "Icon.svg", R"(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="16"><rect width="32" height="16" fill="red"/></svg>)");
        // Opaque mandatory dependencies; this fixture does not render text or retarget a rig.
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
        ASSERT_TRUE(Engine.GetAssetManager().GetRegistry().SetMetaValue(
            Assets / "Color.tga", kTextureUsageMetaKey, "color"));
    }
    void TearDown() override
    {
        TextureAsset::SetGpuTranscodeTarget(PreviousTarget);
        Engine.Shutdown();
        fs::current_path(PreviousDirectory);
    }
    TestUtils::ScopedTempDir Temporary{TestUtils::MakeUniqueTempDirectory("packaged_texture_export")};
    const fs::path PreviousDirectory = fs::current_path();
    const fs::path Project = Temporary.Path() / "Project";
    const fs::path Assets = Project / "Assets";
    const fs::path Runtime = Temporary.Path() / "Runtime";
    const fs::path Output = Temporary.Path() / "Export";
    const TextureGpuTranscodeTarget PreviousTarget = TextureAsset::GetGpuTranscodeTarget();
    EngineCore Engine;
};
} // namespace

TEST_F(PackagedTextureExportTest, ShipsCookedTexturesAndKeepsOnlySourcesAPathReaderOpens)
{
    const GUID color = Engine.GetAssetManager().GetRegistry().GetAssetGUID(Assets / "Color.tga");
    const GUID unclassified = Engine.GetAssetManager().GetRegistry().GetAssetGUID(Assets / "Unclassified.tga");
    ASSERT_FALSE(color.IsNull());
    ASSERT_FALSE(unclassified.IsNull());
    BuildSettings build;
    build.platformName = "Linux";
    build.projectRoot = Project;
    build.prebuiltPlayerDirectory = Runtime;
    build.outputDirectory = Output;
    build.compileScripts = false;
    build.buildConfiguration = "Release";
    build.playerConfig.gameName = "TextureExportProbe";
    BuildPipeline pipeline(nullptr);
    BuildProgress progress;
    ASSERT_TRUE(pipeline.Execute(build, [&progress](const BuildProgress& current) { progress = current; }))
        << progress.statusMessage << '\n' << testing::PrintToString(progress.errors);
    EXPECT_FALSE(fs::exists(Output / "Assets/Color.tga"));
    // Unclassified textures keep their sources beside their payloads.
    EXPECT_TRUE(fs::exists(Output / "Assets/Unclassified.tga"));
    EXPECT_TRUE(fs::exists(Output / "Assets/Height.tga"));
    EXPECT_TRUE(fs::exists(Output / "Assets/Icon.svg"));
    EXPECT_TRUE(fs::exists(Output / "Assets/Fonts/Roboto-Regular.ttf"));
    size_t cookedFiles = 0;
    for (const auto& entry : fs::recursive_directory_iterator(Output))
        if (entry.is_regular_file() && entry.path().extension() == ".ktx2")
            ++cookedFiles;
    EXPECT_EQ(cookedFiles, IsTextureCookEncoderAvailable() ? 4u : 3u);

    // Exercise the same logical paths and GUIDs through both Player load paths,
    // on a BC-capable device and the portable uncompressed fallback.
    for (const auto target : {TextureGpuTranscodeTarget::BC7, TextureGpuTranscodeTarget::RGBA32})
    {
        TextureAsset::SetGpuTranscodeTarget(target);
        for (const bool asynchronous : {false, true})
        {
            SCOPED_TRACE(asynchronous ? "reader thread" : "synchronous");
            AssetManager player;
            ASSERT_TRUE(player.Initialize(Output / "Assets", asynchronous ? &Engine.GetJobSystem() : nullptr,
                Temporary.Path() / "UserData/AssetDatabase.assetdb", Temporary.Path() / "UserData/.Cache/AssetDatabase"));
            EXPECT_EQ(player.ResolveAssetGuid("Color.tga"), color);
            EXPECT_EQ(player.ResolveAssetGuid("Unclassified.tga"), unclassified);
            auto future = player.LoadAssetAsync(color);
            ASSERT_EQ(future.wait_for(std::chrono::seconds(10)), std::future_status::ready);
            const auto texture = std::dynamic_pointer_cast<TextureAsset>(future.get());
            ASSERT_NE(texture, nullptr);
            EXPECT_EQ(texture->GetWidth(), 8u);
            EXPECT_EQ(texture->GetMipmapLevels(), 4u);
            EXPECT_EQ(texture->GetFormat(), target == TextureGpuTranscodeTarget::BC7 && IsTextureCookEncoderAvailable()
                ? TextureFormat::BC7 : TextureFormat::RGBA8);
            auto portableFuture = player.LoadAssetAsync(unclassified);
            ASSERT_EQ(portableFuture.wait_for(std::chrono::seconds(10)), std::future_status::ready);
            const auto portable = std::dynamic_pointer_cast<TextureAsset>(portableFuture.get());
            ASSERT_NE(portable, nullptr);
            EXPECT_EQ(portable->GetFormat(), TextureFormat::RGBA8);
            player.Shutdown();
        }
    }

    // Direct CPU and GPU-upload readers use the owning engine outside an async load.
    Engine.Shutdown();
    ApplicationConfig runtime;
    runtime.WorkspaceDirectory = Output.string();
    runtime.AssetDirectory = (Output / "Assets").string();
    runtime.AssetDatabaseFile = (Temporary.Path() / "RuntimeUserData/AssetDatabase.assetdb").string();
    runtime.AssetDatabaseCacheDirectory = (Temporary.Path() / "RuntimeUserData/.Cache/AssetDatabase").string();
    ASSERT_TRUE(Engine.Initialize(runtime));
    // The heightmap reads the way TerrainService::ResolveHeightmapAsset does: the registered
    // path, decoded with 16-bit precision.
    AssetMetadata heightmap{};
    ASSERT_TRUE(Engine.GetAssetManager().GetRegistry().TryGetAssetMetadata(
        Engine.GetAssetManager().GetRegistry().GetAssetGUID(Output / "Assets/Height.tga"), heightmap));
    Terrain::HeightfieldData heightfield;
    EXPECT_TRUE(heightfield.LoadFromPNG16(heightmap.Path)) << heightmap.Path;
    TextureAsset::SetGpuTranscodeTarget(TextureGpuTranscodeTarget::BC7);
    TextureAsset cpu(color, Output / "Assets/Color.tga");
    ASSERT_TRUE(cpu.Load());
    EXPECT_EQ(cpu.GetFormat(), TextureFormat::RGBA8);
    EXPECT_EQ(cpu.GetMipmapLevels(), 4u);
    ASSERT_NE(cpu.GetPixelData(), nullptr);
    EXPECT_EQ(cpu.GetPixelData()[0], 80);
    TextureAsset upload(color, Output / "Assets/Color.tga");
    upload.SetAdoptCookedArtifacts(true);
    ASSERT_TRUE(upload.Load());
    EXPECT_EQ(upload.GetFormat(), IsTextureCookEncoderAvailable() ? TextureFormat::BC7 : TextureFormat::RGBA8);
}

TEST_F(PackagedTextureExportTest, TemplateSymbolsFollowTheRequestedBuildConfiguration)
{
    WriteFile(Runtime / "Player.pdb", "symbols");
    WriteFile(Runtime / "lib/Engine.debug", "symbols");
    WriteFile(Runtime / "lib/Engine.dwp", "symbols");
    WriteFile(Runtime / ".debug/build-id", "symbols");
    WriteFile(Runtime / "lib/libEngine.so", "runtime");
    WriteFile(Runtime / "Assets/stale.tga", "old game content");
    for (const char* configuration : {"Release", "MinSizeRel", "Debug", "RelWithDebInfo"})
    {
        SCOPED_TRACE(configuration);
        BuildSettings build;
        build.platformName = "Linux";
        build.projectRoot = Project;
        build.prebuiltPlayerDirectory = Runtime;
        build.outputDirectory = Output;
        build.compileScripts = false;
        build.buildConfiguration = configuration;
        build.playerConfig.gameName = "TextureExportProbe";
        BuildPipeline pipeline(nullptr);
        BuildProgress progress;
        ASSERT_TRUE(pipeline.Execute(build, [&progress](const BuildProgress& current) { progress = current; }))
            << progress.statusMessage << '\n' << testing::PrintToString(progress.errors);
        const bool symbols = std::string_view(configuration) == "Debug" || std::string_view(configuration) == "RelWithDebInfo";
        EXPECT_EQ(fs::exists(Output / "Player.pdb"), symbols);
        EXPECT_EQ(fs::exists(Output / "lib/Engine.debug"), symbols);
        EXPECT_EQ(fs::exists(Output / "lib/Engine.dwp"), symbols);
        EXPECT_EQ(fs::exists(Output / ".debug/build-id"), symbols);
        EXPECT_TRUE(fs::exists(Output / "lib/libEngine.so"));
        EXPECT_FALSE(fs::exists(Output / "Assets/stale.tga"));
    }
}
