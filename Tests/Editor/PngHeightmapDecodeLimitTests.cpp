// A 16-bit PNG heightmap decodes whole, so one past the decoder's limit is refused at the terrain's
// decode with the fix, which the Terrain inspector's Base section shows as "The heightmap does not
// decode: ...". Over a live registry and the terrain service, as the inspector resolves it.

#include <gtest/gtest.h>

#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "TerrainECS/TerrainService.h"

#include "../TestTempDir.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

using namespace GameEngine;

namespace
{

class PngHeightmapDecodeLimitTests : public ::testing::Test
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
        if (!TerrainECS::TerrainService::IsInitialized())
            TerrainECS::TerrainService::Initialize();

        s_Root = TestUtils::MakeUniqueTempDirectory("png_heightmap_decode_limit");
        std::filesystem::create_directories(s_Root);
        AssetSourceDesc source{};
        source.Alias = "pngheightmaplimit";
        source.Root = s_Root;
        source.AuthoritativeDbFile = s_Root / "AssetDatabase.assetdb";
        ASSERT_TRUE(engine.GetAssetManager().RegisterSource(source));
    }

    static void TearDownTestSuite()
    {
        std::error_code ec;
        std::filesystem::remove_all(s_Root, ec);
    }

    static std::filesystem::path s_Root;
};

std::filesystem::path PngHeightmapDecodeLimitTests::s_Root;

std::array<char, 4> BigEndian(uint32 value)
{
    return {static_cast<char>(value >> 24), static_cast<char>(value >> 16), static_cast<char>(value >> 8),
            static_cast<char>(value)};
}

TEST_F(PngHeightmapDecodeLimitTests, APngPastTheDecodersLimitIsRefusedWithTheExportThatWorks)
{
    // The signature and header of a 40000 x 40000 16-bit grayscale PNG: the size is all the limit
    // reads, and the decoder refuses it before it would allocate.
    const std::filesystem::path path = s_Root / "lidar.png";
    {
        std::ofstream out(path, std::ios::binary);
        out.write("\x89PNG\r\n\x1a\n", 8);
        out.write(BigEndian(13).data(), 4);
        out.write("IHDR", 4);
        out.write(BigEndian(40000).data(), 4);
        out.write(BigEndian(40000).data(), 4);
        const char rest[5] = {16, 0, 0, 0, 0};
        out.write(rest, 5);
        out.write(BigEndian(0).data(), 4);
    }
    const GUID guid = EngineCore::GetInstance().GetAssetManager().ResolveAssetGuid(path);
    ASSERT_FALSE(guid.IsNull());

    auto& terrain = TerrainECS::TerrainService::Get();
    EXPECT_EQ(terrain.ResolveHeightmapAsset(guid), nullptr);
    const std::string reason = terrain.GetHeightmapDecodeError(guid);
    EXPECT_NE(reason.find("40000 x 40000"), std::string::npos) << reason;
    EXPECT_NE(reason.find("export the heightmap as .r32"), std::string::npos) << reason;
}

TEST_F(PngHeightmapDecodeLimitTests, AHeightmapInAnotherImageFormatStillDecodes)
{
    // A 4 x 4 8-bit grayscale TGA: the image decoder reads it, and the PNG limit is not its concern.
    const std::filesystem::path path = s_Root / "hills.tga";
    {
        std::ofstream out(path, std::ios::binary);
        const unsigned char header[18] = {0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 4, 0, 8, 0};
        out.write(reinterpret_cast<const char*>(header), sizeof(header));
        for (int i = 0; i < 16; ++i)
            out.put(static_cast<char>(i * 16));
    }
    const GUID guid = EngineCore::GetInstance().GetAssetManager().ResolveAssetGuid(path);
    ASSERT_FALSE(guid.IsNull());

    auto& terrain = TerrainECS::TerrainService::Get();
    const auto decoded = terrain.ResolveHeightmapAsset(guid);
    ASSERT_NE(decoded, nullptr) << terrain.GetHeightmapDecodeError(guid);
    EXPECT_EQ(decoded->GetWidth(), 4u);
    EXPECT_EQ(terrain.GetHeightmapDecodeError(guid), "");
}

} // namespace
