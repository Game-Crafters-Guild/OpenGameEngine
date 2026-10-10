#include <gtest/gtest.h>

#include "Assets/AssetRegistry.h"
#include "TestTempDir.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace GameEngine;

namespace
{
static void WriteBinaryFile(const std::filesystem::path& p, const std::string& bytes)
{
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);

    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open()) << p.string();
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

static std::string ReadAllText(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    EXPECT_TRUE(in.is_open()) << p.string();
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}
} // namespace

TEST(AssetDatabase, NormalizesLegacyAssetsPrefixedPathsWhenAssetRootIsAssetsDir)
{
    namespace fs = std::filesystem;

    const fs::path tmpRoot = TestUtils::MakeUniqueTempDirectory("ge_assetdb_legacy_assets_prefix");
    std::error_code ec;
    fs::remove_all(tmpRoot, ec);
    fs::create_directories(tmpRoot, ec);

    const fs::path assetsRoot = tmpRoot / "Assets";
    const fs::path iconPath = assetsRoot / "Icons" / "Folder@64px.png";
    WriteBinaryFile(iconPath, "DUMMY");

    const GUID guid("a3d8e541-8c66-4579-8d71-f1ae63c074eb");
    const fs::path dbPath = tmpRoot / "AssetDatabase.assetdb";
    {
        std::ofstream out(dbPath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open()) << dbPath.string();
        out << "{\"guid\":\"" << guid.ToString() << "\","
            << "\"path\":\"Assets/Icons/Folder@64px.png\","
            << "\"type\":\"Texture\","
            << "\"missing\":false,"
            << "\"kv\":{}}\n";
    }

    AssetRegistry reg;
    ASSERT_TRUE(reg.Initialize(assetsRoot, nullptr));

    // The on-disk asset exists at <assetRoot>/Icons/...; the registry should not
    // try to resolve "<assetRoot>/Assets/Icons/..." (duplicated Assets segment).
    EXPECT_EQ(reg.GetAssetGUID(iconPath), guid);

    AssetMetadata md{};
    EXPECT_TRUE(reg.TryGetAssetMetadata(guid, md));
    // The registry lowercases paths on Windows for case-insensitive lookup,
    // so md.Path may differ in case from the input. Compare filesystem
    // identity rather than exact string equality.
    std::error_code ec2;
    EXPECT_TRUE(fs::equivalent(md.Path, fs::absolute(iconPath), ec2))
        << "md.Path=" << md.Path.string()
        << " iconPath=" << fs::absolute(iconPath).string();

    // Saving should persist the normalized canonical path (no leading "Assets/").
    EXPECT_TRUE(reg.SaveToFile({}));
    const std::string txt = ReadAllText(dbPath);
    EXPECT_NE(txt.find("\"path\":\"Icons/Folder@64px.png\""), std::string::npos);
    EXPECT_EQ(txt.find("\"path\":\"Assets/Icons/Folder@64px.png\""), std::string::npos);

    reg.Shutdown();
    fs::remove_all(tmpRoot, ec);
}


