#include "Assets/AssetManager.h"
#include "Assets/TextureCook.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "EngineLogCapture.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/TextureService.h"
#include "Rendering/Core/CommandList.h"
#include "StagedTestPaths.h"
#include "TestDeviceHelper.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <thread>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace GameEngine::Engine::Renderer
{
// Seed the existing worker-completion boundary without depending on thread
// timing. All admission, GPU publication and pending-bind cleanup run normally.
struct TextureCoverageUploadTestAccess
{
    static void Queue(TextureService& service, const GUID& texture, const GUID& material,
                      const SharedPtr<Asset>& decoded, StringId slot = StringId{})
    {
        std::lock_guard lock(service.m_PendingTexturesMutex);
        const auto epoch = service.m_TextureEpochByGuid[texture];
        service.m_PendingGpuUploads.push_back({texture, decoded, epoch});
        service.m_PendingTextureBinds.push_back({material, slot, texture});
        service.m_TextureLoadHandles.try_emplace(texture);
    }
    static bool HasLoadHandle(TextureService& service, const GUID& texture)
    {
        std::lock_guard lock(service.m_PendingTexturesMutex);
        return service.m_TextureLoadHandles.contains(texture);
    }
    static bool UploadJobsIdle(TextureService& service) { return service.m_UploadJobCounter.IsZero(); }
    // What an Evict does to a load's epoch, without the rest of the Evict: a queued result stays.
    static void SupersedeLoadEpoch(TextureService& service, const GUID& texture)
    {
        std::lock_guard lock(service.m_PendingTexturesMutex);
        ++service.m_TextureEpochByGuid[texture];
    }
    static bool Uploaded(TextureService& service, const GUID& texture)
    {
        return service.m_TextureGPUCache.Contains(texture);
    }
};
} // namespace GameEngine::Engine::Renderer

namespace
{
namespace fs = std::filesystem;
class TextureCoverageGPU : public testing::Test
{
  protected:
    inline static fs::path s_Workspace, s_PreviousDirectory;
    std::unique_ptr<IDevice> m_Device;
    Engine::Renderer::MaterialRegistry m_Materials;
    Engine::Renderer::TextureService m_Textures;
    PipelineHandle m_Pipeline;
    SamplerHandle m_Sampler;
    DescriptorSetLayoutDesc m_Layout;

    static void SetUpTestSuite()
    {
        s_PreviousDirectory = fs::current_path();
        s_Workspace = fs::temp_directory_path() / ("ge-coverage-gpu-" + GUID::Generate().ToString());
        fs::create_directories(s_Workspace / "Assets");
        auto& engine = EngineCore::GetInstance();
        ScriptsConfig scripts;
        scripts.disableClr = true;
        scripts.enableHotReload = false;
        scripts.enableAsyncHotReload = false;
        scripts.enableAutoProjectGeneration = false;
        engine.SetScriptsConfig(scripts);
        ApplicationConfig config;
        config.WorkspaceDirectory = s_Workspace.string();
        config.EnableEditor = false;
        ASSERT_TRUE(engine.Initialize(config));
    }
    static void TearDownTestSuite()
    {
        EngineCore::GetInstance().Shutdown();
        fs::current_path(s_PreviousDirectory);
        std::error_code error;
        fs::remove_all(s_Workspace, error);
    }
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "Vulkan m_Device unavailable";
        m_Materials.Initialize(m_Device.get());
        ASSERT_TRUE(m_Textures.Initialize(m_Device.get(), m_Materials,
                                          RendererProfile::FromCapabilities(m_Device->GetCapabilities())));
        m_Sampler = m_Device->CreateSampler(SamplerDesc::MaterialLinearRepeat("Coverage.Trilinear"));
        ASSERT_TRUE(m_Sampler.IsValid());
        DescriptorBinding binding{};
        binding.binding = 0;
        binding.type = DescriptorType::CombinedImageSampler;
        binding.count = 1;
        binding.shaderStages = kShaderStageFragment;
        m_Layout.bindings.push_back(binding);
        PipelineDesc desc{};
        desc.type = PipelineType::Graphics;
        desc.vertexShader = Read(TestPaths::StagedRoot() / "Shaders/texture_coverage_probe.vert.spv");
        desc.pixelShader = Read(TestPaths::StagedRoot() / "Shaders/texture_coverage_probe.frag.spv");
        ASSERT_FALSE(desc.vertexShader.empty());
        ASSERT_FALSE(desc.pixelShader.empty());
        desc.topology = PrimitiveTopology::TriangleList;
        desc.AddDynamicState(DynamicState::Viewport);
        desc.AddDynamicState(DynamicState::Scissor);
        desc.EnableDepthTest(false);
        desc.SetCullingMode(CullModeFlagBits::None);
        desc.descriptorSetLayouts = {m_Layout};
        desc.pushConstantSize = 24;
        desc.pushConstantStagesMask = kShaderStageFragment;
        desc.colorAttachmentFormats = {uint32_t(Rendering::TextureFormat::R32G32B32A32_FLOAT)};
        m_Pipeline = m_Device->CreatePipeline(desc);
        ASSERT_TRUE(m_Pipeline.IsValid());
    }
    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        if (m_Pipeline.IsValid())
            m_Device->DestroyPipeline(m_Pipeline);
        if (m_Sampler.IsValid())
            m_Device->DestroySampler(m_Sampler);
        m_Textures.Shutdown();
        m_Materials.Shutdown();
        m_Device.reset();
    }
    static std::vector<uint8> Read(const fs::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::vector<uint8>(std::istreambuf_iterator<char>(stream), {});
    }
    // A square TGA of `size` texels a side (64 unless a test needs a slow decode).
    fs::path Image(bool grayAlpha = false, int size = 64)
    {
        const auto path = s_Workspace / "Assets" / (GUID::Generate().ToString() + ".tga");
        std::vector<uint8> bytes(18, 0);
        bytes[2] = grayAlpha ? 3 : 2;
        bytes[12] = uint8(size & 0xff);
        bytes[13] = uint8(size >> 8);
        bytes[14] = uint8(size & 0xff);
        bytes[15] = uint8(size >> 8);
        bytes[16] = grayAlpha ? 16 : 32;
        bytes[17] = 0x28;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
            {
                // Repeated small, irregular leaf islands produce meaningful erosion
                // at intermediate levels while retaining tied coarse tail levels.
                const int dx = x % 16 - 7, dy = y % 16 - 7;
                const uint8 alpha = dx * dx + 2 * dy * dy < 37 ? 255 : 0;
                if (grayAlpha)
                    bytes.insert(bytes.end(), {uint8(91 + x + y), alpha});
                else
                    bytes.insert(bytes.end(), {uint8(51 + x), uint8(77 + y), uint8(91 + x + y), alpha});
            }
        std::ofstream stream(path, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        stream.close();
        return path;
    }
    void Set(const fs::path& path, const char* key, const char* value)
    {
        ASSERT_TRUE(EngineCore::GetInstance().GetAssetManager().GetRegistry().SetMetaValue(path, key, value));
    }
    GUID Register(const fs::path& path, bool coverage)
    {
        Set(path, kTextureCompressionMetaKey, "none");
        Set(path, kTextureColorSpaceMetaKey, "srgb");
        Set(path, kTextureAlphaCoverageMetaKey, coverage ? "1" : "0");
        Set(path, kTextureAlphaCutoffMetaKey, ".538");
        return EngineCore::GetInstance().GetAssetManager().GetRegistry().GetAssetGUID(path);
    }
    std::vector<float> Render(TextureHandle texture, uint32 width, uint32 height, float lod,
                              uint32 mode = 0, float phase = 0.0f)
    {
        TextureDesc desc{};
        desc.width = width;
        desc.height = height;
        desc.format = uint32_t(Rendering::TextureFormat::R32G32B32A32_FLOAT);
        desc.usage = uint32_t(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
        const auto target = m_Device->CreateTexture(desc);
        const auto readback = m_Device->CreateReadbackBuffer(size_t(width) * height * 16);
        const auto set = m_Device->CreateDescriptorSet({m_Layout, "Coverage.Image", true});
        EXPECT_TRUE(target.IsValid());
        EXPECT_TRUE(readback.IsValid());
        EXPECT_TRUE(set.IsValid());
        m_Device->UpdateCombinedImageSamplerBinding(set, 0, texture, m_Sampler);
        struct Push
        {
            float lod, cutoff, phaseX, phaseY;
            uint32 mode, pad0;
        };
        const Push push{lod, .538f, phase, phase * .37f, mode, 0u};
        auto commands = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        commands->Begin();
        commands->Barrier(ResourceBarrier::CreateTextureBarrier(target, ResourceState::Undefined, ResourceState::RenderTarget));
        RenderPassDesc pass{};
        pass.colorTargetCount = 1;
        pass.colorTargets[0] = target;
        pass.clearColor[0] = true;
        pass.colorStoreOp[0] = RenderPassDesc::StoreOp::Store;
        std::fill_n(pass.clearColorValue[0], 4, 0.0f);
        commands->BeginRenderPass(pass);
        commands->SetViewport(0, 0, static_cast<float>(width), static_cast<float>(height));
        commands->SetScissor(0, 0, width, height);
        commands->SetPipeline(m_Pipeline);
        commands->BindDescriptorSet(0, set, m_Pipeline);
        commands->SetPushConstants(push);
        commands->Draw(6);
        commands->EndRenderPass();
        commands->Barrier(ResourceBarrier::CreateTextureBarrier(target, ResourceState::RenderTarget, ResourceState::CopySource));
        commands->CopyTextureToBuffer(target, readback, width, height);
        commands->End();
        m_Device->ExecuteCommandLists({commands.get()});
        m_Device->WaitForIdle();
        const auto* mapped = static_cast<const float*>(m_Device->MapBuffer(readback));
        EXPECT_NE(mapped, nullptr);
        std::vector<float> pixels;
        if (mapped)
            pixels.assign(mapped, mapped + size_t(width) * height * 4);
        m_Device->UnmapBuffer(readback);
        m_Device->DestroyBuffer(readback);
        m_Device->DestroyTexture(target);
        m_Device->DestroyDescriptorSet(set);
        return pixels;
    }
    static double Coverage(const std::vector<float>& pixels)
    {
        size_t passing = 0;
        for (size_t i = 3; i < pixels.size(); i += 4)
            passing += pixels[i] > .5f;
        return double(passing) / (pixels.size() / 4);
    }
    static float LinearByte(uint8 value)
    {
        const float srgb = value / 255.0f;
        return srgb <= 0.04045f ? srgb / 12.92f : std::pow((srgb + 0.055f) / 1.055f, 2.4f);
    }
};
} // namespace

#if defined(GE_HAVE_KTX)
TEST_F(TextureCoverageGPU, RawUploadAndWorkerCookMatchEveryTexelThroughOneByOne)
{
    for (bool grayAlpha : {false, true})
    {
        SCOPED_TRACE(grayAlpha);
        const auto path = Image(grayAlpha);
        const GUID guid = Register(path, true);
        ASSERT_FALSE(guid.IsNull());
        TextureAsset decoded(guid, path);
        ASSERT_TRUE(decoded.Load());
        ASSERT_EQ(decoded.GetChannels(), grayAlpha ? 2u : 4u);
        const auto raw = m_Textures.GetOrUpload(guid);
        ASSERT_TRUE(raw.IsValid());
        std::vector<std::vector<float>> rawLevels;
        for (uint32 level = 0, extent = 64; level < 7; ++level, extent = std::max(extent / 2, 1u))
            rawLevels.push_back(Render(raw, extent, extent, float(level)));
        auto& assets = EngineCore::GetInstance().GetAssetManager();
        {
            AssetManager::ScopedThreadAssetManager context(&assets);
            TextureAsset cooked(guid, path);
            ASSERT_TRUE(cooked.Load());
            ASSERT_EQ(cooked.GetMipChain().size(), 7u);
            for (uint32 level = 0; level < 7; ++level)
            {
                const auto& mip = cooked.GetMipChain()[level];
                ASSERT_EQ(rawLevels[level].size(), size_t(mip.Width) * mip.Height * 4);
                for (size_t i = 0; i < rawLevels[level].size(); ++i)
                {
                    const auto byte = cooked.GetPixelData()[mip.Offset + i];
                    if (i % 4 == 3)
                        EXPECT_NEAR(rawLevels[level][i], byte / 255.0f, 1e-6f);
                    else
                    {
                        // Hardware sRGB decode is approximate. Bound it by the
                        // neighbouring source codes; raw/adopted GPU arrays below
                        // must still match exactly, including every RGB value.
                        const auto lower = uint8(std::max(int(byte) - 1, 0));
                        const auto upper = uint8(std::min(int(byte) + 1, 255));
                        EXPECT_GE(rawLevels[level][i], LinearByte(lower) - 1e-6f);
                        EXPECT_LE(rawLevels[level][i], LinearByte(upper) + 1e-6f);
                    }
                }
            }
        }
        m_Textures.Evict(guid);
        const auto adopted = m_Textures.GetOrUpload(guid);
        ASSERT_TRUE(adopted.IsValid());
        for (uint32 level = 0, extent = 64; level < 7; ++level, extent = std::max(extent / 2, 1u))
            EXPECT_EQ(Render(adopted, extent, extent, float(level)), rawLevels[level]);
    }
}

#endif

// Assigning a texture that is not loaded (the editor's texture assign, UpdateMaterialTextures)
// never decodes it or builds its mips on the calling thread: the call returns with the binding
// pending, an upload job prepares the texture on a worker, and FlushPendingUploads submits and
// binds it. The upload's log line names the thread that prepared it.
TEST_F(TextureCoverageGPU, AssigningATextureThatIsNotLoadedBindsItFromTheWorkerLoad)
{
    std::vector<std::string> lines;
    TestLog::ScopedEngineLogCapture capture(&lines);
    const fs::path path = Image();
    const GUID texture = Register(path, false);
    MaterialDocument doc{};
    doc.schemaVersion = 3;
    doc.materialName = "TextureAssign";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    const GUID material = GUID::Generate();
    ASSERT_NE(m_Materials.Register(material, doc), nullptr);

    doc.textures["albedoMap"] = texture.ToString();
    m_Textures.UpdateMaterialTextures(material, doc);
    EXPECT_FALSE(m_Textures.IsMaterialTextureBindingComplete(material))
        << "the assign loaded and uploaded the texture on the calling thread";

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!m_Textures.IsMaterialTextureBindingComplete(material) && std::chrono::steady_clock::now() < deadline)
    {
        EngineCore::GetInstance().GetAssetManager().Update();
        m_Textures.FlushPendingUploads();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(m_Textures.IsMaterialTextureBindingComplete(material)) << "the worker load never bound";
    const Engine::Renderer::Material* bound = m_Materials.Find(material);
    ASSERT_NE(bound, nullptr);
    EXPECT_TRUE(bound->GetTexture(HashStringId("albedoMap")).IsValid());

    Logger::Log::Flush();
    const std::string name = path.filename().string();
    const auto upload = std::find_if(lines.begin(), lines.end(), [&](const std::string& line)
                                     { return line.find(name) != std::string::npos && line.find("[ModelLoad]") != std::string::npos; });
    ASSERT_NE(upload, lines.end()) << "no upload line for the assigned texture";
    EXPECT_NE(upload->find("on the worker"), std::string::npos) << *upload;
}

// Assigning a texture the AssetManager already holds never hands its shared instance to a worker:
// a reload swaps that instance's payload in place on the main thread. The upload is prepared at
// the drain, on the render thread (a cooked container costs milliseconds there).
TEST_F(TextureCoverageGPU, AssigningATextureAlreadyLoadedPreparesItOnTheRenderThread)
{
    std::vector<std::string> lines;
    TestLog::ScopedEngineLogCapture capture(&lines);
    const fs::path path = Image();
    const GUID texture = Register(path, false);
    ASSERT_NE(EngineCore::GetInstance().GetAssetManager().LoadAssetAsync(texture, AssetLoadPriority::High).get(),
              nullptr);
    MaterialDocument doc{};
    doc.schemaVersion = 3;
    doc.materialName = "TextureAssignResident";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    const GUID material = GUID::Generate();
    ASSERT_NE(m_Materials.Register(material, doc), nullptr);

    doc.textures["albedoMap"] = texture.ToString();
    m_Textures.UpdateMaterialTextures(material, doc);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!m_Textures.IsMaterialTextureBindingComplete(material) && std::chrono::steady_clock::now() < deadline)
    {
        m_Textures.FlushPendingUploads();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(m_Textures.IsMaterialTextureBindingComplete(material));

    Logger::Log::Flush();
    const std::string name = path.filename().string();
    const auto upload = std::find_if(lines.begin(), lines.end(), [&](const std::string& line)
                                     { return line.find(name) != std::string::npos && line.find("[ModelLoad]") != std::string::npos; });
    ASSERT_NE(upload, lines.end()) << "no upload line for the assigned texture";
    EXPECT_NE(upload->find("on the render thread"), std::string::npos) << *upload;
}

// A load result whose epoch an Evict has since moved is never published, whatever route queued it.
TEST_F(TextureCoverageGPU, ALoadResultFromASupersededEpochIsNotPublished)
{
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
    const auto path = Image();
    const GUID texture = Register(path, false);
    auto decoded = MakeShared<TextureAsset>(texture, path);
    ASSERT_TRUE(decoded->Load());
    Access::Queue(m_Textures, texture, GUID::Generate(), decoded);
    Access::SupersedeLoadEpoch(m_Textures, texture);
    m_Textures.FlushPendingUploads();
    EXPECT_FALSE(Access::Uploaded(m_Textures, texture)) << "a superseded load was published";
}

// Evicting a texture while its upload job runs drops the job's result: it is a pre-eviction
// load, so it is never published, and the slot binds the texture assigned after it. The evicted
// texture is large enough that its decode and mips are still running when Evict lands.
TEST_F(TextureCoverageGPU, EvictingATextureWhileItsUploadJobRunsDropsTheJobResult)
{
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
    const StringId slot = HashStringId("albedoMap");
    MaterialDocument doc{};
    doc.schemaVersion = 3;
    doc.materialName = "TextureEvictWhileLoading";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    const GUID material = GUID::Generate();
    ASSERT_NE(m_Materials.Register(material, doc), nullptr);
    const Engine::Renderer::Material* bound = m_Materials.Find(material);
    ASSERT_NE(bound, nullptr);

    for (int round = 0; round < 8; ++round)
    {
        SCOPED_TRACE(round);
        const GUID evicted = Register(Image(false, 2048), false);
        const GUID kept = Register(Image(), false);
        doc.textures["albedoMap"] = evicted.ToString();
        m_Textures.UpdateMaterialTextures(material, doc);
        doc.textures["albedoMap"] = kept.ToString();
        m_Textures.UpdateMaterialTextures(material, doc);
        m_Textures.Evict(evicted);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while ((!m_Textures.IsMaterialTextureBindingComplete(material) || !Access::UploadJobsIdle(m_Textures)) &&
               std::chrono::steady_clock::now() < deadline)
        {
            m_Textures.FlushPendingUploads();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        m_Textures.FlushPendingUploads();
        ASSERT_TRUE(Access::UploadJobsIdle(m_Textures)) << "an upload job never finished";
        EXPECT_FALSE(Access::Uploaded(m_Textures, evicted)) << "the evicted texture's job result was published";
        EXPECT_EQ(bound->GetTexture(slot), m_Textures.GetOrUpload(kept));
    }
}

// Shutdown with upload jobs in flight waits for them, so none outlives the service.
TEST_F(TextureCoverageGPU, ShutdownWaitsForUploadJobsStillRunning)
{
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
    MaterialDocument doc{};
    doc.schemaVersion = 3;
    doc.materialName = "TextureShutdownWhileLoading";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    const GUID material = GUID::Generate();
    ASSERT_NE(m_Materials.Register(material, doc), nullptr);
    // Large enough that the jobs are still decoding when Shutdown runs.
    for (int i = 0; i < 4; ++i)
    {
        doc.textures["albedoMap"] = Register(Image(false, 2048), false).ToString();
        m_Textures.UpdateMaterialTextures(material, doc);
    }
    m_Textures.Shutdown();
    EXPECT_TRUE(Access::UploadJobsIdle(m_Textures)) << "Shutdown returned with an upload job still running";
}

// A second assign to a slot supersedes a first whose load has not landed yet: the first texture's
// upload (here a landed job result queued for the drain) must not bind over the second.
TEST_F(TextureCoverageGPU, ALaterAssignSupersedesAnEarlierOneStillLoading)
{
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
    const StringId slot = HashStringId("albedoMap");
    const auto firstPath = Image();
    const GUID first = Register(firstPath, false);
    const GUID second = Register(Image(), false);
    MaterialDocument doc{};
    doc.schemaVersion = 3;
    doc.materialName = "TextureReassign";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    const GUID material = GUID::Generate();
    ASSERT_NE(m_Materials.Register(material, doc), nullptr);

    // The first assign's load has landed but is not uploaded yet.
    auto firstDecoded = MakeShared<TextureAsset>(first, firstPath);
    ASSERT_TRUE(firstDecoded->Load());
    Access::Queue(m_Textures, first, material, firstDecoded, slot);
    doc.textures["albedoMap"] = second.ToString();
    m_Textures.UpdateMaterialTextures(material, doc);
    m_Textures.FlushPendingUploads();
    const Engine::Renderer::Material* bound = m_Materials.Find(material);
    ASSERT_NE(bound, nullptr);
    const Rendering::TextureHandle firstUpload = m_Textures.GetOrUpload(first);
    ASSERT_TRUE(firstUpload.IsValid());
    EXPECT_NE(bound->GetTexture(slot), firstUpload) << "the superseded assign bound when its load landed";

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!m_Textures.IsMaterialTextureBindingComplete(material) && std::chrono::steady_clock::now() < deadline)
    {
        EngineCore::GetInstance().GetAssetManager().Update();
        m_Textures.FlushPendingUploads();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(m_Textures.IsMaterialTextureBindingComplete(material));
    EXPECT_EQ(bound->GetTexture(slot), m_Textures.GetOrUpload(second));
}

TEST_F(TextureCoverageGPU, LateInvalidPolicyDrainsQueuedBindAndLoadHandleThenRecovers)
{
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
    const auto path = Image(true);
    const GUID guid = Register(path, true);
    auto decoded = MakeShared<TextureAsset>(guid, path);
    ASSERT_TRUE(decoded->Load());
    ASSERT_EQ(decoded->GetChannels(), 2u);
    const auto material = GUID::Generate();
    Access::Queue(m_Textures, guid, material, decoded);
    ASSERT_EQ(m_Textures.PendingMaterialTextureBindCount(), 1u);
    ASSERT_TRUE(Access::HasLoadHandle(m_Textures, guid));
    // Metadata changed after successful decode but before the render-thread drain.
    Set(path, kTextureAlphaCutoffMetaKey, "nan");
    m_Textures.FlushPendingUploads();
    EXPECT_EQ(m_Textures.PendingMaterialTextureBindCount(), 0u);
    EXPECT_FALSE(Access::HasLoadHandle(m_Textures, guid));
    EXPECT_TRUE(m_Textures.IsMaterialTextureBindingComplete(material));
    EXPECT_FALSE(m_Textures.GetOrUpload(guid).IsValid());
    Set(path, kTextureAlphaCutoffMetaKey, ".538");
    m_Textures.Evict(guid);
    Access::Queue(m_Textures, guid, material, decoded);
    m_Textures.FlushPendingUploads();
    EXPECT_EQ(m_Textures.PendingMaterialTextureBindCount(), 0u);
    EXPECT_FALSE(Access::HasLoadHandle(m_Textures, guid));
    const auto recovered = m_Textures.GetOrUpload(guid);
    ASSERT_TRUE(recovered.IsValid());
    const auto pixels = Render(recovered, 64, 64, 0);
    ASSERT_EQ(pixels.size(), 64u * 64u * 4u);
    bool transparent = false, opaque = false;
    for (size_t i = 0; i < pixels.size(); i += 4)
    {
        EXPECT_FLOAT_EQ(pixels[i], pixels[i + 1]);
        EXPECT_FLOAT_EQ(pixels[i], pixels[i + 2]);
        transparent |= pixels[i + 3] == 0;
        opaque |= pixels[i + 3] == 1;
    }
    EXPECT_TRUE(transparent);
    EXPECT_TRUE(opaque);
}

TEST_F(TextureCoverageGPU, InvalidMetadataCannotPublishUncorrectedRawTexture)
{
    for (const auto& invalid : {std::array<const char*, 3>{"true", ".538", "none"},
                                {"1", "nan", "none"},
                                {"1", ".538", "bc1"},
                                {"1", ".538", "bc4"},
                                {"1", ".538", "bc5"},
                                {"1", ".538", "bc6h"}})
    {
        const auto path = Image();
        const GUID guid = Register(path, true);
        Set(path, kTextureAlphaCoverageMetaKey, invalid[0]);
        Set(path, kTextureAlphaCutoffMetaKey, invalid[1]);
        Set(path, kTextureCompressionMetaKey, invalid[2]);
        EXPECT_FALSE(m_Textures.GetOrUpload(guid).IsValid());
        Set(path, kTextureAlphaCoverageMetaKey, "0");
        Set(path, kTextureCompressionMetaKey, "none");
        m_Textures.Evict(guid);
        EXPECT_TRUE(m_Textures.GetOrUpload(guid).IsValid());
    }
}

TEST_F(TextureCoverageGPU, FullChainMaskedCardsMeasureIntegerFractionalAndPhaseCoverage)
{
#if defined(GE_HAVE_KTX)
    const auto compressionCases = {false, true};
#else
    const auto compressionCases = {false};
#endif
    for (bool bc7 : compressionCases)
    {
        SCOPED_TRACE(bc7);
        const auto ordinaryPath = Image(), coveredPath = Image();
        const GUID ordinaryGuid = Register(ordinaryPath, false), coveredGuid = Register(coveredPath, true);
        if (bc7)
        {
            if (!IsTextureCookEncoderAvailable() ||
                TextureAsset::GetGpuTranscodeTarget() != TextureGpuTranscodeTarget::BC7)
                GTEST_SKIP() << "BC7 encoder or device sampling unavailable";
            for (const auto& pair : {std::pair{ordinaryPath, ordinaryGuid}, std::pair{coveredPath, coveredGuid}})
            {
                Set(pair.first, kTextureCompressionMetaKey, "bc7");
                AssetManager::ScopedThreadAssetManager context(&EngineCore::GetInstance().GetAssetManager());
                TextureAsset cooked(pair.second, pair.first);
                ASSERT_TRUE(cooked.Load());
                ASSERT_EQ(cooked.GetFormat(), GameEngine::TextureFormat::BC7);
                ASSERT_EQ(cooked.GetMipChain().size(), 7u);
            }
        }
        const auto ordinary = m_Textures.GetOrUpload(ordinaryGuid);
        const auto covered = m_Textures.GetOrUpload(coveredGuid);
        ASSERT_TRUE(ordinary.IsValid());
        ASSERT_TRUE(covered.IsValid());
        const double reference = Coverage(Render(ordinary, 256, 256, 0, 1));
        double ordinaryError = 0, coveredError = 0;
        for (float lod = 0; lod <= 6; lod += .5f)
            for (float phase : {0.0f, .31f / 64.0f, .73f / 64.0f})
            {
                const double before = Coverage(Render(ordinary, 256, 256, lod, 1, phase));
                const double after = Coverage(Render(covered, 256, 256, lod, 1, phase));
                EXPECT_GE(after, 0.0);
                EXPECT_LE(after, 1.0);
                // Coarsest tied levels are measured, but cannot represent the base ratio.
                // Improvement is asserted only over this complete, explicitly fixed sample set.
                ordinaryError += std::abs(before - reference);
                coveredError += std::abs(after - reference);
                std::cout << "coverage-sample bc7=" << bc7 << " lod=" << lod << " phase=" << phase << " base=" << reference
                          << " ordinary=" << before << " preserved=" << after << '\n';
            }
        EXPECT_LT(coveredError, ordinaryError);
        if (bc7)
        {
            AssetManager::ScopedThreadAssetManager context(&EngineCore::GetInstance().GetAssetManager());
            TextureAsset cooked(coveredGuid, coveredPath);
            ASSERT_TRUE(cooked.Load());
            for (uint32 level = 0; level < 7; ++level)
            {
                const auto& mip = cooked.GetMipChain()[level];
                std::vector<uint8> decoded;
                ASSERT_TRUE(DecodeBlockPayloadRGBA8(cooked.GetFormat(), mip.Width, mip.Height,
                                                    cooked.GetPixelData() + mip.Offset, mip.Size, decoded));
                const auto gpu = Render(covered, mip.Width, mip.Height, float(level));
                ASSERT_EQ(decoded.size(), gpu.size());
                for (size_t i = 0; i < decoded.size(); ++i)
                {
                    const float expected = i % 4 == 3 ? decoded[i] / 255.0f : LinearByte(decoded[i]);
                    // Independent BC7 decoders may round by one UNORM8 code;
                    // the RGB allowance also includes the sRGB transfer slope.
                    EXPECT_NEAR(gpu[i], expected, i % 4 == 3 ? 1.0f / 255.0f : 0.012f);
                }
            }
        }
    }
}
