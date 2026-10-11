#include <gtest/gtest.h>

#include "AssetCore/SharedFileRead.h"
#include "AssetDatabase/AssetStore_TextJsonl.h"
#include "Assets/AssetManager.h"
#include "Assets/PackagedTexturePayload.h"
#include "Assets/Packages/PackageMounts.h"
#include "Engine/Build/TexturePackageCook.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/TextureService.h"
#include "EngineLogCapture.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RendererProfile.h"
#include "TestTempDir.h"

#include <bit>
#include <cmath>
#include <chrono>
#include <future>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(GE_HAVE_KTX)
#include <ktx.h>
#endif

using namespace GameEngine;
namespace
{
namespace fs = std::filesystem;
using Metadata = decltype(AssetDatabase::AssetRecord::kv);

Vector<uint8> SmallTga(uint8 red = 80, bool cutout = false)
{
    Vector<uint8> bytes(18, 0);
    const uint8 width = cutout ? 19 : 8, height = cutout ? 13 : 8;
    bytes[2] = 2; bytes[12] = width; bytes[14] = height; bytes[16] = 32; bytes[17] = 0x28;
    for (size_t i = 0; i != static_cast<size_t>(width) * height; ++i)
    {
        bytes.push_back(180); bytes.push_back(120); bytes.push_back(red);
        constexpr uint8 alpha[] = {0, 0, 17, 63, 127, 128, 190, 255, 255, 255, 1};
        bytes.push_back(cutout ? alpha[(i * 7 + i / width) % std::size(alpha)] : 255);
    }
    return bytes;
}

// Names the derived cache actually holds, so a missing artifact reports what
// landed instead of only that nothing matched.
std::string ListDirectory(const fs::path& directory)
{
    std::string names;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(directory, ec))
        names += entry.path().filename().string() + " ";
    return names.empty() ? "<nothing>" : names;
}

std::string ReadWholeFile(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void WriteBytes(const fs::path& path, const Vector<uint8>& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    ASSERT_TRUE(stream.good());
}

TextureCookInputs Inputs(const Metadata& metadata)
{
    TextureCookInputs inputs;
    std::string error;
    EXPECT_TRUE(ResolveTextureCookInputs(".tga", [&](const char* key, std::string& value) {
        const auto it = metadata.find(key);
        if (it == metadata.end()) return false;
        value = it->second;
        return true;
    }, inputs, error)) << error;
    return inputs;
}

#if defined(GE_HAVE_KTX)
// A KTX2 container of BC7 levels for an 8x8 sRGB texture (8, 4, 2 and 1 texels:
// 4, 1, 1 and 1 blocks), each block byte its level index plus one, so an
// adopted payload is recognizable byte for byte. Written by hand because a
// host without a BC encoder cannot bake one.
Vector<uint8> HandWrittenBc7Ktx2()
{
    constexpr ktx_uint32_t kVkFormatBc7SrgbBlock = 146;
    constexpr uint32_t kLevels = 4;
    constexpr size_t kBc7BlockBytes = 16;
    ktxTextureCreateInfo createInfo{};
    createInfo.vkFormat = kVkFormatBc7SrgbBlock;
    createInfo.baseWidth = 8;
    createInfo.baseHeight = 8;
    createInfo.baseDepth = 1;
    createInfo.numDimensions = 2;
    createInfo.numLevels = kLevels;
    createInfo.numLayers = 1;
    createInfo.numFaces = 1;
    createInfo.isArray = KTX_FALSE;
    createInfo.generateMipmaps = KTX_FALSE;
    ktxTexture2* texture = nullptr;
    EXPECT_EQ(ktxTexture2_Create(&createInfo, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture), KTX_SUCCESS);
    if (!texture)
        return {};
    for (uint32_t level = 0; level < kLevels; ++level)
    {
        const std::vector<uint8> blocks((level == 0 ? 4 : 1) * kBc7BlockBytes, static_cast<uint8>(level + 1));
        EXPECT_EQ(ktxTexture_SetImageFromMemory(ktxTexture(texture), level, 0, 0, blocks.data(), blocks.size()),
                  KTX_SUCCESS);
    }
    ktx_uint8_t* bytes = nullptr;
    ktx_size_t size = 0;
    EXPECT_EQ(ktxTexture_WriteToMemory(ktxTexture(texture), &bytes, &size), KTX_SUCCESS);
    ktxTexture_Destroy(ktxTexture(texture));
    Vector<uint8> out;
    if (bytes)
    {
        out.assign(bytes, bytes + size);
        free(bytes);
    }
    return out;
}
#endif

// Null off Apple platforms, where asking the factory for Metal would bring up
// another backend's device only for the test to skip.
std::unique_ptr<Rendering::IDevice> CreateHeadlessMetalDevice()
{
#if defined(__APPLE__)
    Rendering::DeviceDesc desc{};
    desc.preferredAPI = Rendering::GraphicsAPI::Metal;
    desc.enableSwapchain = false;
    auto device = Rendering::DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc))
        return nullptr;
    if (device->GetAPI() != Rendering::GraphicsAPI::Metal)
    {
        device->Shutdown();
        return nullptr;
    }
    return device;
#else
    return nullptr;
#endif
}

// Shuts a texture service and its device down when the test leaves, also
// through a failed ASSERT.
struct TextureServiceShutdown
{
    Engine::Renderer::TextureService& Textures;
    Rendering::IDevice& Device;
    ~TextureServiceShutdown()
    {
        Textures.Shutdown();
        Device.Shutdown();
    }
};

class TexturePackageCookTest : public testing::Test
{
protected:
    TestUtils::ScopedTempDir Temp{TestUtils::MakeUniqueTempDirectory("texture-package")};
    AssetManager Manager;
    AssetManifest Manifest;
    TexturePackageCookStats Stats;
    std::string Error;
    std::shared_ptr<std::atomic<bool>> Cancel = std::make_shared<std::atomic<bool>>(false);
    TextureGpuTranscodeTarget PreviousTarget = TextureAsset::GetGpuTranscodeTarget();
    const GUID Id = GUID::Derive(GUID::Null(), "texture-package-test");
    Metadata Meta{{kTextureColorSpaceMetaKey, "srgb"}, {kTextureCompressionMetaKey, "none"},
                  {kTextureUsageMetaKey, "color"}, {kTextureFilterMetaKey, "point"}};
    fs::path MountRoot;
    bool Cutout = false;
    JobSystem::WorkStealingThreadPool* ManifestParsePool = nullptr;

    void SetUp() override
    {
        ASSERT_TRUE(Manager.Initialize());
        StageSource();
    }
    void TearDown() override
    {
        TextureAsset::SetGpuTranscodeTarget(PreviousTarget);
        Manager.Shutdown();
    }
    void StageSource(const std::string& alias = "project", uint8 red = 80)
    {
        MountRoot = alias == "project" ? Temp.Path() : Temp.Path() / "Packages" / alias;
        const auto source = MountRoot / "Assets" / "Leaves.tga";
        WriteBytes(source, SmallTga(red, Cutout));
        AssetDatabase::AssetRecord record;
        record.guid = Id; record.type = AssetType::Texture; record.path = "Leaves.tga";
        // Exercise the shipping key list before serializing the same manifest
        // format consumed by the bake and the Player mount.
        for (const auto& key : kRuntimeAssetMetadataKeys)
        {
            if (key.Type != AssetType::Texture)
                continue;
            if (const auto found = Meta.find(key.Name); found != Meta.end())
                record.kv[key.Name] = found->second;
        }
        AssetDatabase::AssetStore_TextJsonl store(nullptr);
        ASSERT_TRUE(store.UpsertAsset(record, &Error)) << Error;
        ASSERT_TRUE(store.SaveToFile(MountRoot / "Assets" / ".assetmanifest", &Error)) << Error;
        Manifest.entries = {{Id, AssetType::Texture, source,
            source.lexically_relative(Temp.Path()), 0, alias}};
    }
    void StageMaterialBindings(const std::vector<std::pair<std::string, std::string>>& bindings)
    {
        const auto materialPath = MountRoot / "Assets" / "Cold.material";
        std::string json = R"({"schemaVersion":3,"textures":{)";
        for (const auto& [slot, reference] : bindings)
        {
            if (json.back() != '{') json += ',';
            json += "\"" + slot + "\":\"" + reference + "\"";
        }
        json += "}}";
        WriteBytes(materialPath, Vector<uint8>(json.begin(), json.end()));
        Manifest.entries.push_back({GUID::Derive(Id, "material"), AssetType::Material, materialPath,
                                    materialPath.lexically_relative(Temp.Path()), 0, "project"});
        Mount();
        const auto usages = CollectPackagedTextureUsages(Temp.Path(), Manifest, Manager);
        AssetDatabase::AssetRecord record;
        record.guid = Id; record.type = AssetType::Texture; record.path = "Leaves.tga";
        const auto usage = usages.find(Id);
        StageTextureImportMetadata(Manager.GetRegistry(), Manifest.entries[0],
            usage == usages.end() ? TextureCookUsage::Auto : usage->second, record);
        AssetDatabase::AssetStore_TextJsonl store(nullptr);
        ASSERT_TRUE(store.UpsertAsset(record, &Error)) << Error;
        ASSERT_TRUE(store.SaveToFile(MountRoot / "Assets" / ".assetmanifest", &Error)) << Error;
        // Source registry remains at its original metadata until the Player remount.
        std::string original;
        EXPECT_EQ(Manager.GetRegistry().TryGetMetaValue(Manifest.entries[0].sourcePath,
                                                      kTextureUsageMetaKey, original), Meta.contains(kTextureUsageMetaKey));
        if (Meta.contains(kTextureUsageMetaKey)) EXPECT_EQ(original, Meta.at(kTextureUsageMetaKey));
        Meta = record.kv;
    }
    bool Bake()
    {
        return StagePackagedTextureCooks(Temp.Path(), Manifest, Manager.GetRegistry(),
            TextureCookEncodeQualityFor(TextureCookOutput::BC7), /*encodeWorkers=*/nullptr, ManifestParsePool,
            [this]() { return Cancel->load(); }, Stats, Error);
    }
    fs::path Artifact(TextureCookOutput output, uint8 red = 80) const
    {
        const auto bytes = SmallTga(red, Cutout);
        return MountRoot / "Tex" / TextureCookArtifactName(Id,
            ComputeTextureCookSourceHash(bytes.data(), bytes.size()), Inputs(Meta), output);
    }
    void Mount()
    {
        ASSERT_TRUE(Manager.RegisterSource(MakePackageMount("project", MountRoot / "Assets")));
        const DerivedArtifactPolicy policy = Manager.GetRegistry().GetDerivedArtifactPolicy(Id);
        EXPECT_FALSE(policy.InfersTextureImportSettings);
        EXPECT_FALSE(policy.CooksOnMiss);
    }
    std::unique_ptr<TextureAsset> LoadInWorkerContext()
    {
        AssetManager::ScopedThreadAssetManager context(&Manager);
        auto texture = std::make_unique<TextureAsset>(Id, MountRoot / "Assets" / "Leaves.tga");
        EXPECT_TRUE(texture->Load());
        return texture;
    }
};

TEST_F(TexturePackageCookTest, ColdMaterialExportInfersColorWithoutViewingOrMutatingSource)
{
    if (!IsTextureCookEncoderAvailable()) GTEST_SKIP() << "BC encoder unavailable";
    Meta.clear(); StageSource();
    StageMaterialBindings({{"albedoMap", Id.ToString()}, {"emissiveMap", Id.ToString()}});
    EXPECT_EQ(Meta.at(kTextureUsageMetaKey), "color");
    EXPECT_FALSE(Meta.contains(kTextureColorSpaceMetaKey));
    ASSERT_TRUE(Bake()) << Error;
    EXPECT_EQ(Stats.UnclassifiedTextures, 0u);
    EXPECT_TRUE(fs::exists(Artifact(TextureCookOutput::BC7)));
    AssetDatabase::AssetStore_TextJsonl store(nullptr);
    ASSERT_TRUE(store.LoadFromFile(MountRoot / "Assets" / ".assetmanifest", &Error));
    AssetDatabase::AssetRecord record;
    ASSERT_TRUE(store.TryGetAsset(Id, record));
    for (const auto& [key, value] : Meta) EXPECT_EQ(record.kv.at(key), value);
    EXPECT_TRUE(record.kv.contains(kTexturePackagedPortableMetaKey));
    EXPECT_FALSE(fs::exists(MountRoot / "Assets" / "Leaves.tga"));
    // Remount in a fresh Player context to consume the staged metadata and cook.
    AssetManager player;
    ASSERT_TRUE(player.Initialize());
    ASSERT_TRUE(player.RegisterSource(MakePackageMount("project", MountRoot / "Assets")));
    {
        AssetManager::ScopedThreadAssetManager context(&player);
        TextureAsset::SetGpuTranscodeTarget(TextureGpuTranscodeTarget::BC7);
        TextureAsset texture(Id, MountRoot / "Assets" / "Leaves.tga");
        ASSERT_TRUE(texture.Load());
        EXPECT_EQ(texture.GetFormat(), TextureFormat::BC7);
        EXPECT_EQ(texture.GetMipmapLevels(), 4u);
    }
    player.Shutdown();
}

// A staged manifest past the store's parallel threshold is parsed on the
// cook's manifest pool: the bake publishes the parse to it (the census's
// cumulative global pushes grow; nothing else in the bake uses that pool).
TEST_F(TexturePackageCookTest, ALargeStagedManifestParsesOnTheManifestPool)
{
    if (!IsTextureCookEncoderAvailable()) GTEST_SKIP() << "BC encoder unavailable";
    const fs::path manifestPath = MountRoot / "Assets" / ".assetmanifest";
    {
        AssetDatabase::AssetStore_TextJsonl store(nullptr);
        ASSERT_TRUE(store.LoadFromFile(manifestPath, &Error)) << Error;
        // Past AssetStore_TextJsonl's 4000-line parallel threshold.
        for (int i = 0; i < 4500; ++i)
        {
            AssetDatabase::AssetRecord padding;
            padding.guid = GUID::Derive(Id, "padding-" + std::to_string(i));
            padding.type = AssetType::Material;
            padding.path = "Padding/pad_" + std::to_string(i) + ".material";
            ASSERT_TRUE(store.UpsertAsset(padding, &Error)) << Error;
        }
        ASSERT_TRUE(store.SaveToFile(manifestPath, &Error)) << Error;
    }

    JobSystem::WorkStealingThreadPool pool(4);
    ManifestParsePool = &pool;
    const auto before = pool.GetStatistics();
    ASSERT_TRUE(Bake()) << Error;
    const auto after = pool.GetStatistics();
    EXPECT_EQ(Stats.Textures, 1u);
    EXPECT_GT(after.GlobalPushes - before.GlobalPushes, 0u) << "the staged manifest was parsed on the calling thread";
}

// The packaged Player mounts its content through AssetManager::Initialize with a
// writable cache root in the user directory. The shipped content is read-only and
// never cooks, so that root must not move where it looks for bakes: they are the
// ones the export published beside the content, and nothing else fills a cache.
TEST_F(TexturePackageCookTest, PackagedPlayerMountAdoptsTheBakeTheExportShipped)
{
    if (!IsTextureCookEncoderAvailable()) GTEST_SKIP() << "BC encoder unavailable";
    Meta.clear(); StageSource();
    StageMaterialBindings({{"albedoMap", Id.ToString()}});
    ASSERT_TRUE(Bake()) << Error;
    ASSERT_TRUE(fs::exists(Artifact(TextureCookOutput::BC7)));

    AssetManager player;
    const fs::path userData = Temp.Path() / "UserData";
    ASSERT_TRUE(player.Initialize(MountRoot / "Assets", nullptr, userData / "AssetDatabase.assetdb",
                                  userData / ".Cache" / "AssetDatabase"));
    std::vector<std::string> logLines;
    {
        TestLog::ScopedEngineLogCapture capture(&logLines);
        Logger::Log::Info("probe: the capture is live");
        AssetManager::ScopedThreadAssetManager context(&player);
        TextureAsset::SetGpuTranscodeTarget(TextureGpuTranscodeTarget::BC7);
        TextureAsset texture(Id, MountRoot / "Assets" / "Leaves.tga");
        ASSERT_TRUE(texture.Load());
        EXPECT_EQ(texture.GetFormat(), TextureFormat::BC7);
        EXPECT_EQ(texture.GetMipmapLevels(), 4u);
        Logger::Log::Flush();
    }
    EXPECT_EQ(TestLog::CountLinesContaining(logLines, "probe: the capture is live"), 1u);
    EXPECT_EQ(TestLog::CountLinesContaining(logLines, "packaged texture bake missing"), 0u)
        << TestLog::FirstLineContaining(logLines, "packaged texture bake missing");
    EXPECT_FALSE(fs::exists(userData / ".Cache" / "Tex")) << "a packaged Player wrote a texture cache";
    player.Shutdown();
}

TEST_F(TexturePackageCookTest, MaterialXUsesStagedBytesWithSourcePathAndNoRegistryMutation)
{
    Meta.clear(); StageSource(); Mount();
    const auto authoringMaterial = MountRoot / "Assets" / "Cold.mtlx";
    const auto stagedPath = "StagingElsewhere/Cold.mtlx";
    const std::string xml = R"(<materialx version="1.39">
      <image name="base" type="color3"><input name="file" type="filename" value="Leaves.tga"/></image>
      <image name="missing" type="color3"><input name="file" type="filename" value="NeverRegistered.tga"/></image>
      <open_pbr_surface name="M" type="surfaceshader">
        <input name="base_color" type="color3" nodename="base"/>
        <input name="emission_color" type="color3" nodename="missing"/>
      </open_pbr_surface></materialx>)";
    WriteBytes(Temp.Path() / stagedPath, Vector<uint8>(xml.begin(), xml.end()));
    Manifest.entries.push_back({GUID::Derive(Id, "mtlx"), AssetType::Material, authoringMaterial,
                                stagedPath, 0, "project"});
    AssetManager::ScopedThreadAssetManager callerContext(&Manager);
    const auto usages = CollectPackagedTextureUsages(Temp.Path(), Manifest, Manager);
    ASSERT_TRUE(usages.contains(Id));
    EXPECT_EQ(usages.at(Id), TextureCookUsage::Color);
    EXPECT_EQ(AssetManager::GetThreadCurrent(), &Manager);
    EXPECT_TRUE(Manager.GetRegistry().GetAssetGUID(MountRoot / "Assets" / "NeverRegistered.tga").IsNull());
    EXPECT_FALSE(fs::exists(authoringMaterial)); // Only the staged bytes were read.
}

TEST_F(TexturePackageCookTest, StaleGuidUsesSourceAwarePathCompanion)
{
    Meta.clear(); StageSource(); Mount();
    const auto material = MountRoot / "Assets" / "Cold.material";
    const std::string json = R"({"schemaVersion":3,"textures":{"normalMap":{"guid":")" +
        GUID::Derive(Id, "old").ToString() + R"(","path":"Leaves.tga"}}})";
    WriteBytes(material, Vector<uint8>(json.begin(), json.end()));
    Manifest.entries.push_back({GUID::Derive(Id, "material"), AssetType::Material, material,
                                material.lexically_relative(Temp.Path()), 0, "project"});
    const auto usages = CollectPackagedTextureUsages(Temp.Path(), Manifest, Manager);
    ASSERT_TRUE(usages.contains(Id));
    EXPECT_EQ(usages.at(Id), TextureCookUsage::Normal);
}

TEST_F(TexturePackageCookTest, ColdPathBoundNormalGetsLinearBC5)
{
    if (!IsTextureCookEncoderAvailable()) GTEST_SKIP() << "BC encoder unavailable";
    Meta = {{kTextureColorSpaceMetaKey, "auto"}};
    StageSource();
    StageMaterialBindings({{"normalMap", "Leaves.tga"}});
    EXPECT_EQ(Meta.at(kTextureUsageMetaKey), "normal");
    EXPECT_EQ(Meta.at(kTextureColorSpaceMetaKey), "linear");
    ASSERT_TRUE(Bake()) << Error;
    EXPECT_TRUE(fs::exists(Artifact(TextureCookOutput::BC5)));
}

TEST_F(TexturePackageCookTest, AutoColorSpaceRemainsInferableForDataSlots)
{
    Meta = {{kTextureColorSpaceMetaKey, "auto"}};
    StageSource();
    StageMaterialBindings({{"metallicRoughnessMap", Id.ToString()}});
    EXPECT_EQ(Meta.at(kTextureUsageMetaKey), "packed");
    EXPECT_EQ(Meta.at(kTextureColorSpaceMetaKey), "linear");
    ASSERT_TRUE(Bake()) << Error;
    EXPECT_EQ(Stats.UnclassifiedTextures, 0u);
}

TEST_F(TexturePackageCookTest, UnresolvedMaterialReferenceKeepsTextureConservative)
{
    Meta.clear(); StageSource();
    StageMaterialBindings({{"normalMap", "missing-texture.tga"}});
    EXPECT_FALSE(Meta.contains(kTextureUsageMetaKey));
    EXPECT_FALSE(Meta.contains(kTextureColorSpaceMetaKey));
    ASSERT_TRUE(Bake()) << Error;
    EXPECT_EQ(Stats.UnclassifiedTextures, 1u);
}

TEST_F(TexturePackageCookTest, ColdOrmSharedWithAoPreservesPackedChannels)
{
    if (!IsTextureCookEncoderAvailable()) GTEST_SKIP() << "BC encoder unavailable";
    Meta.clear(); StageSource();
    StageMaterialBindings({{"aoMap", Id.ToString()}, {"metallicRoughnessMap", Id.ToString()}});
    EXPECT_EQ(Meta.at(kTextureUsageMetaKey), "packed");
    EXPECT_EQ(Meta.at(kTextureColorSpaceMetaKey), "linear");
    ASSERT_TRUE(Bake()) << Error;
    EXPECT_TRUE(fs::exists(Artifact(TextureCookOutput::BC7)));
    EXPECT_FALSE(fs::exists(Artifact(TextureCookOutput::BC4)));
}

// The occlusion-roughness-metallic map a first aoMap bind tagged Mask ships Packed, not BC4.
TEST_F(TexturePackageCookTest, AuthoredMaskWidensForAPackedBinding)
{
    if (!IsTextureCookEncoderAvailable()) GTEST_SKIP() << "BC encoder unavailable";
    Meta = {{kTextureUsageMetaKey, "mask"}};
    StageSource();
    StageMaterialBindings({{"aoMap", Id.ToString()}, {"metallicRoughnessMap", Id.ToString()}});
    EXPECT_EQ(Meta.at(kTextureUsageMetaKey), "packed");
    EXPECT_EQ(Meta.at(kTextureColorSpaceMetaKey), "linear");
    ASSERT_TRUE(Bake()) << Error;
    EXPECT_TRUE(fs::exists(Artifact(TextureCookOutput::BC7)));
    EXPECT_FALSE(fs::exists(Artifact(TextureCookOutput::BC4)));
}

TEST_F(TexturePackageCookTest, ConflictingBindingsStayConservative)
{
    Meta.clear(); StageSource();
    StageMaterialBindings({{"albedoMap", Id.ToString()}, {"normalMap", Id.ToString()}});
    EXPECT_FALSE(Meta.contains(kTextureUsageMetaKey));
    EXPECT_FALSE(Meta.contains(kTextureColorSpaceMetaKey));
    ASSERT_TRUE(Bake()) << Error;
    EXPECT_EQ(Stats.UnclassifiedTextures, 1u);
    EXPECT_EQ(Stats.Artifacts, 1u);
}

TEST_F(TexturePackageCookTest, UnknownSlotPreventsGuessingFromAnotherKnownBinding)
{
    Meta.clear(); StageSource();
    StageMaterialBindings({{"customLookup", Id.ToString()}, {"normalMap", Id.ToString()}});
    EXPECT_FALSE(Meta.contains(kTextureUsageMetaKey));
    ASSERT_TRUE(Bake()) << Error;
    EXPECT_EQ(Stats.UnclassifiedTextures, 1u);
}

TEST_F(TexturePackageCookTest, ExplicitImportMetadataWinsOverMaterialInference)
{
    StageMaterialBindings({{"normalMap", Id.ToString()}});
    EXPECT_EQ(Meta.at(kTextureUsageMetaKey), "color");
    EXPECT_EQ(Meta.at(kTextureColorSpaceMetaKey), "srgb");
    EXPECT_EQ(Meta.at(kTextureCompressionMetaKey), "none");
    ASSERT_TRUE(Bake()) << Error;
    EXPECT_EQ(Stats.Artifacts, 1u);
    EXPECT_TRUE(fs::exists(Artifact(TextureCookOutput::Uncompressed)));
}

TEST_F(TexturePackageCookTest, WholeDirectoryFallbackReportsUnregisteredTexture)
{
    Manifest.entries[0].guid = GUID::Null();
    Manifest.entries[0].type = AssetType::Unknown;
    ASSERT_TRUE(Bake()) << Error;
    EXPECT_EQ(Stats.SkippedTextures, 1u);
    EXPECT_EQ(Stats.Artifacts, 0u);
}

TEST_F(TexturePackageCookTest, PreservesAuthoredLeavesAndPortableSamplerMetadata)
{
    // Real Farmlands regression: explicit none/color was lost and became BC7.
    ASSERT_TRUE(Bake()) << Error;
    EXPECT_EQ(Stats.Textures, 1u); EXPECT_EQ(Stats.Artifacts, 1u);
    EXPECT_GT(Stats.Bytes, 0u);
    Mount();
    TextureAsset::SetGpuTranscodeTarget(TextureGpuTranscodeTarget::BC7);
    auto texture = LoadInWorkerContext();
    EXPECT_EQ(texture->GetFormat(), TextureFormat::RGBA8);
    EXPECT_EQ(texture->GetColorSpace(), TextureColorSpace::SRGB);
    EXPECT_EQ(texture->GetMipmapLevels(), 4u);
    std::string value;
    ASSERT_TRUE(Manager.GetRegistry().TryGetMetaValue(MountRoot / "Assets" / "Leaves.tga", kTextureFilterMetaKey, value));
    EXPECT_EQ(value, "point");
}

TEST_F(TexturePackageCookTest, PackedOrmKeepsAllChannelsAndProvidesPortableVariant)
{
    if (!IsTextureCookEncoderAvailable()) GTEST_SKIP() << "BC encoder unavailable";
    Meta[kTextureColorSpaceMetaKey] = "linear";
    Meta[kTextureCompressionMetaKey] = "auto";
    Meta[kTextureUsageMetaKey] = "packed";
    StageSource();
    ASSERT_TRUE(Bake()) << Error;
    EXPECT_EQ(Stats.Artifacts, 2u);
    ASSERT_TRUE(fs::exists(Artifact(TextureCookOutput::BC7)));
    ASSERT_TRUE(fs::exists(Artifact(TextureCookOutput::Uncompressed)));
    Mount();
    TextureAsset::SetGpuTranscodeTarget(TextureGpuTranscodeTarget::BC7);
    auto bc = LoadInWorkerContext();
    EXPECT_EQ(bc->GetFormat(), TextureFormat::BC7); // Never inferred single-channel BC4.
    EXPECT_EQ(bc->GetColorSpace(), TextureColorSpace::Linear);
    TextureAsset::SetGpuTranscodeTarget(TextureGpuTranscodeTarget::RGBA32);
    auto portable = LoadInWorkerContext();
    EXPECT_EQ(portable->GetFormat(), TextureFormat::RGBA8);
    ASSERT_NE(portable->GetPixelData(), nullptr);
    ASSERT_GE(portable->GetDataSize(), 3u);
    EXPECT_EQ(portable->GetPixelData()[0], 80);
    EXPECT_EQ(portable->GetPixelData()[1], 120);
    EXPECT_EQ(portable->GetPixelData()[2], 180);
    EXPECT_EQ(portable->GetMipmapLevels(), 4u);
}

TEST_F(TexturePackageCookTest, ReusesOnlyValidExactSourceAndSettingsKey)
{
    ASSERT_TRUE(Bake()) << Error;
    EXPECT_EQ(Stats.ReusedArtifacts, 0u);
    StageSource();
    ASSERT_TRUE(Bake()) << Error;
    EXPECT_EQ(Stats.ReusedArtifacts, 1u);
    const auto original = Artifact(TextureCookOutput::Uncompressed);
    WriteBytes(original, {1, 2, 3});
    StageSource();
    ASSERT_TRUE(Bake()) << Error;
    EXPECT_EQ(Stats.ReusedArtifacts, 0u);
    StageSource("project", 90);
    ASSERT_TRUE(Bake()) << Error;
    EXPECT_EQ(Stats.ReusedArtifacts, 0u);
    EXPECT_NE(original, Artifact(TextureCookOutput::Uncompressed, 90));
    Meta[kTextureMipLimitMetaKey] = "2";
    StageSource("project", 90);
    ASSERT_TRUE(Bake()) << Error;
    EXPECT_EQ(Stats.ReusedArtifacts, 0u);
    EXPECT_TRUE(fs::exists(Artifact(TextureCookOutput::Uncompressed, 90)));
}

TEST_F(TexturePackageCookTest, UsesStagedBytesAndMetadataInEachMountNamespace)
{
    StageSource("art");
    Manifest.entries[0].sourcePath = Temp.Path() / "unavailable-authoring-source.tga";
    ASSERT_TRUE(Bake()) << Error;
    EXPECT_TRUE(fs::exists(Artifact(TextureCookOutput::Uncompressed)));
    EXPECT_FALSE(fs::exists(Temp.Path() / "Tex"));
    AssetDatabase::AssetStore_TextJsonl store(nullptr);
    ASSERT_TRUE(store.LoadFromFile(MountRoot / "Assets" / ".assetmanifest", &Error));
    AssetDatabase::AssetRecord record;
    ASSERT_TRUE(store.TryGetAsset(Id, record));
    for (const auto& [key, value] : Meta) EXPECT_EQ(record.kv.at(key), value);
    EXPECT_TRUE(record.kv.contains(kTexturePackagedPortableMetaKey));
    EXPECT_FALSE(fs::exists(MountRoot / "Assets" / "Leaves.tga"));
}

TEST_F(TexturePackageCookTest, FreshStagingReusesValidatedAuthoringCacheWithoutReadingAuthoringSettings)
{
    ASSERT_TRUE(Bake()) << Error;
    const auto stagedArtifact = Artifact(TextureCookOutput::Uncompressed);
    const auto authoring = Temp.Path() / "authoring";
    fs::create_directories(authoring / "Assets");
    fs::create_directories(authoring / "Tex");
    fs::copy_file(stagedArtifact, authoring / "Tex" / stagedArtifact.filename());
    AssetDatabase::AssetStore_TextJsonl store(nullptr);
    ASSERT_TRUE(store.LoadFromFile(MountRoot / "Assets" / ".assetmanifest", &Error));
    AssetDatabase::AssetRecord record;
    ASSERT_TRUE(store.TryGetAsset(Id, record));
    record.kv[kTextureColorSpaceMetaKey] = "linear"; // Live authoring changed after copy.
    ASSERT_TRUE(store.UpsertAsset(record, &Error));
    ASSERT_TRUE(store.SaveToFile(authoring / "Assets" / ".assetmanifest", &Error));
    WriteBytes(authoring / "Assets" / "Leaves.tga", SmallTga(90));
    ASSERT_TRUE(Manager.RegisterSource(MakePackageMount("authoring", authoring / "Assets")));
    fs::remove(stagedArtifact);
    StageSource();
    ASSERT_TRUE(Bake()) << Error;
    EXPECT_EQ(Stats.ReusedArtifacts, 1u);
    EXPECT_TRUE(fs::exists(stagedArtifact));
}

TEST_F(TexturePackageCookTest, CancellationAndInvalidSourceFailWithoutPublishingArtifact)
{
    Cancel->store(true);
    EXPECT_FALSE(Bake()); EXPECT_EQ(Error, kTextureCookCancelledError);
    EXPECT_FALSE(fs::exists(MountRoot / "Tex"));
    Cancel->store(false);
    WriteBytes(MountRoot / "Assets" / "Leaves.tga", {1, 2, 3});
    EXPECT_FALSE(Bake()); EXPECT_NE(Error.find("Texture bake failed"), std::string::npos);
    EXPECT_FALSE(fs::exists(MountRoot / "Tex"));
}

TEST_F(TexturePackageCookTest, RejectsMismatchedStagedIdentityAndSupportsBothTargetQualities)
{
    Manifest.entries[0].outputPath = "Assets/Other.tga";
    EXPECT_FALSE(Bake()); EXPECT_NE(Error.find("path disagrees"), std::string::npos);
    if (!IsTextureCookEncoderAvailable()) return;
    Meta[kTextureCompressionMetaKey] = "bc7";
    StageSource();
    auto bytes = SmallTga();
    for (size_t i = 18; i < bytes.size(); ++i) bytes[i] = static_cast<uint8>((i * 73 + i / 9) % 256);
    WriteBytes(MountRoot / "Assets" / "Leaves.tga", bytes);
    Vector<uint8> previous;
    for (const auto quality : {TextureCookEncodeQuality::Full, TextureCookEncodeQuality::QuickBC7})
    {
        StageSource();
        WriteBytes(MountRoot / "Assets" / "Leaves.tga", bytes);
        ASSERT_TRUE(StagePackagedTextureCooks(Temp.Path(), Manifest, Manager.GetRegistry(),
            quality, /*encodeWorkers=*/nullptr, /*manifestParsePool=*/nullptr, {}, Stats, Error)) << Error;
        const auto name = TextureCookArtifactName(Id, ComputeTextureCookSourceHash(bytes.data(), bytes.size()),
                                                  Inputs(Meta), TextureCookOutput::BC7, quality);
        Vector<uint8> actual;
        ASSERT_TRUE(ReadFileBytesShared(MountRoot / "Tex" / name, actual));
        std::vector<uint8> expected;
        ASSERT_TRUE(CookTexture(bytes.data(), bytes.size(), ".tga", Inputs(Meta), TextureCookOutput::BC7,
                               expected, Error, {}, quality)) << Error;
        EXPECT_EQ(actual, expected); // Target quality drives both bytes and artifact identity.
        if (!previous.empty()) EXPECT_NE(actual, previous);
        previous = actual;
    }
}

// A package texture loads only through its packaged payload. One without payload
// metadata (a package exported before payloads were recorded) raw-decodes: it neither
// adopts a source-hash-keyed artifact sitting in the package nor runs an encoder.
TEST_F(TexturePackageCookTest, PackageTextureWithoutPayloadNeverAdoptsOrCooks)
{
    Meta[kTextureCompressionMetaKey] = "bc7";
    StageSource();
    Mount();
    TextureAsset::SetGpuTranscodeTarget(TextureGpuTranscodeTarget::BC7);
    const auto source = SmallTga();
    for (const auto output : {TextureCookOutput::Uncompressed, TextureCookOutput::BC7})
    {
        if (output == TextureCookOutput::BC7 && !IsTextureCookEncoderAvailable())
            continue;
        std::vector<uint8> cooked;
        ASSERT_TRUE(CookTexture(source.data(), source.size(), ".tga", Inputs(Meta), output, cooked, Error)) << Error;
        WriteBytes(Artifact(output), Vector<uint8>(cooked.begin(), cooked.end()));
    }
    const auto before = fs::file_size(Artifact(TextureCookOutput::Uncompressed));
    auto texture = LoadInWorkerContext();
    EXPECT_EQ(texture->GetFormat(), TextureFormat::RGBA8);
    EXPECT_EQ(texture->GetMipmapLevels(), 1u); // An adopted artifact would carry its mip chain.
    EXPECT_EQ(fs::file_size(Artifact(TextureCookOutput::Uncompressed)), before);
}

// The mirror of the test above, on the one published shape that still cooks.
// An engine package is mounted only by a process built from the engine's own
// tree, which is encoder-equipped and keeps the package's derived cache under
// its own cache root — and is the only thing that ever fills it, since the
// export bakes into the packaged game's tree instead. So a miss there is work
// to do, not damage to report, and doing it must not touch the manifest the
// package is committed with: the settings the cook reads are authored, never
// inferred.
TEST_F(TexturePackageCookTest, EnginePackageMountCooksAMissingArtifactAndLeavesItsManifestAlone)
{
    if (!IsTextureCookEncoderAvailable()) GTEST_SKIP() << "BC encoder unavailable";
    Meta[kTextureCompressionMetaKey] = "bc7";
    StageSource("eztree");

    ResolvedPackage package;
    // Alias as the fixture's other mounts do: metadata lookups resolve through
    // the project source, which the registry only names from this alias, and
    // the policy under test belongs to the descriptor rather than the alias.
    package.Alias = "project";
    package.Priority = 25;
    package.SourceKind = PackageSourceKind::Engine;
    package.RootDir = MountRoot;
    package.AssetsDir = MountRoot / "Assets";
    package.AuthoringRootDir = MountRoot;
    ASSERT_TRUE(Manager.RegisterSource(MakeEnginePackageMount(package, Temp.Path() / "host-cache")));
    const DerivedArtifactPolicy policy = Manager.GetRegistry().GetDerivedArtifactPolicy(Id);
    EXPECT_FALSE(policy.InfersTextureImportSettings);
    // Not fatal: the assertions below are what this test is for, and a revoked
    // grant should be reported as the raw upload and the error a user sees.
    EXPECT_TRUE(policy.CooksOnMiss);

    // The cook reads the manifest's authored settings; nothing inferred them.
    AssetMetadata registered{};
    ASSERT_TRUE(Manager.GetRegistry().TryGetAssetMetadata(Id, registered));
    std::string compression;
    ASSERT_TRUE(Manager.GetRegistry().TryGetMetaValue(registered.Path,
                                                      kTextureCompressionMetaKey, compression))
        << "registered at " << registered.Path.string();
    EXPECT_EQ(compression, "bc7");

    const fs::path manifest = package.AssetsDir / ".assetmanifest";
    const std::string authored = ReadWholeFile(manifest);
    ASSERT_FALSE(authored.empty());
    const Vector<uint8> sourceBytes = SmallTga();
    const fs::path cookedTextures =
        Temp.Path() / "host-cache" / "Packages" / package.Alias / "Tex";
    const fs::path artifact = cookedTextures /
        TextureCookArtifactName(Id,
                                ComputeTextureCookSourceHash(sourceBytes.data(), sourceBytes.size()),
                                Inputs(Meta), TextureCookOutput::BC7);
    ASSERT_FALSE(fs::exists(artifact));

    TextureAsset::SetGpuTranscodeTarget(TextureGpuTranscodeTarget::BC7);
    ASSERT_EQ(TextureAsset::GetGpuTranscodeTarget(), TextureGpuTranscodeTarget::BC7);
    std::vector<std::string> logLines;
    {
        TestLog::ScopedEngineLogCapture capture(&logLines, Logger::LogLevel::Error);
        Logger::Log::Error("probe: the capture is live");
        auto cooked = LoadInWorkerContext();
        EXPECT_EQ(cooked->GetFormat(), TextureFormat::BC7);
        EXPECT_EQ(cooked->GetMipmapLevels(), 4u);
        Logger::Log::Flush();
    }
    EXPECT_EQ(TestLog::CountLinesContaining(logLines, "probe: the capture is live"), 1u);
    EXPECT_EQ(TestLog::CountLinesContaining(logLines, "packaged texture bake missing"), 0u)
        << TestLog::FirstLineContaining(logLines, "packaged texture bake missing");
    EXPECT_TRUE(fs::exists(artifact))
        << "the host cache holds: " << ListDirectory(cookedTextures);
    EXPECT_EQ(ReadWholeFile(manifest), authored) << "the cook authored the committed manifest";
}

TEST_F(TexturePackageCookTest, PrebuiltPortableFallbackAndDirectCpuReadRemainValid)
{
    if (!IsTextureCookEncoderAvailable()) GTEST_SKIP() << "BC encoder unavailable";
    Meta[kTextureCompressionMetaKey] = "bc7";
    StageSource();
    ASSERT_TRUE(Bake()) << Error;
    fs::remove(Artifact(TextureCookOutput::BC7));
    Mount();
    TextureAsset::SetGpuTranscodeTarget(TextureGpuTranscodeTarget::BC7);
    auto portable = LoadInWorkerContext();
    EXPECT_EQ(portable->GetFormat(), TextureFormat::RGBA8);
    EXPECT_EQ(portable->GetMipmapLevels(), 4u);
    EXPECT_FALSE(fs::exists(Artifact(TextureCookOutput::BC7)));
    // CPU readers consume the portable container with full pixel access.
    TextureAsset cpu(GUID::Null(), Artifact(TextureCookOutput::Uncompressed));
    ASSERT_TRUE(cpu.Load());
    EXPECT_EQ(cpu.GetFormat(), TextureFormat::RGBA8);
    EXPECT_EQ(cpu.GetMipmapLevels(), 4u);
    ASSERT_NE(cpu.GetPixelData(), nullptr);
    EXPECT_EQ(cpu.GetPixelData()[0], 80);
}

TEST_F(TexturePackageCookTest, NoopRawAndUnknownUsageHaveStableDefaults)
{
    Meta.clear();
    Meta[kTextureMipsMetaKey] = "0";
    StageSource();
    ASSERT_TRUE(Bake()) << Error;
    EXPECT_EQ(Stats.Artifacts, 0u);
    EXPECT_FALSE(fs::exists(MountRoot / "Tex"));
    Mount();
    auto raw = LoadInWorkerContext();
    EXPECT_EQ(raw->GetFormat(), TextureFormat::RGBA8);
    EXPECT_EQ(raw->GetColorSpace(), TextureColorSpace::SRGB);
    std::string value;
    EXPECT_FALSE(Manager.GetRegistry().TryGetMetaValue(MountRoot / "Assets" / "Leaves.tga", kTextureUsageMetaKey, value));
    EXPECT_FALSE(Manager.GetRegistry().TryGetMetaValue(MountRoot / "Assets" / "Leaves.tga", kTextureColorSpaceMetaKey, value));
}

class TexturePackageCoverageTest : public TexturePackageCookTest,
                                   public testing::WithParamInterface<const char*> {};

TEST_P(TexturePackageCoverageTest, StagedCoverageKeepsExactCutoffAndLoadsBakedMipChain)
{
    Cutout = true;
    const float cutoff = std::nextafter(0.538f, 1.0f);
    Meta[kTextureAlphaCoverageMetaKey] = "1";
    Meta[kTextureAlphaCutoffMetaKey] = TextureAlphaCutoffMetaValue(cutoff);
    StageSource(GetParam());
    AssetDatabase::AssetStore_TextJsonl store(nullptr);
    ASSERT_TRUE(store.LoadFromFile(MountRoot / "Assets" / ".assetmanifest", &Error));
    AssetDatabase::AssetRecord staged;
    ASSERT_TRUE(store.TryGetAsset(Id, staged));
    ASSERT_EQ(staged.kv.at(kTextureAlphaCoverageMetaKey), "1");
    ASSERT_EQ(staged.kv.at(kTextureAlphaCutoffMetaKey), Meta.at(kTextureAlphaCutoffMetaKey));
    const auto inputs = Inputs(staged.kv);
    ASSERT_TRUE(inputs.Settings.PreserveAlphaCoverage);
    EXPECT_EQ(std::bit_cast<uint32>(inputs.Settings.AlphaCoverageCutoff), std::bit_cast<uint32>(cutoff));
    auto ordinaryInputs = inputs;
    ordinaryInputs.Settings.PreserveAlphaCoverage = false;
    const auto source = SmallTga(80, Cutout);
    const auto sourceHash = ComputeTextureCookSourceHash(source.data(), source.size());
    const auto ordinaryName = TextureCookArtifactName(Id, sourceHash, ordinaryInputs, TextureCookOutput::Uncompressed);
    ASSERT_TRUE(Bake()) << Error;
    const auto artifact = Artifact(TextureCookOutput::Uncompressed);
    EXPECT_NE(artifact.filename().string(), ordinaryName);
    Vector<uint8> baked;
    ASSERT_TRUE(ReadFileBytesShared(artifact, baked));
    const auto modified = fs::last_write_time(artifact);
    Mount();
    auto loaded = LoadInWorkerContext();
    ASSERT_EQ(loaded->GetMipmapLevels(), 5u);
    TextureAsset expected(GUID::Null(), "expected.ktx2");
    ASSERT_TRUE(expected.LoadFromData(baked));
    ASSERT_EQ(loaded->GetDataSize(), expected.GetDataSize());
    EXPECT_EQ(std::memcmp(loaded->GetPixelData(), expected.GetPixelData(), expected.GetDataSize()), 0);
    EXPECT_EQ(fs::last_write_time(artifact), modified); // packaged loading never rewrites the bake
    EXPECT_FALSE(fs::exists(MountRoot / "Tex" / ordinaryName));

    // The portable bake really applied coverage: compare against an ordinary
    // mip chain of the same pixels, independently loaded from its container.
    std::vector<uint8> ordinaryBytes;
    ASSERT_TRUE(CookTexture(source.data(), source.size(), ".tga", ordinaryInputs,
                           TextureCookOutput::Uncompressed, ordinaryBytes, Error));
    TextureAsset ordinary(GUID::Null(), "ordinary.ktx2");
    ASSERT_TRUE(ordinary.LoadFromData(Vector<uint8>(ordinaryBytes.begin(), ordinaryBytes.end())));
    ASSERT_EQ(loaded->GetDataSize(), ordinary.GetDataSize());
    EXPECT_NE(std::memcmp(loaded->GetPixelData(), ordinary.GetPixelData(), ordinary.GetDataSize()), 0);
}

INSTANTIATE_TEST_SUITE_P(ProjectAndPackage, TexturePackageCoverageTest,
                        testing::Values("project", "art"));

TEST_F(TexturePackageCookTest, CoveredBc7AndPortableVariantsLoadWithoutRuntimeEncoding)
{
    if (!IsTextureCookEncoderAvailable()) GTEST_SKIP() << "BC encoder unavailable";
    Cutout = true;
    Meta[kTextureCompressionMetaKey] = "bc7";
    Meta[kTextureAlphaCoverageMetaKey] = "1";
    Meta[kTextureAlphaCutoffMetaKey] = ".538";
    StageSource("art");
    ASSERT_TRUE(Bake()) << Error;
    ASSERT_EQ(Stats.Artifacts, 2u);
    Mount();
    for (auto target : {TextureGpuTranscodeTarget::BC7, TextureGpuTranscodeTarget::RGBA32})
    {
        TextureAsset::SetGpuTranscodeTarget(target);
        auto texture = LoadInWorkerContext();
        EXPECT_EQ(texture->GetFormat(), target == TextureGpuTranscodeTarget::BC7
            ? TextureFormat::BC7 : TextureFormat::RGBA8);
        EXPECT_EQ(texture->GetMipmapLevels(), 5u);
    }
    const auto compressed = Artifact(TextureCookOutput::BC7);
    const auto portable = Artifact(TextureCookOutput::Uncompressed);
    fs::remove(compressed);
    fs::remove(portable);
    TextureAsset::SetGpuTranscodeTarget(TextureGpuTranscodeTarget::BC7);
    AssetManager::ScopedThreadAssetManager context(&Manager);
    TextureAsset damaged(Id, MountRoot / "Assets" / "Leaves.tga");
    EXPECT_FALSE(damaged.Load());
    EXPECT_EQ(damaged.GetState(), AssetState::Failed);
    EXPECT_FALSE(fs::exists(compressed));
    EXPECT_FALSE(fs::exists(portable));
}

// A package exported where a BC encoder exists ships each texture twice, and
// its manifest records both payloads: the BC artifact and the portable RGBA8
// one. The Player picks between them by the transcode target the texture
// service publishes from the device at init, so on a Mac the Metal device's BC
// support decides whether the game reads and uploads the BC7 artifact or four
// times the bytes of RGBA8.
TEST_F(TexturePackageCookTest, MetalDeviceAdoptsTheShippedBc7ArtifactOverThePortableOne)
{
#if !defined(GE_HAVE_KTX)
    GTEST_SKIP() << "libktx unavailable";
#else
    auto device = CreateHeadlessMetalDevice();
    if (!device)
        GTEST_SKIP() << "Metal backend unavailable";
    Engine::Renderer::MaterialRegistry materials;
    Engine::Renderer::TextureService textures;
    const TextureServiceShutdown shutdown{textures, *device};
    ASSERT_TRUE(textures.Initialize(device.get(), materials,
                                    Rendering::RendererProfile::FromCapabilities(device->GetCapabilities())));
    EXPECT_EQ(TextureAsset::GetGpuTranscodeTarget(), TextureGpuTranscodeTarget::BC7);

    // This host bakes only the portable payload, so the BC7 one is written by
    // hand and recorded in the staged manifest, as a Windows export records it.
    Meta[kTextureCompressionMetaKey] = "bc7";
    StageSource();
    ASSERT_TRUE(Bake()) << Error;
    ASSERT_TRUE(fs::exists(Artifact(TextureCookOutput::Uncompressed))) << ListDirectory(MountRoot / "Tex");
    const Vector<uint8> bc7 = HandWrittenBc7Ktx2();
    ASSERT_FALSE(bc7.empty());
    WriteBytes(Artifact(TextureCookOutput::BC7), bc7);
    const fs::path manifest = MountRoot / "Assets" / ".assetmanifest";
    AssetDatabase::AssetStore_TextJsonl store(nullptr);
    ASSERT_TRUE(store.LoadFromFile(manifest, &Error)) << Error;
    ASSERT_TRUE(store.SetKeyValue(Id, kTexturePackagedCompressedMetaKey,
                                  Artifact(TextureCookOutput::BC7).filename().string(), &Error)) << Error;
    ASSERT_TRUE(store.SaveToFile(manifest, &Error)) << Error;
    Mount();

    auto texture = LoadInWorkerContext();
    EXPECT_EQ(texture->GetFormat(), TextureFormat::BC7);
    ASSERT_EQ(texture->GetMipmapLevels(), 4u);
    ASSERT_EQ(texture->GetMipChain()[0].Size, 64u);
    EXPECT_EQ(texture->GetPixelData()[texture->GetMipChain()[0].Offset], 1u);
    EXPECT_EQ(texture->GetPixelData()[texture->GetMipChain()[3].Offset], 4u);
#endif
}

TEST_F(TexturePackageCookTest, InvalidStagedCoverageFailsBakeAndLoadWithoutRawFallback)
{
    Meta[kTextureAlphaCoverageMetaKey] = "1";
    Meta[kTextureAlphaCutoffMetaKey] = "nan";
    StageSource();
    EXPECT_FALSE(Bake());
    EXPECT_NE(Error.find("Invalid texture import policy"), std::string::npos);
    EXPECT_FALSE(fs::exists(MountRoot / "Tex"));
    Mount();
    AssetManager::ScopedThreadAssetManager context(&Manager);
    TextureAsset file(Id, MountRoot / "Assets" / "Leaves.tga");
    EXPECT_FALSE(file.Load());
    TextureAsset memory(Id, MountRoot / "Assets" / "Leaves.tga");
    EXPECT_FALSE(memory.LoadFromData(SmallTga()));
    EXPECT_FALSE(fs::exists(MountRoot / "Tex"));
}

TEST_F(TexturePackageCookTest, PackagedArtifactPathMatchesOwningMountCacheRoot)
{
    StageSource("art");
    ASSERT_TRUE(Bake()) << Error;
    auto mount = MakePackageMount("project", MountRoot / "Assets");
    ASSERT_TRUE(Manager.RegisterSource(mount));
    EXPECT_EQ(Manager.GetRegistry().TryGetCacheRoot(Id), MountRoot);
    EXPECT_EQ(Manager.GetRegistry().TryGetCacheRoot(MountRoot / "Assets" / "Leaves.tga"), MountRoot);
    auto texture = LoadInWorkerContext();
    EXPECT_EQ(texture->GetMipmapLevels(), 4u);
}

TEST_F(TexturePackageCookTest, DisabledCookingDoesNotRedirectExplicitCacheRoot)
{
    auto mount = MakePackageMount("project", MountRoot / "Assets");
    const auto derivedRoot = Temp.Path() / "authoring-cache";
    mount.CacheRoot = derivedRoot / "AssetDatabase";
    ASSERT_TRUE(Manager.RegisterSource(mount));
    EXPECT_FALSE(Manager.GetRegistry().GetDerivedArtifactPolicy(Id).CooksOnMiss);
    EXPECT_EQ(Manager.GetRegistry().TryGetCacheRoot(Id), derivedRoot);
    EXPECT_EQ(Manager.GetRegistry().TryGetCacheRoot(MountRoot / "Assets" / "Leaves.tga"), derivedRoot);
}

TEST_F(TexturePackageCookTest, HdrBytesUnderLdrExtensionUseSameClassificationInFileMemoryAndBake)
{
    const std::string header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 1\n";
    Vector<uint8> source(header.begin(), header.end());
    source.insert(source.end(), {128, 64, 32, 131});
    const auto path = MountRoot / "Assets" / "Leaves.tga";
    WriteBytes(path, source);
    Mount();
    AssetManager::ScopedThreadAssetManager context(&Manager);
    TextureAsset file(Id, path), memory(Id, path);
    ASSERT_TRUE(file.Load());
    ASSERT_TRUE(memory.LoadFromData(source));
    EXPECT_EQ(file.GetFormat(), TextureFormat::RGB8);
    EXPECT_EQ(memory.GetFormat(), file.GetFormat());
    EXPECT_EQ(memory.GetColorSpace(), file.GetColorSpace());
    ASSERT_EQ(memory.GetDataSize(), file.GetDataSize());
    EXPECT_EQ(std::memcmp(memory.GetPixelData(), file.GetPixelData(), file.GetDataSize()), 0);
    ASSERT_TRUE(Bake()) << Error;
    ASSERT_TRUE(Manager.BeginUnregisterSource("project"));
    Mount(); // A Player mounts the finished manifest after export.
    TextureAsset baked(Id, path);
    ASSERT_TRUE(baked.Load());
    EXPECT_EQ(baked.GetFormat(), TextureFormat::RGBA8);
    EXPECT_EQ(baked.GetColorSpace(), file.GetColorSpace());
    EXPECT_EQ(std::memcmp(baked.GetPixelData(), file.GetPixelData(), 3), 0);
}

TEST_F(TexturePackageCookTest, MissingOrCorruptHdrBakePreservesFloatRangeForFileAndMemoryLoads)
{
    const std::string header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 1\n";
    Vector<uint8> source(header.begin(), header.end());
    source.insert(source.end(), {128, 64, 32, 131}); // RGB = 4, 2, 1
    const auto path = MountRoot / "Assets" / "Light.hdr";
    WriteBytes(path, source);
    AssetDatabase::AssetRecord record;
    record.guid = Id; record.type = AssetType::Texture; record.path = "Light.hdr";
    AssetDatabase::AssetStore_TextJsonl store(nullptr);
    ASSERT_TRUE(store.UpsertAsset(record, &Error));
    ASSERT_TRUE(store.SaveToFile(MountRoot / "Assets" / ".assetmanifest", &Error));
    Mount();
    TextureAsset::SetGpuTranscodeTarget(TextureGpuTranscodeTarget::BC7);
    AssetManager::ScopedThreadAssetManager context(&Manager);
    TextureCookInputs inputs;
    ASSERT_TRUE(ResolveTextureCookInputs(".hdr", [](const char*, std::string&) { return false; }, inputs, Error));
    const auto sourceHash = ComputeTextureCookSourceHash(source.data(), source.size());
    const auto compressed = MountRoot / "Tex" / TextureCookArtifactName(Id, sourceHash, inputs, TextureCookOutput::BC6H);
    const auto portable = MountRoot / "Tex" / TextureCookArtifactName(Id, sourceHash, inputs, TextureCookOutput::Uncompressed);
    for (bool corrupt : {false, true})
    {
        SCOPED_TRACE(corrupt);
        if (corrupt)
        {
            WriteBytes(compressed, {1, 2, 3});
            WriteBytes(portable, {1, 2, 3});
        }
        for (bool fromMemory : {false, true})
        {
            SCOPED_TRACE(fromMemory);
            TextureAsset texture(Id, path);
            ASSERT_TRUE(fromMemory ? texture.LoadFromData(source) : texture.Load());
            EXPECT_EQ(texture.GetFormat(), TextureFormat::RGB32F);
            EXPECT_EQ(texture.GetColorSpace(), TextureColorSpace::Linear);
            ASSERT_EQ(texture.GetDataSize(), 3u * sizeof(float));
            float pixels[3];
            std::memcpy(pixels, texture.GetPixelData(), sizeof(pixels));
            EXPECT_FLOAT_EQ(pixels[0], 4.0f);
            EXPECT_FLOAT_EQ(pixels[1], 2.0f);
            EXPECT_FLOAT_EQ(pixels[2], 1.0f);
            if (corrupt)
            {
                EXPECT_EQ(fs::file_size(compressed), 3u);
                EXPECT_EQ(fs::file_size(portable), 3u);
            }
            else
                EXPECT_FALSE(fs::exists(MountRoot / "Tex"));
        }
    }
}

} // namespace

TEST_F(TexturePackageCookTest, SourceFreePackageKeepsImplicitExplicitAndReferrerPaths)
{
    StageSource("art");
    ASSERT_TRUE(Bake()) << Error;
    ASSERT_FALSE(fs::exists(MountRoot / "Assets" / "Leaves.tga"));
    ASSERT_TRUE(Manager.RegisterSource(MakePackageMount("art", MountRoot / "Assets")));
    EXPECT_EQ(Manager.ResolveAssetGuid("Leaves.tga"), Id);
    EXPECT_EQ(Manager.ResolveAssetGuid("art:Leaves.tga"), Id);
    // A project file has higher implicit priority; references authored in art still prefer art.
    const auto projectRoot = Temp.Path() / "Project" / "Assets";
    WriteBytes(projectRoot / "Leaves.tga", SmallTga(90));
    AssetDatabase::AssetStore_TextJsonl projectStore(nullptr);
    AssetDatabase::AssetRecord projectRecord;
    projectRecord.guid = GUID::Derive(Id, "project");
    projectRecord.path = "Leaves.tga";
    projectRecord.type = AssetType::Texture;
    ASSERT_TRUE(projectStore.UpsertAsset(projectRecord, &Error));
    ASSERT_TRUE(projectStore.SaveToFile(projectRoot / ".assetmanifest", &Error));
    ASSERT_TRUE(Manager.RegisterSource(MakePackageMount("project", projectRoot)));
    EXPECT_EQ(Manager.ResolveAssetGuid("Leaves.tga"), projectRecord.guid);
    EXPECT_EQ(Manager.GetRegistry().GetAssetGUID(
        Manager.ResolveAssetPathFromReference("Leaves.tga", MountRoot / "Assets" / "Cold.material")), Id);
    EXPECT_EQ(Manager.GetRegistry().GetAssetGUID(
        Manager.ResolveAssetPathPreferringSource("Leaves.tga", "art", AssetPathKind::AnyEntry)), Id);
    auto future = Manager.LoadAssetAsync(Id);
    ASSERT_EQ(future.wait_for(std::chrono::seconds(10)), std::future_status::ready);
    const auto texture = std::dynamic_pointer_cast<TextureAsset>(future.get());
    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->GetMipmapLevels(), 4u);
}

TEST_F(TexturePackageCookTest, SourceFreeCompressionWithoutMipsStillShipsPortablePixels)
{
    if (!IsTextureCookEncoderAvailable()) GTEST_SKIP() << "BC encoder unavailable";
    Meta[kTextureCompressionMetaKey] = "bc7";
    Meta[kTextureMipsMetaKey] = "0";
    StageSource();
    ASSERT_TRUE(Bake()) << Error;
    EXPECT_EQ(Stats.Artifacts, 2u);
    EXPECT_FALSE(fs::exists(MountRoot / "Assets" / "Leaves.tga"));
    Mount();
    TextureAsset::SetGpuTranscodeTarget(TextureGpuTranscodeTarget::RGBA32);
    auto texture = LoadInWorkerContext();
    EXPECT_EQ(texture->GetFormat(), TextureFormat::RGBA8);
    EXPECT_EQ(texture->GetMipmapLevels(), 1u);
    ASSERT_NE(texture->GetPixelData(), nullptr);
    EXPECT_EQ(texture->GetPixelData()[0], 80);
}

TEST_F(TexturePackageCookTest, CorruptCompressedPayloadUsesPortableWithoutSourceOrRecook)
{
    if (!IsTextureCookEncoderAvailable()) GTEST_SKIP() << "BC encoder unavailable";
    Meta[kTextureCompressionMetaKey] = "bc7";
    StageSource();
    ASSERT_TRUE(Bake()) << Error;
    const auto compressed = Artifact(TextureCookOutput::BC7);
    WriteBytes(compressed, {1, 2, 3});
    Mount();
    TextureAsset::SetGpuTranscodeTarget(TextureGpuTranscodeTarget::BC7);
    auto texture = LoadInWorkerContext();
    EXPECT_EQ(texture->GetFormat(), TextureFormat::RGBA8);
    EXPECT_EQ(texture->GetMipmapLevels(), 4u);
    EXPECT_EQ(fs::file_size(compressed), 3u);
    EXPECT_FALSE(fs::exists(MountRoot / "Assets" / "Leaves.tga"));
}

TEST_F(TexturePackageCookTest, PackagedPayloadFilenameCannotEscapeTheMount)
{
    ASSERT_TRUE(Bake()) << Error;
    AssetDatabase::AssetStore_TextJsonl store(nullptr);
    const auto manifest = MountRoot / "Assets" / ".assetmanifest";
    ASSERT_TRUE(store.LoadFromFile(manifest, &Error));
    ASSERT_TRUE(store.SetKeyValue(Id, kTexturePackagedPortableMetaKey, "../outside.ktx2", &Error));
    ASSERT_TRUE(store.SaveToFile(manifest, &Error));
    Mount();
    AssetManager::ScopedThreadAssetManager context(&Manager);
    TextureAsset texture(Id, MountRoot / "Assets" / "Leaves.tga");
    EXPECT_FALSE(texture.Load());
    EXPECT_EQ(texture.GetState(), AssetState::Failed);
}
