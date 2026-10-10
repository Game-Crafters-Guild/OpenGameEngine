// A hot reload of a model hands its embedded images to the live materials that sample them before
// it releases the old textures: no material binds a texture when it is destroyed (issue #3153).

#include <gtest/gtest.h>

#include "Assets/ModelAsset.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/TextureService.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "TestDeviceHelper.h"

#include <iterator>
#include <memory>
#include <string>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Rendering;

namespace
{

// 1x1 RGBA PNGs: (200, 120, 40, 255) and (40, 200, 120, 255).
constexpr uint8 kOrangePng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
    0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x38, 0x51, 0xa1, 0xf1, 0x1f, 0x00, 0x05, 0xdc, 0x02, 0x68,
    0x88, 0x92, 0x98, 0xf1, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
constexpr uint8 kGreenPng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
    0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xd0, 0x38, 0x51, 0xf1, 0x1f, 0x00, 0x04, 0xec, 0x02, 0x68,
    0x3f, 0xdb, 0x74, 0x34, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

constexpr StringId kAlbedo = HashStringId("albedoMap");

template <size_t N>
Vector<EmbeddedImage> OneImage(const uint8 (&png)[N])
{
    Vector<EmbeddedImage> images(1);
    images[0].MimeType = "image/png";
    images[0].Data.assign(std::begin(png), std::end(png));
    return images;
}

class EmbeddedTextureReload : public testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
        m_Services = std::make_unique<RenderServices>();
        ASSERT_TRUE(m_Services->Initialize(m_Device.get()));
    }

    void TearDown() override
    {
        if (m_Services)
            m_Services->Shutdown();
        if (m_Device)
            m_Device->Shutdown();
    }

    // A material of the model whose albedo is the model's embedded image 0, as model spawning
    // registers it.
    Material* SpawnMaterial(const Vector<EmbeddedImage>& images)
    {
        MaterialDocument doc = MaterialDocument::CreateDefaultPBR("EmbeddedReload");
        doc.textures["albedoMap"] = std::string(kEmbeddedTexturePrefix) + "0";
        Material* mat = m_Services->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
        if (mat)
            Textures().ResolveEmbeddedTextures(mat, doc, m_ModelGuid, images);
        return mat;
    }

    TextureService& Textures() { return m_Services->Textures(); }

    const GUID m_ModelGuid = GUID::Generate();
    std::unique_ptr<IDevice> m_Device;
    std::unique_ptr<RenderServices> m_Services;
};

} // namespace

// The common hot reload rewrites the file with the same images: the material keeps sampling a
// live texture (the shared content survives the reload rather than being released under it).
TEST_F(EmbeddedTextureReload, AReloadWithTheSameImagesKeepsTheMaterialOnALiveTexture)
{
    Material* mat = SpawnMaterial(OneImage(kOrangePng));
    ASSERT_NE(mat, nullptr);
    const TextureHandle before = mat->GetTexture(kAlbedo);
    ASSERT_TRUE(m_Device->IsTextureAlive(before));

    Textures().ReloadEmbeddedForModel(m_ModelGuid, OneImage(kOrangePng));

    EXPECT_TRUE(m_Device->IsTextureAlive(mat->GetTexture(kAlbedo)))
        << "the reload released the texture its material still binds";
    EXPECT_EQ(mat->GetTexture(kAlbedo), before) << "content both payloads share is not uploaded again";
    EXPECT_FALSE(mat->IsTextureAwaited(static_cast<uint32_t>(TextureSlot::kAlbedo)));
}

// A reload with a new image rebinds the material to it, and only then releases the old one.
TEST_F(EmbeddedTextureReload, AReloadWithANewImageRebindsTheMaterialAndReleasesTheOldTexture)
{
    Material* mat = SpawnMaterial(OneImage(kOrangePng));
    ASSERT_NE(mat, nullptr);
    const TextureHandle before = mat->GetTexture(kAlbedo);
    ASSERT_TRUE(m_Device->IsTextureAlive(before));

    Textures().ReloadEmbeddedForModel(m_ModelGuid, OneImage(kGreenPng));

    const TextureHandle after = mat->GetTexture(kAlbedo);
    EXPECT_NE(after, before) << "the material still binds the previous payload's texture";
    EXPECT_TRUE(m_Device->IsTextureAlive(after));
    EXPECT_FALSE(m_Device->IsTextureAlive(before)) << "no material binds it any more, so it is released";
}

// A model that left memory has no images to hand over: its materials' slots await, and the old
// texture is released.
TEST_F(EmbeddedTextureReload, AModelThatLeftMemoryLeavesItsMaterialSlotsAwaited)
{
    Material* mat = SpawnMaterial(OneImage(kOrangePng));
    ASSERT_NE(mat, nullptr);
    const TextureHandle before = mat->GetTexture(kAlbedo);
    ASSERT_TRUE(m_Device->IsTextureAlive(before));

    Textures().ReloadEmbeddedForModel(m_ModelGuid, {});

    EXPECT_FALSE(mat->GetTexture(kAlbedo).IsValid());
    EXPECT_TRUE(mat->IsTextureAwaited(static_cast<uint32_t>(TextureSlot::kAlbedo)));
    EXPECT_FALSE(m_Device->IsTextureAlive(before));
}

// A slot left awaiting because its image had no bytes takes the image once a reload supplies it.
TEST_F(EmbeddedTextureReload, AnAwaitedSlotTakesTheImageAReloadSupplies)
{
    Vector<EmbeddedImage> empty(1);
    empty[0].MimeType = "image/png";
    Material* mat = SpawnMaterial(empty);
    ASSERT_NE(mat, nullptr);
    ASSERT_TRUE(mat->IsTextureAwaited(static_cast<uint32_t>(TextureSlot::kAlbedo)));

    Textures().ReloadEmbeddedForModel(m_ModelGuid, OneImage(kOrangePng));

    EXPECT_TRUE(m_Device->IsTextureAlive(mat->GetTexture(kAlbedo)));
    EXPECT_FALSE(mat->IsTextureAwaited(static_cast<uint32_t>(TextureSlot::kAlbedo)));
}
