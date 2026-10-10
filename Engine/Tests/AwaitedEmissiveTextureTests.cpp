// An emissive map that is assigned but not resident samples black: emission multiplies its map,
// so the white an unassigned emissive slot samples would light the whole surface before the map
// arrives, and forever when it fails (Material.h, AwaitedSlotDefault).

#include <gtest/gtest.h>

#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Assets/TextureCook.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "Engine/Rendering/DDGIMaterialMapAtlas.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/TextureService.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "TestDeviceHelper.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Rendering;

namespace
{

// A 1x1 RGBA PNG, (200, 120, 40, 255).
constexpr uint8 kOnePixelPng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
    0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x38, 0x51, 0xa1, 0xf1, 0x1f, 0x00, 0x05, 0xdc, 0x02, 0x68,
    0x88, 0x92, 0x98, 0xf1, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

constexpr StringId kEmissive = HashStringId("emissiveMap");

class AwaitedEmissiveTexture : public testing::Test
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

    Material* Register(const GUID& guid, const char* emissiveRef)
    {
        MaterialDocument doc = MaterialDocument::CreateDefaultPBR("AwaitedEmissive");
        if (emissiveRef)
            doc.textures["emissiveMap"] = emissiveRef;
        return m_Services->Materials().RegisterMaterialFromDocument(guid, doc);
    }

    TextureService& Textures() { return m_Services->Textures(); }

    std::unique_ptr<IDevice> m_Device;
    std::unique_ptr<RenderServices> m_Services;
};

// The emissive layer DDGI's map atlas records for `material` after a refresh.
float AtlasEmissiveLayer(DDGIMaterialMapAtlas& atlas, const Material& material)
{
    atlas.Refresh();
    return atlas.GetRecordForMaterialSlot(material.GetGpuSceneMaterialIndex()).EmissiveLayer;
}

namespace fs = std::filesystem;

// The editor's texture assign loads through an upload job, which needs the engine's asset
// manager and job pool: a running engine over a scratch workspace, stopped before the test ends
// so the isolation tripwire reads an idle pool.
class AwaitedEmissiveTextureUploadJob : public testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
        m_Workspace = fs::temp_directory_path() / ("ge-awaited-emissive-" + GUID::Generate().ToString());
        fs::create_directories(m_Workspace / "Assets");
        m_PreviousDirectory = fs::current_path();
        auto& engine = EngineCore::GetInstance();
        ScriptsConfig scripts;
        scripts.disableClr = true;
        scripts.enableHotReload = false;
        scripts.enableAsyncHotReload = false;
        scripts.enableAutoProjectGeneration = false;
        engine.SetScriptsConfig(scripts);
        ApplicationConfig config;
        config.WorkspaceDirectory = m_Workspace.string();
        config.EnableEditor = false;
        ASSERT_TRUE(engine.Initialize(config));
        m_Materials.Initialize(m_Device.get());
        ASSERT_TRUE(m_Textures.Initialize(m_Device.get(), m_Materials,
                                          RendererProfile::FromCapabilities(m_Device->GetCapabilities())));
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        m_Textures.Shutdown();
        m_Materials.Shutdown();
        m_Device.reset();
        EngineCore::GetInstance().Shutdown();
        fs::current_path(m_PreviousDirectory);
        std::error_code error;
        fs::remove_all(m_Workspace, error);
    }

    // A 1x1 PNG, or (`corrupt`) a file that is no PNG at all, so its decode fails.
    GUID WriteTexture(bool corrupt) const
    {
        const fs::path path = m_Workspace / "Assets" / (GUID::Generate().ToString() + ".png");
        constexpr uint8 kNotAnImage[] = {'n', 'o', 't', ' ', 'a', 'n', ' ', 'i', 'm', 'a', 'g', 'e'};
        std::ofstream stream(path, std::ios::binary);
        if (corrupt)
            stream.write(reinterpret_cast<const char*>(kNotAnImage), sizeof(kNotAnImage));
        else
            stream.write(reinterpret_cast<const char*>(kOnePixelPng), sizeof(kOnePixelPng));
        stream.close();
        auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
        EXPECT_TRUE(registry.SetMetaValue(path, kTextureCompressionMetaKey, "none"));
        return registry.GetAssetGUID(path);
    }

    // Pumps the upload until the material has no bind left waiting (a landed or a failed load).
    void PumpUntilSettled(const GUID& material)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!m_Textures.IsMaterialTextureBindingComplete(material) && std::chrono::steady_clock::now() < deadline)
        {
            m_Textures.FlushPendingUploads();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        m_Textures.FlushPendingUploads();
        ASSERT_TRUE(m_Textures.IsMaterialTextureBindingComplete(material)) << "the upload job never settled";
    }

    fs::path m_Workspace;
    fs::path m_PreviousDirectory;
    std::unique_ptr<IDevice> m_Device;
    MaterialRegistry m_Materials;
    TextureService m_Textures;
};

} // namespace

// An unassigned emissive slot samples white, the identity emission multiplies; an assigned one
// whose texture is evicted for a reload samples black until the reload binds it.
TEST_F(AwaitedEmissiveTexture, AnEvictedEmissiveMapAwaitsItsReloadInBlack)
{
    const GUID matGuid = GUID::Generate();
    const GUID texGuid = GUID::Generate();
    Material* mat = Register(matGuid, nullptr);
    ASSERT_NE(mat, nullptr);
    EXPECT_EQ(mat->GetBindlessTextureIndex(kEmissive), Textures().DefaultWhiteBindlessIndex());

    mat->SetBindlessTextureIndex(kEmissive, 42u);
    Textures().TrackMaterialTextureRefForTesting(matGuid, kEmissive, texGuid);
    Textures().Evict(texGuid);

    EXPECT_EQ(mat->GetBindlessTextureIndex(kEmissive), Textures().DefaultBlackBindlessIndex());
    // The slot carries no texture of its own while it waits: consumers that read the texture (the
    // Classic bind group, the DDGI map atlas) see it as awaited, not as a texture to sample.
    EXPECT_FALSE(mat->GetTexture(kEmissive).IsValid());
    EXPECT_TRUE(mat->IsTextureAwaited(static_cast<uint32_t>(TextureSlot::kEmissive)));
}

// A model's embedded emissive image that cannot become a texture leaves the slot black; once it
// is resident the slot samples the texture itself.
TEST_F(AwaitedEmissiveTexture, AnEmbeddedEmissiveMapIsBlackWhenItFailsAndItsOwnTextureWhenResident)
{
    const GUID modelGuid = GUID::Generate();
    MaterialDocument doc = MaterialDocument::CreateDefaultPBR("EmbeddedEmissive");
    doc.textures["emissiveMap"] = std::string(kEmbeddedTexturePrefix) + "0";
    Material* mat = m_Services->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
    ASSERT_NE(mat, nullptr);

    Vector<EmbeddedImage> images(1);
    images[0].MimeType = "image/png";
    Textures().ResolveEmbeddedTextures(mat, doc, modelGuid, images);
    EXPECT_EQ(mat->GetBindlessTextureIndex(kEmissive), Textures().DefaultBlackBindlessIndex())
        << "an image with no bytes never becomes a texture";

    images[0].Data.assign(std::begin(kOnePixelPng), std::end(kOnePixelPng));
    Textures().ResolveEmbeddedTextures(mat, doc, modelGuid, images);
    const uint32_t resident = mat->GetBindlessTextureIndex(kEmissive);
    EXPECT_NE(resident, Textures().DefaultBlackBindlessIndex());
    EXPECT_NE(resident, Textures().DefaultWhiteBindlessIndex());
    EXPECT_TRUE(mat->GetTexture(kEmissive).IsValid());
    EXPECT_FALSE(mat->IsTextureAwaited(static_cast<uint32_t>(TextureSlot::kEmissive)));
}

// A document reference that resolves to no asset is an assigned map that failed: black.
TEST_F(AwaitedEmissiveTexture, AnEmissiveReferenceThatResolvesToNoAssetStaysBlack)
{
    Material* mat = Register(GUID::Generate(), "Textures/missing_emission.png");
    ASSERT_NE(mat, nullptr);
    EXPECT_EQ(mat->GetBindlessTextureIndex(kEmissive), Textures().DefaultBlackBindlessIndex());
}

// A surface that declares its own texture list places emissiveMap at the ordinal it declares.
// DDGI asks whether the emissive map is awaited by that name: an awaited emissive map emits
// nothing, and an awaited texture on the standard emissive ordinal leaves emission alone.
TEST_F(AwaitedEmissiveTexture, DDGIReadsTheAwaitedEmissiveMapWhereTheSurfaceDeclaresIt)
{
    Material* mat = Register(GUID::Generate(), nullptr);
    ASSERT_NE(mat, nullptr);
    const StringId detail = HashStringId("detailMap");
    mat->SetTextureSlotMap({{"emissiveMap", 0}, {"detailMap", static_cast<uint8_t>(TextureSlot::kEmissive)}});
    DDGIMaterialMapAtlas atlas(m_Device.get(), &m_Services->Materials());

    mat->MarkTextureAwaited(detail);
    EXPECT_EQ(AtlasEmissiveLayer(atlas, *mat), DDGIMaterialMapAtlas::kNoLayer)
        << "an awaited texture on the standard emissive ordinal is not an emissive map";

    mat->SetTexture(detail, TextureHandle{});
    mat->MarkTextureAwaited(kEmissive);
    EXPECT_EQ(AtlasEmissiveLayer(atlas, *mat), DDGIMaterialMapAtlas::kAwaitedLayer)
        << "an awaited emissive map declared at another ordinal emits nothing";
}

// An emissive map assigned through an upload job samples black from the assign until the job
// lands, and stays black when the job fails; a job that lands binds the texture itself.
TEST_F(AwaitedEmissiveTextureUploadJob, AnAssignedEmissiveMapIsBlackUntilItsUploadJobLands)
{
    constexpr uint32_t kEmissiveOrdinal = static_cast<uint32_t>(TextureSlot::kEmissive);
    MaterialDocument doc = MaterialDocument::CreateDefaultPBR("AwaitedEmissiveUploadJob");
    const GUID matGuid = GUID::Generate();
    Material* mat = m_Materials.Register(matGuid, doc);
    ASSERT_NE(mat, nullptr);

    doc.textures["emissiveMap"] = WriteTexture(/*corrupt=*/true).ToString();
    m_Textures.UpdateMaterialTextures(matGuid, doc);
    EXPECT_EQ(mat->GetBindlessTextureIndex(kEmissive), m_Textures.DefaultBlackBindlessIndex()) << "in flight";
    EXPECT_TRUE(mat->IsTextureAwaited(kEmissiveOrdinal)) << "in flight";
    PumpUntilSettled(matGuid);
    EXPECT_EQ(mat->GetBindlessTextureIndex(kEmissive), m_Textures.DefaultBlackBindlessIndex()) << "failed";
    EXPECT_TRUE(mat->IsTextureAwaited(kEmissiveOrdinal)) << "failed";

    doc.textures["emissiveMap"] = WriteTexture(/*corrupt=*/false).ToString();
    m_Textures.UpdateMaterialTextures(matGuid, doc);
    EXPECT_EQ(mat->GetBindlessTextureIndex(kEmissive), m_Textures.DefaultBlackBindlessIndex()) << "in flight";
    PumpUntilSettled(matGuid);
    EXPECT_TRUE(mat->GetTexture(kEmissive).IsValid()) << "landed";
    EXPECT_NE(mat->GetBindlessTextureIndex(kEmissive), m_Textures.DefaultBlackBindlessIndex()) << "landed";
    EXPECT_FALSE(mat->IsTextureAwaited(kEmissiveOrdinal)) << "landed";
}
