#include <gtest/gtest.h>

#include "AssetCore/AssetTypes.h"

#include <algorithm>

using namespace GameEngine;

TEST(AssetTypes, DefaultExtensionSetsContainExpectedEntries)
{
    // This test exists to prevent drift between the core extension→type map and
    // call sites that use GetDefaultExtensionsForAssetType() for registrations/UI.

    const auto modelExts = GetDefaultExtensionsForAssetType(AssetType::Model);
    EXPECT_NE(std::find(modelExts.begin(), modelExts.end(), ".gltf"), modelExts.end());
    EXPECT_NE(std::find(modelExts.begin(), modelExts.end(), ".glb"), modelExts.end());
    EXPECT_NE(std::find(modelExts.begin(), modelExts.end(), ".fbx"), modelExts.end());

    const auto audioExts = GetDefaultExtensionsForAssetType(AssetType::Audio);
    EXPECT_NE(std::find(audioExts.begin(), audioExts.end(), ".aac"), audioExts.end());

    const auto shaderExts = GetDefaultExtensionsForAssetType(AssetType::Shader);
    EXPECT_NE(std::find(shaderExts.begin(), shaderExts.end(), ".comp"), shaderExts.end());
    EXPECT_NE(std::find(shaderExts.begin(), shaderExts.end(), ".geom"), shaderExts.end());

    const auto animationLibraryExts = GetDefaultExtensionsForAssetType(AssetType::AnimationLibrary);
    EXPECT_NE(std::find(animationLibraryExts.begin(), animationLibraryExts.end(), ".animlib"), animationLibraryExts.end());

    const auto animationControllerExts = GetDefaultExtensionsForAssetType(AssetType::AnimationController);
    EXPECT_NE(std::find(animationControllerExts.begin(), animationControllerExts.end(), ".animcontroller"), animationControllerExts.end());

    const auto animationGraphExts = GetDefaultExtensionsForAssetType(AssetType::AnimationGraph);
    EXPECT_NE(std::find(animationGraphExts.begin(), animationGraphExts.end(), ".animgraph"), animationGraphExts.end());
    EXPECT_TRUE(IsTextBasedAssetType(AssetType::AnimationGraph));

    const auto spriteFrameExts = GetDefaultExtensionsForAssetType(AssetType::SpriteFrames);
    EXPECT_NE(std::find(spriteFrameExts.begin(), spriteFrameExts.end(), ".spriteframes"), spriteFrameExts.end());

    const auto terrainMaterialLibraryExts =
        GetDefaultExtensionsForAssetType(AssetType::TerrainMaterialLibrary);
    EXPECT_NE(std::find(terrainMaterialLibraryExts.begin(), terrainMaterialLibraryExts.end(),
                        ".terrainmatlib"),
              terrainMaterialLibraryExts.end());
    // JSON, so the editor's text-asset paths (external edit, diffing, VCS) treat it as text.
    EXPECT_TRUE(IsTextBasedAssetType(AssetType::TerrainMaterialLibrary));

    // `.xml` is intentionally classified as generic XML by default (UILayout requires sniffing).
    const auto uiLayoutExts = GetDefaultExtensionsForAssetType(AssetType::UILayout);
    EXPECT_EQ(std::find(uiLayoutExts.begin(), uiLayoutExts.end(), ".xml"), uiLayoutExts.end());
    EXPECT_NE(std::find(uiLayoutExts.begin(), uiLayoutExts.end(), ".uxml"), uiLayoutExts.end());
}

