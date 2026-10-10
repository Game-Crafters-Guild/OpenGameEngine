// An EmbeddedImage has content when it holds its bytes or names the asset that holds them.
// The readers that wait for a model's textures (the Asset View thumbnail, the Polyhaven
// placeholders) wait on this: a glTF image the AssetDatabase already tracks is referenced by
// GUID and never fills Data, so a wait on Data alone never ends.

#include <gtest/gtest.h>

#include "AssetCore/GUID.h"
#include "Assets/ModelAsset.h"

using namespace GameEngine;

TEST(EmbeddedImageContent, AnImageWithNeitherBytesNorAssetHasNoContent)
{
    const EmbeddedImage image{};
    EXPECT_FALSE(image.HasContent());
}

TEST(EmbeddedImageContent, AnImageWithBytesHasContent)
{
    EmbeddedImage image{};
    image.Data = {0x89, 'P', 'N', 'G'};
    EXPECT_TRUE(image.HasContent());
}

TEST(EmbeddedImageContent, AnAssetBackedImageHasContentWithoutBytes)
{
    EmbeddedImage image{};
    image.AssetGuid = GUID::Generate();
    ASSERT_TRUE(image.Data.empty());
    EXPECT_TRUE(image.HasContent());
}
