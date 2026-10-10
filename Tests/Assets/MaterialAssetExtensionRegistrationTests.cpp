#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/CoreAssetRegistrations.h"
#include "Assets/MaterialAsset.h"
#include "TestTempDir.h"

using namespace GameEngine;

namespace
{
static void WriteTextFile(const std::filesystem::path& p, const std::string& text)
{
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    ASSERT_TRUE(out.is_open()) << p.string();
    out << text;
}
} // namespace

TEST(MaterialAsset, CoreAssetTypes_RegisterDotMaterialExtension)
{
    AssetManager am;
    // AssetManager init also initializes its AssetRegistry + type registry, but factories are registered separately.
    const auto tmpRoot = TestUtils::MakeUniqueTempDirectory("ge_material_asset_ext_reg");
    const auto assetsRoot = tmpRoot / "Assets";
    std::error_code ec;
    std::filesystem::remove_all(tmpRoot, ec);
    std::filesystem::create_directories(assetsRoot, ec);
    ASSERT_TRUE(am.Initialize(assetsRoot, nullptr));

    RegisterCoreAssetTypes(am.GetAssetTypeRegistry());

    // Material should be registered and accept .material as an extension.
    EXPECT_TRUE(am.GetAssetTypeRegistry().IsAssetTypeRegistered(AssetType::Material));

    const auto exts = GetDefaultExtensionsForAssetType(AssetType::Material);
    const bool hasDotMaterial = std::find(exts.begin(), exts.end(), std::string(".material")) != exts.end();
    EXPECT_TRUE(hasDotMaterial);
}

TEST(MaterialAsset, AssetManager_CreateAsset_InfersMaterialTypeWhenMetadataUnknown)
{
    AssetManager am;
    const auto tmpRoot = TestUtils::MakeUniqueTempDirectory("ge_material_asset_create_infer");
    const auto assetsRoot = tmpRoot / "Assets";
    std::error_code ec;
    std::filesystem::remove_all(tmpRoot, ec);
    std::filesystem::create_directories(assetsRoot, ec);
    ASSERT_TRUE(am.Initialize(assetsRoot, nullptr));

    RegisterCoreAssetTypes(am.GetAssetTypeRegistry());

    // Create a minimal .material file. This test is about *type recognition and instantiation*,
    // not shader compilation (which happens during Load()).
    const auto matPath = assetsRoot / "Materials" / "Test.material";
    WriteTextFile(matPath,
                  "{\n"
                  "  \"schemaVersion\": 2,\n"
                  "  \"materialName\": \"Test\",\n"
                  "  \"lightingModel\": \"unlit\",\n"
                  "  \"surfaceShader\": \"Materials/Surfaces/unlit_solid.glsl\",\n"
                  "  \"properties\": {\"baseColor\": [1.0, 1.0, 1.0, 1.0]},\n"
                  "  \"textures\": {}\n"
                  "}\n");

    auto& reg = am.GetRegistry();
    ASSERT_TRUE(reg.RegisterAsset(matPath));

    AssetMetadata md{};
    ASSERT_TRUE(reg.TryGetAssetMetadata(matPath, md));
    ASSERT_EQ(md.Extension, ".material");
    ASSERT_EQ(md.Type, AssetType::Material) << "registry should classify .material as Material";

    // Force Unknown and ensure AssetManager::CreateAsset still instantiates via extension inference.
    md.Type = AssetType::Unknown;
    SharedPtr<Asset> a = am.CreateAsset(md);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->GetType(), AssetType::Material);
    EXPECT_NE(dynamic_cast<MaterialAsset*>(a.get()), nullptr);
}

