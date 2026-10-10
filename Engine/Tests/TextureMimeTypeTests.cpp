// Texture MIME mapping shared by the glTF and FBX loaders (extension -> MIME)
// and editor texture extraction (MIME -> extension).

#include <gtest/gtest.h>

#include "Assets/Textures/TextureMimeType.h"

#include <filesystem>
#include <string_view>

using namespace GameEngine;

TEST(TextureMimeType, MapsEveryLoadableExtension)
{
    EXPECT_EQ(MimeFromTextureExtension("a.png"), "image/png");
    EXPECT_EQ(MimeFromTextureExtension("a.jpg"), "image/jpeg");
    EXPECT_EQ(MimeFromTextureExtension("a.jpeg"), "image/jpeg");
    EXPECT_EQ(MimeFromTextureExtension("a.tga"), "image/x-tga");
    EXPECT_EQ(MimeFromTextureExtension("a.bmp"), "image/bmp");
    EXPECT_EQ(MimeFromTextureExtension("a.hdr"), "image/vnd.radiance");
    EXPECT_EQ(MimeFromTextureExtension("a.tif"), "image/tiff");
    EXPECT_EQ(MimeFromTextureExtension("a.tiff"), "image/tiff");
}

TEST(TextureMimeType, ExtensionMatchIsCaseInsensitive)
{
    EXPECT_EQ(MimeFromTextureExtension("Textures/Albedo.TGA"), "image/x-tga");
    EXPECT_EQ(MimeFromTextureExtension("Textures/Albedo.JpEg"), "image/jpeg");
}

TEST(TextureMimeType, UnknownExtensionFallsBackToPng)
{
    EXPECT_EQ(MimeFromTextureExtension("a.xyz"), "image/png");
    EXPECT_EQ(MimeFromTextureExtension("noextension"), "image/png");
}

TEST(TextureMimeType, ExtensionRoundTripsThroughMime)
{
    for (const std::string_view ext : {".png", ".jpg", ".tga", ".bmp", ".hdr", ".tif"})
    {
        const std::filesystem::path path = std::filesystem::path("tex").replace_extension(ext);
        EXPECT_EQ(TextureExtensionFromMime(MimeFromTextureExtension(path)), ext) << ext;
    }
}

TEST(TextureMimeType, MimeAliasesAndUnknownMapToExtension)
{
    EXPECT_EQ(TextureExtensionFromMime("image/jpg"), ".jpg");
    EXPECT_EQ(TextureExtensionFromMime("image/tga"), ".tga");
    EXPECT_EQ(TextureExtensionFromMime(""), ".png");
    EXPECT_EQ(TextureExtensionFromMime("image/webp"), ".png");
}
