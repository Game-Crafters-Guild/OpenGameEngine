// Execute the production bloom shaders on HDR textures. These tests measure
// pixels, including moving highlights and radius-boundary kernels; they do not
// duplicate the shader's filtering math on the CPU.
#include <gtest/gtest.h>

#include "Engine/Rendering/PostProcessSettings.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/QueryPool.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Materials/ShaderReflection.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

using namespace GameEngine::Rendering;
using GameEngine::Engine::Renderer::PostProcessSettings;

namespace
{
using Pixel = std::array<float, 4>;
using Value = std::variant<float, int32_t>;
using Params = std::map<std::string, Value>;

void Require(bool condition, const std::string& why)
{
    if (!condition) throw std::runtime_error(why);
}

struct Image
{
    TextureHandle Handle{};
    uint32_t Width = 0, Height = 0;
};

class BloomRendering : public ::testing::Test
{
protected:
    void SetUp() override
    {
#ifdef _WIN32
        _putenv_s("GE_HEADLESS_TEST", "1");
#else
        setenv("GE_HEADLESS_TEST", "1", 1);
#endif
        Utils::SetShaderPathResolver(+[](const std::filesystem::path& rel)
        {
            return std::filesystem::path(RENDERING_SHADER_OUTPUT_DIR) / rel;
        });
        DeviceDesc dd{};
        dd.preferredAPI = GraphicsAPI::Vulkan;
        dd.enableDynamicRendering = true;
        m_Device = DeviceFactory::CreateDevice(dd);
        if (!m_Device || !m_Device->Initialize(dd))
            GTEST_SKIP() << "No usable Vulkan device";
        m_Sampler = m_Device->CreateSampler(SamplerDesc::MaterialLinearClamp("BloomTests"));
        ASSERT_TRUE(m_Sampler.IsValid());
        BufferDesc bd{};
        bd.size = 2048;
        bd.usage = static_cast<uint32_t>(BufferUsage::Uniform | BufferUsage::Storage);
        bd.memoryUsage = BufferMemoryUsage::Upload;
        m_View = m_Device->CreateBuffer(bd);
        m_Exposure = m_Device->CreateBuffer(bd);
        ASSERT_TRUE(m_View.IsValid() && m_Exposure.IsValid());
        // Only near/far is read by bloom. A far distance of 1000 with sky depth
        // zero gives a deterministic full veil, independent of projection.
        std::array<float, 512> view{};
        view.fill(1000.0f);
        m_Device->UpdateBuffer(m_View, 0, sizeof(view), view.data());
        SetExposure(1.0f);
        m_Depth = Upload(1, 1, {{0, 0, 0, 0}});
    }

    void TearDown() override
    {
        if (!m_Device) return;
        m_Device->WaitForIdle();
        for (auto image : m_Images) m_Device->DestroyTexture(image.Handle);
        for (auto& [name, pipeline] : m_Pipelines) m_Device->DestroyPipeline(pipeline.Handle);
        if (m_Sampler.IsValid()) m_Device->DestroySampler(m_Sampler);
        if (m_View.IsValid()) m_Device->DestroyBuffer(m_View);
        if (m_Exposure.IsValid()) m_Device->DestroyBuffer(m_Exposure);
        m_Device->Shutdown();
    }

    void SetExposure(float scale)
    {
        std::array<uint32_t, 4> bytes{0, 1, 0, 0};
        std::memcpy(bytes.data(), &scale, sizeof(scale));
        m_Device->UpdateBuffer(m_Exposure, 0, sizeof(bytes), bytes.data());
    }

    Image Allocate(uint32_t w, uint32_t h)
    {
        TextureDesc td{};
        td.width = w; td.height = h; td.depth = 1;
        td.mipLevels = td.arrayLayers = td.sampleCount = 1;
        td.format = static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT);
        td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::RenderTarget |
                                         TextureUsage::TransferSrc | TextureUsage::TransferDst);
        td.debugName = "BloomTests.Image";
        Image image{m_Device->CreateTexture(td), w, h};
        Require(image.Handle.IsValid(), "HDR image allocation failed");
        m_Images.push_back(image);
        return image;
    }

    Image Upload(uint32_t w, uint32_t h, const std::vector<Pixel>& pixels)
    {
        Require(pixels.size() == w * h, "upload extent mismatch");
        auto image = Allocate(w, h);
        auto upload = m_Device->CreateUploadBuffer(pixels.size() * sizeof(Pixel), "BloomTests.Upload");
        Require(upload.IsValid(), "upload allocation failed");
        m_Device->UpdateBuffer(upload, 0, pixels.size() * sizeof(Pixel), pixels.data());
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(image.Handle, ResourceState::Undefined, ResourceState::CopyDest));
        cl->CopyBufferToTextureSubresource(upload, image.Handle, 0, 0, w, h, 0, w * sizeof(Pixel));
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(image.Handle, ResourceState::CopyDest, ResourceState::ShaderResource));
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->WaitForIdle();
        m_Device->DestroyBuffer(upload);
        return image;
    }

    Image Solid(uint32_t w, uint32_t h, Pixel color)
    {
        return Upload(w, h, std::vector<Pixel>(w * h, color));
    }

    struct Pipeline
    {
        PipelineHandle Handle{};
        PipelineDesc Desc{};
        ShaderMeta Meta{};
    };

    Pipeline& GetPipeline(const std::string& shader)
    {
        auto [it, added] = m_Pipelines.try_emplace(shader);
        if (!added) return it->second;
        auto& p = it->second;
        auto vs = Utils::LoadShaderFile("fullscreen_noinput.vert.spv");
        auto fs = Utils::LoadShaderFile((shader + ".frag.spv").c_str());
        Require(!vs.empty() && !fs.empty(), "missing production shader: " + shader);
        StageReflectionResult rvs{}, rfs{};
        ReflectionOptions opts{};
        Require(ReflectSpirv(ShaderStageKind::Vertex, reinterpret_cast<const uint32_t*>(vs.data()),
                            vs.size() / 4, opts, rvs, nullptr), "vertex reflection failed");
        Require(ReflectSpirv(ShaderStageKind::Fragment, reinterpret_cast<const uint32_t*>(fs.data()),
                            fs.size() / 4, opts, rfs, nullptr), "fragment reflection failed: " + shader);
        p.Meta = MergeStages({rvs, rfs});
        p.Desc.type = PipelineType::Graphics;
        p.Desc.vertexShader = vs;
        p.Desc.pixelShader = fs;
        MaterialBuilder::FormatsHint formats{};
        formats.ColorFormats = {static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT)};
        std::string why;
        Require(MaterialBuilder::BuildPipelineDescFromMeta(p.Meta, p.Desc, formats,
            MaterialBuilder::MergeMode::Auto, MaterialBuilder::PushConstantPolicy{}, &why), why);
        p.Handle = m_Device->CreatePipeline(p.Desc);
        Require(p.Handle.IsValid(), "pipeline creation failed: " + shader);
        return p;
    }

    Image Render(const std::string& shader, uint32_t w, uint32_t h,
                 const std::map<std::string, Image>& inputs, const Params& overrides = {})
    {
        auto& p = GetPipeline(shader);
        Require(p.Meta.Sets.size() == 1 && p.Meta.Sets[0].Set == 0, "unexpected bloom descriptor sets");
        Require(p.Meta.PushConstants.size() == 1, "missing bloom push constants");
        DescriptorSetDesc sd{};
        sd.layout = p.Desc.descriptorSetLayouts[0];
        sd.transient = false;
        auto ds = m_Device->CreateDescriptorSet(sd);
        Require(ds.IsValid(), "descriptor allocation failed");
        for (const auto& binding : p.Meta.Sets[0].Bindings)
        {
            if (binding.Type == ShaderMetaBindingType::kCombinedImageSampler)
            {
                auto it = inputs.find(binding.Name);
                Require(it != inputs.end() || binding.Name == "uScattering", "missing input " + binding.Name);
                auto handle = it == inputs.end() ? m_Depth.Handle : it->second.Handle;
                m_Device->UpdateCombinedImageSamplerBinding(ds, binding.Binding, handle, m_Sampler);
            }
            else if (binding.Type == ShaderMetaBindingType::kStorageBuffer)
                m_Device->UpdateStorageBufferBinding(ds, binding.Binding, m_Exposure, 0, 16);
            else if (binding.Type == ShaderMetaBindingType::kUniformBuffer)
                m_Device->UpdateBufferBinding(ds, binding.Binding, m_View, 0, 2048);
            else throw std::runtime_error("unexpected bloom descriptor");
        }
        const Params defaults{
            {"bloomOctaves", int32_t(3)},
            {"bloomAntiFlicker", int32_t(1)},
            {"exposure", 1.0f}, {"threshold", 1.0f}, {"knee", 0.1f},
            {"intensity", 1.0f},
            {"bloomTintR", 1.0f}, {"bloomTintG", 1.0f}, {"bloomTintB", 1.0f},
            {"bloomDepthVeilTintR", 1.0f}, {"bloomDepthVeilTintG", 1.0f}, {"bloomDepthVeilTintB", 1.0f},
            {"bloomDepthVeilStart", 25.0f}, {"bloomDepthVeilEnd", 500.0f},
            {"bloomSampleScale", 1.0f}, {"bloomScatter", 0.5f}, {"bloomOctaveBlend", 1.0f},
            {"bloomLensDirtScatter", 0.5f}
        };
        const auto& range = p.Meta.PushConstants[0];
        std::vector<uint8_t> bytes(range.Size, 0);
        size_t seen = 0;
        for (const auto& member : range.Block.Members)
        {
            const Value* value = nullptr;
            if (auto overrideIt = overrides.find(member.Name); overrideIt != overrides.end())
            {
                ++seen;
                value = &overrideIt->second;
            }
            else if (auto defaultIt = defaults.find(member.Name); defaultIt != defaults.end())
                value = &defaultIt->second;
            if (!value) continue;
            Require(member.Offset + 4 <= bytes.size(), "invalid reflected offset");
            std::visit([&](auto v) { std::memcpy(bytes.data() + member.Offset, &v, 4); }, *value);
        }
        Require(seen == overrides.size(), "an override was absent from " + shader + " reflection");
        auto image = Allocate(w, h);
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(image.Handle, ResourceState::Undefined, ResourceState::RenderTarget));
        RenderPassDesc rp{};
        rp.colorTargets[0] = image.Handle;
        rp.colorTargetCount = 1;
        rp.clearColor[0] = true;
        rp.colorStoreOp[0] = RenderPassDesc::StoreOp::Store;
        auto* queries = m_TimingEnabled ? m_Device->GetQueryPool() : nullptr;
        uint32_t start = ~0u, finish = ~0u;
        if (m_TimingEnabled)
        {
            Require(queries && queries->IsValid(), "GPU timestamps unavailable");
            queries->BeginFrame(0);
            queries->InvalidateCachedTimestampResults(0);
            queries->ResetQueries(cl.get());
            start = queries->WriteTimestamp(cl.get(), TimestampPoint::SpanBegin);
        }
        cl->BeginRenderPass(rp);
        cl->SetViewport(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
        cl->SetScissor(0, 0, w, h);
        cl->SetPipeline(p.Handle);
        cl->BindDescriptorSet(0, ds, p.Handle);
        cl->SetPushConstantsById(0, bytes.data(), bytes.size(), 0);
        cl->Draw(3, 1);
        cl->EndRenderPass();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(image.Handle, ResourceState::RenderTarget, ResourceState::ShaderResource));
        if (queries) finish = queries->WriteTimestamp(cl.get(), TimestampPoint::SpanEnd);
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->WaitForIdle();
        if (queries)
        {
            QueryResult begin{}, end{};
            Require(queries->GetTimestampResult(start, begin) && begin.Available &&
                    queries->GetTimestampResult(finish, end) && end.Available && end.Value > begin.Value,
                    "GPU timing interval unavailable");
            m_TimedGpuMs += queries->TimestampToMs(end.Value - begin.Value);
        }
        m_Device->DestroyDescriptorSet(ds);
        return image;
    }

    std::vector<Pixel> Read(Image image)
    {
        const size_t size = image.Width * image.Height * sizeof(Pixel);
        auto rb = m_Device->CreateReadbackBuffer(size, "BloomTests.Readback");
        Require(rb.IsValid(), "readback allocation failed");
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(image.Handle, ResourceState::ShaderResource, ResourceState::CopySource));
        cl->CopyTextureToBuffer(image.Handle, rb, image.Width, image.Height, 0, 0, 0, 0);
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(image.Handle, ResourceState::CopySource, ResourceState::ShaderResource));
        cl->End();
        m_Device->ExecuteCommandLists({cl.get()});
        m_Device->WaitForIdle();
        auto* data = m_Device->MapBuffer(rb);
        Require(data != nullptr, "readback map failed");
        std::vector<Pixel> pixels(image.Width * image.Height);
        std::memcpy(pixels.data(), data, size);
        m_Device->UnmapBuffer(rb);
        m_Device->DestroyBuffer(rb);
        for (const auto& px : pixels)
            for (float channel : px) Require(std::isfinite(channel), "nonfinite bloom output");
        return pixels;
    }

    Image Prefilter(Image scene, int mode, const Params& overrides = {})
    {
        Params p = overrides;
        p["thresholdFree"] = int32_t(mode == 2);
        return Render("bloom_threshold", std::max(scene.Width / 2, 1u), std::max(scene.Height / 2, 1u),
                      {{"uHDR", scene}, {"uDepth", m_Depth}}, p);
    }

    std::vector<Image> Pyramid(Image source, int count)
    {
        std::vector<Image> levels{source};
        for (int i = 1; i < count; ++i)
        {
            auto previous = levels.back();
            levels.push_back(Render("bloom_downsample", std::max(previous.Width / 2, 1u),
                std::max(previous.Height / 2, 1u), {{"uSrc", previous}},
                {}));
        }
        return levels;
    }

    Image Gather(const std::vector<Image>& levels, const PostProcessSettings& s)
    {
        std::map<std::string, Image> inputs;
        for (int i = 0; i < 8; ++i)
            inputs["uBloom" + std::to_string(i)] = levels[std::min(i, int(levels.size()) - 1)];
        return Render("bloom_octave_gather", levels[0].Width, levels[0].Height, inputs,
            {{"bloomOctaves", s.BloomOctaves}, {"bloomSampleScale", s.BloomSampleScale},
             {"bloomScatter", s.BloomScatter}, {"bloomOctaveBlend", s.BloomOctaveBlend}});
    }

    static double Energy(const std::vector<Pixel>& pixels, int channel = 0)
    {
        double sum = 0;
        for (const auto& px : pixels) sum += px[channel];
        return sum;
    }

    void ReleaseImagesAfter(size_t count)
    {
        m_Device->WaitForIdle();
        for (size_t i = count; i < m_Images.size(); ++i) m_Device->DestroyTexture(m_Images[i].Handle);
        m_Images.resize(count);
    }

    bool m_TimingEnabled = false;
    double m_TimedGpuMs = 0.0;
    std::unique_ptr<IDevice> m_Device;
    SamplerHandle m_Sampler{};
    BufferHandle m_View{}, m_Exposure{};
    Image m_Depth{};
    std::vector<Image> m_Images;
    std::map<std::string, Pipeline> m_Pipelines;
};

TEST_F(BloomRendering, EightDistinctOctavesMatchIndependentWeightedOracle)
{
    std::vector<Image> levels;
    const std::array<float, 8> values{0.125f, 3.0f, 0.75f, 11.0f, 2.0f, 7.0f, 1.5f, 19.0f};
    for (float v : values) levels.push_back(Solid(4, 4, {v, v * 0.25f, v * 0.5f, 0}));
    for (int count : {3, 4, 8})
        for (float scatter : {0.0f, 0.5f, 1.0f})
            for (float fade : {0.2f, 1.0f})
            {
                PostProcessSettings s{};
                s.BloomOctaves = count; s.BloomScatter = scatter; s.BloomOctaveBlend = fade;
                double sum = 0, total = 0;
                for (int i = 0; i < count; ++i)
                {
                    double w = std::pow(double(i + 1), double(scatter) * 5.0 - 2.5);
                    if (count > 3 && i == count - 1) w *= fade;
                    sum += values[i] * w; total += w;
                }
                for (const auto& px : Read(Gather(levels, s)))
                    EXPECT_NEAR(px[0], sum / total, 2e-5) << count << " " << scatter << " " << fade;
            }
}

TEST_F(BloomRendering, AdditiveGainRemainsLinearWhileScatteringChanges)
{
    auto scene = Solid(8, 8, {4, 2, 1, 1});
    auto highlights = Solid(4, 4, {2, 1, 0.5f, 0});
    auto scattered = Solid(4, 4, {1, 0.5f, 0.25f, 0});
    for (float fraction : {0.0f, 0.3f, 1.0f})
        for (float gain : {0.0f, 0.5f, 4.0f})
            for (const auto& px : Read(Render("bloom_combine", 8, 8,
                    {{"uHDR", scene}, {"uBloom", highlights}, {"uScattering", scattered}},
                    {{"bloomScatteringAmount", fraction}, {"intensity", gain}})))
                EXPECT_NEAR(px[0], 4.0f * (1 - fraction) + fraction + 2.0f * gain, 1e-5);
}

TEST_F(BloomRendering, HalationDoesNotTintMidtonesAndWorksWithoutBloom)
{
    for (float exposure : {0.001f, 1.0f, 16.0f})
    {
        SetExposure(exposure);
        for (float displayed : {0.0f, 0.18f, 0.8f, 4.0f})
        {
            auto scene = Solid(32, 16, {displayed / exposure, displayed / exposure, displayed / exposure, 1});
            auto source = Render("halation_prefilter", 16, 8, {{"uHDR", scene}},
                                 {{"useAutoExposure", int32_t(1)}});
            auto horizontal = Render("halation_blur_h", 16, 8, {{"uHighlights", source}},
                                     {{"halationRadius", 8.0f}});
            for (float amount : {0.0f, 0.5f, 2.0f})
            {
                auto result = Read(Render("halation_composite", 32, 16,
                    {{"uHDR", scene}, {"uHighlights", horizontal}},
                    {{"halationRadius", 8.0f}, {"halationIntensity", amount},
                     {"halationTintR", 1.0f}, {"halationTintG", 0.0f}, {"halationTintB", 0.0f}}));
                for (const auto& px : result)
                {
                    EXPECT_NEAR(px[0] * exposure, displayed + std::max(displayed - 1.0f, 0.0f) * amount, 3e-5);
                    EXPECT_NEAR(px[1] * exposure, displayed, 3e-5);
                    EXPECT_NEAR(px[2] * exposure, displayed, 3e-5);
                }
            }
        }
    }
}

TEST_F(BloomRendering, PureScatteringDoesNotSampleElidedHighlightAlias)
{
    auto scene = Solid(8, 8, {4, 2, 1, 1});
    auto scattered = Solid(4, 4, {1, 0.5f, 0.25f, 1});
    for (int invalidVeil : {0, 1})
        for (float gain : {0.0f, 0.0001f})
            for (const auto& px : Read(Render("bloom_combine", 8, 8,
                {{"uHDR", scene}, {"uBloom", scene}, {"uScattering", scattered}},
                {{"bloomScatteringAmount", 0.5f}, {"intensity", gain},
                 {"bloomDepthVeilEnabled", int32_t(invalidVeil)}, {"bloomDepthVeilIntensity", 1.0f},
                 {"bloomDepthVeilStart", 25.0f}, {"bloomDepthVeilEnd", 25.0f}})))
                EXPECT_NEAR(px[0], 2.5f, 1e-6) << "elided alias alpha must not add a white veil";
}

TEST_F(BloomRendering, HalationRadiusSpreadsHighlightsWithoutChangingEnergy)
{
    constexpr int w = 128, h = 128;
    std::vector<Pixel> pixels(w * h, {0.18f, 0.18f, 0.18f, 1});
    for (int y = 60; y < 68; ++y)
        for (int x = 60; x < 68; ++x) pixels[y * w + x] = {8, 8, 8, 1};
    auto scene = Upload(w, h, pixels);
    auto source = Render("halation_prefilter", w / 2, h / 2, {{"uHDR", scene}});
    double lastVariance = 0;
    for (float radius : {3.0f, 16.0f, 32.0f})
    {
        auto horizontal = Render("halation_blur_h", w / 2, h / 2, {{"uHighlights", source}},
                                 {{"halationRadius", radius}});
        auto result = Read(Render("halation_composite", w, h, {{"uHDR", scene}, {"uHighlights", horizontal}},
            {{"halationRadius", radius}, {"halationIntensity", 1.0f}, {"halationTintR", 1.0f}}));
        double added = 0, secondMoment = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const double v = result[y * w + x][0] - pixels[y * w + x][0];
                EXPECT_GE(v, -1e-5);
                added += v;
                secondMoment += v * ((x - 63.5) * (x - 63.5) + (y - 63.5) * (y - 63.5));
            }
        EXPECT_NEAR(added, 64.0 * 7.0, 0.1);
        EXPECT_GT(secondMoment / added, lastVariance);
        lastVariance = secondMoment / added;
        EXPECT_NEAR(result.front()[0], 0.18f, 1e-6) << "no full-frame red haze";
    }
}

TEST_F(BloomRendering, AntiFlickerBoundsAnIsolatedHotTexelAcrossExposure)
{
    for (float exposure : {0.01f, 1.0f, 8.0f})
    {
        SetExposure(exposure);
        std::vector<Pixel> pixels(64 * 32, {0, 0, 0, 1});
        pixels[16 * 64 + 32] = {600.0f / exposure, 150.0f / exposure, 0, 1};
        auto scene = Upload(64, 32, pixels);
        auto source = Prefilter(scene, 1, {{"useAutoExposure", int32_t(1)}});
        const auto result = Read(Pyramid(source, 2).back());
        float peak = 0;
        for (const auto& px : result) peak = std::max(peak, px[0] * exposure);
        EXPECT_LT(peak, 1.0f);
        EXPECT_GT(peak, 0.0f);
    }
}

TEST_F(BloomRendering, ScatteringPreservesUniformSceneAcrossExposureAndAmount)
{
    for (float brightness : {0.02f, 0.18f, 2.0f, 2000.0f})
    {
        const Pixel color{brightness, brightness * 0.5f, brightness * 0.25f, 1};
        auto scene = Solid(64, 32, color);
        for (float exposure : {0.0001f, 1.0f, 16.0f})
        {
            SetExposure(exposure);
            auto source = Prefilter(scene, 2, {{"useAutoExposure", int32_t(1)}});
            auto levels = Pyramid(source, 4);
            PostProcessSettings s{};

            s.BloomOctaves = 4;
            auto bloom = Gather(levels, s);
            for (float amount : {0.0f, 0.05f, 0.5f, 1.0f})
            {
                auto result = Read(Render("bloom_combine", 64, 32, {{"uHDR", scene}, {"uBloom", bloom}, {"uScattering", bloom}},
                    {{"bloomScatteringAmount", amount}, {"intensity", 0.0f}}));
                float worst = 0.0f;
                for (const auto& pixel : result)
                    for (int c = 0; c < 3; ++c)
                        worst = std::max(worst, std::abs(pixel[c] - color[c]) / std::max(1e-5f, color[c] * 2e-5f));
                EXPECT_LE(worst, 1.0f) << "brightness " << brightness << ", exposure " << exposure << ", amount " << amount;
            }
        }
    }
}

TEST_F(BloomRendering, AdditiveIntensityIsLinearAndDoesNotDimTheScene)
{
    auto scene = Solid(64, 32, {4, 2, 1, 1});
    auto source = Prefilter(scene, 1);
    PostProcessSettings s{};

    auto bloom = Gather(Pyramid(source, 3), s);
    std::vector<double> gains;
    for (float amount : {0.0f, 0.25f, 1.0f, 4.0f})
    {
        auto result = Read(Render("bloom_combine", 64, 32, {{"uHDR", scene}, {"uBloom", bloom}, {"uScattering", bloom}},
            {{"intensity", amount}}));
        const double gain = Energy(result) / result.size() - 4.0;
        gains.push_back(gain);
    }
    EXPECT_NEAR(gains[0], 0.0, 1e-6);
    ASSERT_GT(gains[2], 0.1);
    EXPECT_NEAR(gains[1], gains[2] * 0.25, 1e-5);
    EXPECT_NEAR(gains[3], gains[2] * 4.0, 1e-5);
}

TEST_F(BloomRendering, ReconstructionUsesLinearSampling)
{
    auto source = Upload(2, 1, {{0, 0, 0, 0}, {1, 1, 1, 0}});
    auto bloom = Solid(1, 1, {0, 0, 0, 0});
    auto result = Read(Render("bloom_combine", 64, 1, {{"uHDR", source}, {"uBloom", bloom}, {"uScattering", bloom}},
                              {{"intensity", 0.0f}}));
    for (int x = 0; x < 64; ++x)
        EXPECT_NEAR(result[x][0], std::clamp((x + 0.5f) / 32.0f - 0.5f, 0.0f, 1.0f), 1e-5)
            << "pixel " << x;
}

TEST_F(BloomRendering, RadiusKernelIsContinuousAtEveryOctaveBoundary)
{
    std::vector<Pixel> pixels(512 * 256, {0, 0, 0, 0});
    for (int y = 120; y < 136; ++y)
        for (int x = 248; x < 264; ++x) pixels[y * 512 + x] = {4, 2, 1, 0};
    auto levels = Pyramid(Upload(512, 256, pixels), 8);
    for (float scatter : {0.0f, 0.5f, 1.0f})
    {
        for (int boundary = 3; boundary <= 8; ++boundary)
        {
            // At height 1024 the requested extent is Radius + 2.
            PostProcessSettings left{}, right{};

            left.BloomOctaves = right.BloomOctaves = 8;
            left.BloomScatter = right.BloomScatter = scatter;
            left.BloomRadius = float(boundary - 2) - 0.0001f;
            right.BloomRadius = float(boundary - 2) + 0.0001f;
            left.ResolveBloomPyramid(1024);
            right.ResolveBloomPyramid(1024);
            auto a = Read(Gather(levels, left));
            auto b = Read(Gather(levels, right));
            double difference = 0;
            for (size_t i = 0; i < a.size(); ++i) difference += std::abs(a[i][0] - b[i][0]);
            ASSERT_GT(Energy(a), 1.0);
            EXPECT_LT(difference / Energy(a), 0.002) << "octave " << boundary << ", scatter " << scatter;
        }
    }
}

TEST_F(BloomRendering, MovingSmallColoredHighlightKeepsEnergyAndHue)
{
    std::vector<double> modern;
    for (int phase = 0; phase <= 16; ++phase)
    {
        const float f = phase / 16.0f;
        std::vector<Pixel> pixels(64 * 32, {0, 0, 0, 1});
        // A subpixel emitter moving across one source pixel with constant energy.
        pixels[16 * 64 + 31] = {16 * (1 - f), 4 * (1 - f), 0, 1};
        pixels[16 * 64 + 32] = {16 * f, 4 * f, 0, 1};
        auto scene = Upload(64, 32, pixels);
        auto modernSource = Prefilter(scene, 1, {{"threshold", 0.0f}, {"knee", 0.0f}});
        auto a = Read(Pyramid(modernSource, 2).back());
        modern.push_back(Energy(a));
        EXPECT_NEAR(Energy(a, 1) / Energy(a), 0.25, 1e-5);
        EXPECT_NEAR(Energy(a, 2), 0.0, 1e-7);
    }
    const auto [lo, hi] = std::minmax_element(modern.begin(), modern.end());
    EXPECT_GT(*lo, 0.99); // quarter-resolution: 16 source units / 16 pixels
    EXPECT_LT((*hi - *lo) / *hi, 0.01);
    RecordProperty("modern_energy_min", *lo);
    RecordProperty("modern_energy_max", *hi);
}

TEST_F(BloomRendering, AdditiveDepthVeilSurvivesAtZeroHighlightIntensity)
{
    auto scene = Solid(64, 32, {0, 0, 0, 1});
    auto source = Prefilter(scene, 1, {{"bloomDepthVeilEnabled", int32_t(1)},
                                     {"bloomDepthVeilIntensity", 2.0f}});
    PostProcessSettings s{};

    auto bloom = Gather(Pyramid(source, 3), s);
    auto result = Read(Render("bloom_combine", 64, 32, {{"uHDR", scene}, {"uBloom", bloom}, {"uScattering", bloom}},
        {{"intensity", 0.0f}, {"bloomDepthVeilEnabled", int32_t(1)}, {"bloomDepthVeilIntensity", 2.0f}, {"bloomDepthVeilTintR", 0.25f},
         {"bloomDepthVeilTintG", 0.5f}, {"bloomDepthVeilTintB", 1.0f}}));
    for (const auto& px : result)
    {
        EXPECT_NEAR(px[0], 0.5f, 1e-5);
        EXPECT_NEAR(px[1], 1.0f, 1e-5);
        EXPECT_NEAR(px[2], 2.0f, 1e-5);
    }
}

TEST_F(BloomRendering, ScatteringRedistributesLightWithoutChangingItsMean)
{
    constexpr uint32_t width = 512, height = 256;
    std::vector<Pixel> pixels(width * height, {0.04f, 0.04f, 0.04f, 1.0f});
    for (uint32_t y = 104; y < 152; ++y)
        for (uint32_t x = 232; x < 280; ++x)
            pixels[y * width + x] = {16.0f, 4.0f, 0.5f, 1.0f};
    for (uint32_t y = 112; y < 144; ++y)
        pixels[y * width + 176] = {0.0f, 4.0f, 16.0f, 1.0f};
    pixels[128 * width + 336] = {2.0f, 16.0f, 4.0f, 1.0f};
    auto scene = Upload(width, height, pixels);
    const auto exportPixels = [&](const char* name, const std::vector<Pixel>& image)
    {
        if (const char* directory = std::getenv("GE_BLOOM_EVIDENCE_DIR"))
        {
            std::filesystem::create_directories(directory);
            std::ofstream file(std::filesystem::path(directory) / (std::string(name) + ".rgba32f"),
                               std::ios::binary);
            file.write(reinterpret_cast<const char*>(&width), sizeof(width));
            file.write(reinterpret_cast<const char*>(&height), sizeof(height));
            file.write(reinterpret_cast<const char*>(image.data()), image.size() * sizeof(Pixel));
            Require(file.good(), "could not write bloom evidence");
        }
    };
    exportPixels("source", pixels);
    for (int mode : {1, 2})
    {
        PostProcessSettings settings{};

        settings.BloomOctaves = 8;
        settings.BloomRadius = 4.0f;
        settings.ResolveBloomPyramid(height);
        auto bloom = Gather(Pyramid(Prefilter(scene, mode), settings.BloomOctaves), settings);
            auto result = Read(Render("bloom_combine", width, height, {{"uHDR", scene}, {"uBloom", bloom}, {"uScattering", bloom}},
            {{"bloomScatteringAmount", mode == 2 ? 0.05f : 0.0f}, {"intensity", mode == 2 ? 0.0f : 1.0f}}));
        if (mode == 2)
        {
            for (int c = 0; c < 3; ++c)
                EXPECT_NEAR(Energy(result, c), Energy(pixels, c), Energy(pixels, c) * 0.002);
            EXPECT_LT(result[128 * width + 256][0], pixels[128 * width + 256][0]);
            EXPECT_GT(result[128 * width + 225][0], pixels[128 * width + 225][0])
                << "the source must actually spread outside the emitter";
        }
        exportPixels(mode == 1 ? "additive" : "scattering", result);
    }
}

// Export the same stress scene used for visual inspection, and measure the
// halo outside the sharp emitter where reconstruction artifacts are visible.
TEST_F(BloomRendering, HighIntensityHaloHasSmoothFalloff)
{
    constexpr uint32_t width = 1024, height = 512;
    std::vector<Pixel> pixels(width * height, {0.001f, 0.001f, 0.001f, 1.0f});
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
        {
            const float dx = float(x) - 512.0f, dy = float(y) - 256.0f;
            const float coverage = std::clamp(22.5f - std::sqrt(dx*dx + dy*dy), 0.0f, 1.0f);
            pixels[y * width + x] = {0.001f + 32.0f * coverage,
                0.001f + 10.0f * coverage, 0.001f + 2.0f * coverage, 1.0f};
        }
    auto scene = Upload(width, height, pixels);
    PostProcessSettings settings{};

    settings.BloomOctaves = 8;
    settings.BloomRadius = 5.0f;
    settings.BloomScatter = 0.8f;
    settings.ResolveBloomPyramid(height);
    auto bloom = Gather(Pyramid(Prefilter(scene, 1), settings.BloomOctaves), settings);
    auto result = Read(Render("bloom_combine", width, height, {{"uHDR", scene}, {"uBloom", bloom}, {"uScattering", bloom}},
        {{"intensity", 8.0f}}));
    const auto display = [](float value) { return std::pow(value / (1.0f + value), 1.0f / 2.2f); };
    float largestStep = 0.0f;
    for (int y = 32; y < 480; ++y)
        for (int x = 32; x < 992; ++x)
        {
            const int dx = x - 512, dy = y - 256;
            if (dx*dx + dy*dy < 60*60) continue;
            const float c = display(result[y * width + x][0]);
            largestStep = std::max(largestStep, std::abs(c - display(result[y * width + x + 1][0])));
            largestStep = std::max(largestStep, std::abs(c - display(result[(y + 1) * width + x][0])));
        }
    EXPECT_LT(largestStep, 0.02f) << "visible pixel step in the outer halo";
    float ringMinimum = 1.0f, ringMaximum = 0.0f;
    for (int angle = 0; angle < 360; ++angle)
    {
        const float radians = angle * (3.14159265359f / 180.0f);
        const int x = 512 + static_cast<int>(std::lround(160.0f * std::cos(radians)));
        const int y = 256 + static_cast<int>(std::lround(160.0f * std::sin(radians)));
        const float value = display(result[y * width + x][0]);
        ringMinimum = std::min(ringMinimum, value);
        ringMaximum = std::max(ringMaximum, value);
    }
    EXPECT_LT(ringMaximum - ringMinimum, 0.02f) << "angular outline around a round emitter";
    // Compare complete isophote contours, not just brightness along one
    // circle. A smooth rounded square can pass a single-ring brightness test.
    float worstContourVariation = 0.0f;
    for (float level : {0.25f, 0.5f, 0.75f})
    {
        std::vector<float> radii;
        for (int angle = 0; angle < 360; angle += 3)
        {
            const float radians = angle * (3.14159265359f / 180.0f);
            bool found = false;
            for (float radius = 24.0f; radius < 254.0f; radius += 0.25f)
            {
                const float x = 512.0f + radius * std::cos(radians);
                const float y = 256.0f + radius * std::sin(radians);
                const int ix = static_cast<int>(x), iy = static_cast<int>(y);
                const float fx = x - ix, fy = y - iy;
                const float value = display(result[iy * width + ix][0]) * (1-fx) * (1-fy)
                    + display(result[iy * width + ix + 1][0]) * fx * (1-fy)
                    + display(result[(iy + 1) * width + ix][0]) * (1-fx) * fy
                    + display(result[(iy + 1) * width + ix + 1][0]) * fx * fy;
                if (value < level) { radii.push_back(radius); found = true; break; }
            }
            ASSERT_TRUE(found) << "contour left the image at level " << level << ", angle " << angle;
        }
        const auto [minimum, maximum] = std::minmax_element(radii.begin(), radii.end());
        const float mean = std::accumulate(radii.begin(), radii.end(), 0.0f) / radii.size();
        const float variation = (*maximum - *minimum) / mean;
        EXPECT_LT(variation, 0.02f) << "rounded-square contour at display level " << level;
        worstContourVariation = std::max(worstContourVariation, variation);
    }
    RecordProperty("maximum_contour_radius_variation", worstContourVariation);
    RecordProperty("maximum_display_step", largestStep);
    RecordProperty("halo_ring_display_range", ringMaximum - ringMinimum);
    if (const char* directory = std::getenv("GE_BLOOM_EVIDENCE_DIR"))
    {
        std::filesystem::create_directories(directory);
        const auto write = [&](const char* name, const std::vector<Pixel>& image)
        {
            std::ofstream file(std::filesystem::path(directory) / name, std::ios::binary);
            file.write(reinterpret_cast<const char*>(&width), sizeof(width));
            file.write(reinterpret_cast<const char*>(&height), sizeof(height));
            file.write(reinterpret_cast<const char*>(image.data()), image.size() * sizeof(Pixel));
            Require(file.good(), "could not write high-intensity bloom evidence");
        };
        write("high-intensity-source.rgba32f", pixels);
        write("high-intensity-additive.rgba32f", result);
    }
}

TEST_F(BloomRendering, WidePrefilterDoesNotEraseIsolatedThresholdedLights)
{
    std::vector<Pixel> pixels(64 * 32, {0, 0, 0, 1});
    pixels[16 * 64 + 32] = {0, 16, 4, 1};
    auto result = Read(Prefilter(Upload(64, 32, pixels), 1));
    float peak = 0.0f;
    for (const auto& pixel : result) peak = std::max(peak, pixel[1]);
    EXPECT_LT(peak, 0.9f) << "probe must be diluted below the threshold after averaging";
    EXPECT_GT(Energy(result, 1), 2.9)
        << "a bright emitter must survive even when its filtered peak falls below threshold";
    EXPECT_NEAR(Energy(result, 2) / Energy(result, 1), 0.25, 1e-5);
}
} // namespace
