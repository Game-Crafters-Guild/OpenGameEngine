// Execute the shipping GTAO shaders on synthetic depth. These tests exercise
// real texture formats, representative pixel locations, radius rejection and
// temporal disocclusion; they do not duplicate the GLSL estimator on the CPU.
#include "Engine/Rendering/AmbientOcclusion/GtaoQuality.h"
#include "Engine/Rendering/Pipeline/Nodes/AONode.h"
#include "Engine/Rendering/Pipeline/Nodes/ViewParamsUploadNode.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/BarrierMapping.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/NamedPushConstantWriter.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/QueryPool.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/ViewParamsLayout.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "TestDeviceHelper.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;

namespace
{
struct Image
{
    TextureHandle Handle;
    uint32_t Width, Height, Levels, Components;
    bool Half;
    std::vector<ResourceState> States;
};
struct Storage
{
    const char* Name;
    Image* Texture;
    uint32_t Mip = 0;
};
struct Program
{
    ShaderMeta Meta;
    DescriptorSetLayoutDesc Layout;
    PipelineHandle Pipeline;
};

class GtaoComputeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        if (const char* api = std::getenv("GE_GTAO_TEST_API"); api && std::string(api) == "metal")
        {
            DeviceDesc desc{};
            desc.preferredAPI = GraphicsAPI::Metal;
            desc.enableDebugLayer = false;
            Device = DeviceFactory::CreateDevice(desc);
            if (Device && !Device->Initialize(desc))
                Device.reset();
        }
        else
            Device = CreateVulkanDeviceFast();
        if (!Device)
            GTEST_SKIP() << "Requested graphics device unavailable";
        Sampler = Device->CreateSampler(SamplerDesc::PointClamp("GTAO.Test"));
        SetCamera(32, 32);
    }
    void TearDown() override
    {
        if (!Device)
            return;
        Device->WaitForIdle();
        for (auto v : Views)
            Device->DestroyTextureView(v);
        for (auto& i : Images)
            Device->DestroyTexture(i->Handle);
        for (auto b : Buffers)
            Device->DestroyBuffer(b);
        Device->Shutdown();
    }
    void SetCamera(uint32_t w, uint32_t h, bool ortho = false)
    {
        Params = {};
        const glm::mat4 proj = ortho ? glm::orthoLH_ZO(-2.0f, 2.0f, -2.0f, 2.0f, Far, Near)
                                     : glm::perspectiveLH_ZO(1.570796327f, float(w) / h, Far, Near);
        const glm::mat4 inverse = glm::inverse(proj);
        const glm::mat4 identity(1.0f);
        std::memcpy(Params.ge_proj, &proj[0][0], 64);
        std::memcpy(Params.ge_invProj, &inverse[0][0], 64);
        std::memcpy(Params.ge_viewProj, &proj[0][0], 64);
        std::memcpy(Params.ge_prevViewProj, &proj[0][0], 64);
        std::memcpy(Params.ge_view, &identity[0][0], 64);
        std::memcpy(Params.ge_invView, &identity[0][0], 64);
        Params.ge_nearFar[0] = Near;
        Params.ge_nearFar[1] = Far;
        Params.ge_screenSize[0] = float(w);
        Params.ge_screenSize[1] = float(h);
        Params.ge_screenSize[2] = 1.0f / w;
        Params.ge_screenSize[3] = 1.0f / h;
    }
    float Raw(float z) const
    {
        return (Params.ge_proj[10] * z + Params.ge_proj[14]) /
               (Params.ge_proj[11] * z + Params.ge_proj[15]);
    }
    BufferHandle Buffer(size_t bytes, BufferUsage usage, BufferMemoryUsage memory)
    {
        BufferDesc d{};
        d.size = bytes;
        d.usage = static_cast<uint32_t>(usage);
        d.memoryUsage = memory;
        auto b = Device->CreateBuffer(d);
        Buffers.push_back(b);
        EXPECT_TRUE(b.IsValid());
        return b;
    }
    Image* MakeImage(uint32_t w, uint32_t h, TextureFormat format, uint32_t levels = 1)
    {
        TextureDesc d{};
        d.width = w;
        d.height = h;
        d.depth = d.arrayLayers = d.sampleCount = 1;
        d.mipLevels = levels;
        d.format = static_cast<uint32_t>(format);
        d.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::UnorderedAccess |
                                        TextureUsage::TransferSrc | TextureUsage::TransferDst);
        uint32_t components = format == TextureFormat::R32_FLOAT ? 1u : format == TextureFormat::R32G32_FLOAT ? 2u
                                                                                                              : 4u;
        Images.push_back(std::make_unique<Image>(Image{Device->CreateTexture(d), w, h, levels, components,
                                                       format == TextureFormat::R16G16B16A16_FLOAT, std::vector<ResourceState>(levels, ResourceState::Undefined)}));
        EXPECT_TRUE(Images.back()->Handle.IsValid());
        return Images.back().get();
    }
    void Transition(CommandList* cl, Image* i, ResourceState state, uint32_t mip)
    {
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(i->Handle, i->States[mip], state, mip, 1));
        i->States[mip] = state;
    }
    void Submit(CommandList* cl)
    {
        cl->End();
        Device->ExecuteCommandLists({cl});
        Device->WaitForIdle();
    }
    void Upload(Image* i, const std::vector<float>& data)
    {
        EXPECT_EQ(data.size(), size_t(i->Width) * i->Height * i->Components);
        size_t bytes = data.size() * (i->Half ? 2 : 4);
        auto b = Buffer(bytes, BufferUsage::TransferSrc, BufferMemoryUsage::Upload);
        void* p = Device->MapBuffer(b);
        ASSERT_NE(p, nullptr);
        if (i->Half)
            for (size_t k = 0; k < data.size(); ++k)
                static_cast<uint16_t*>(p)[k] = glm::packHalf1x16(data[k]);
        else
            std::memcpy(p, data.data(), bytes);
        Device->UnmapBuffer(b);
        auto cl = Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->CopyBufferToTextureSubresource(b, i->Handle, 0, 0, i->Width, i->Height);
        i->States[0] = ResourceState::CopyDest;
        Submit(cl.get());
    }
    std::vector<float> Read(Image* i, uint32_t mip = 0)
    {
        uint32_t w = std::max(1u, i->Width >> mip), h = std::max(1u, i->Height >> mip);
        size_t count = size_t(w) * h * i->Components;
        auto b = Device->CreateReadbackBuffer(count * (i->Half ? 2 : 4));
        Buffers.push_back(b);
        auto cl = Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->CopyTextureSubresourceToBuffer(i->Handle, mip, 0, b, w, h, 0, 0, 0, 0);
        i->States[mip] = ResourceState::CopySource;
        Submit(cl.get());
        std::vector<float> result(count);
        const void* p = Device->MapBuffer(b);
        EXPECT_NE(p, nullptr);
        if (p)
        {
            if (i->Half)
                for (size_t k = 0; k < count; ++k)
                    result[k] = glm::unpackHalf1x16(static_cast<const uint16_t*>(p)[k]);
            else
                std::memcpy(result.data(), p, count * sizeof(float));
            Device->UnmapBuffer(b);
        }
        return result;
    }
    void Run(const char* name, uint32_t w, uint32_t h,
             std::initializer_list<std::pair<const char*, Image*>> sampled,
             std::initializer_list<Storage> storage,
             GtaoQualitySettings quality = GetGtaoQualitySettings(GtaoQuality::Medium),
             float radius = 2.0f, uint32_t frame = 0, uint32_t historyValid = 0)
    {
        auto& program = Programs[name];
        if (!program.Pipeline.IsValid())
        {
            ShaderPackage pkg{};
            std::string err;
            ASSERT_TRUE(LoadShaderPkg(std::string("Shaders/") + name + ".shaderpkg", Device->PreferredShaderSource(), pkg, &err)) << err;
            program.Meta = std::move(pkg.meta);
            ComputePipelineDesc cd{};
            cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(pkg.stageBytes.at("cs"));
            auto patch = [&](uint32_t set, DescriptorSetLayoutDesc& layout)
            { if (set == 0) program.Layout = layout; };
            MaterialHelper::ApplyShaderMetaToComputeDesc(*Device, program.Meta, cd, MaterialBuilder::MergeMode::Auto,
                                                         {true, 128}, patch, &err);
            auto id = Device->InternComputePipeline(std::move(cd));
            program.Pipeline = Device->GetOrCreateComputePipeline(id);
            ASSERT_TRUE(program.Pipeline.IsValid()) << name << ": " << err;
        }
        std::vector<std::pair<const char*, Image*>> inputs(sampled);
        if (std::string(name) == "gtao_temporal" &&
            std::none_of(inputs.begin(), inputs.end(), [](const auto& input)
                         { return std::string(input.first) == "uGuide"; }))
        {
            auto it = std::find_if(inputs.begin(), inputs.end(), [](const auto& input)
                                   { return std::string(input.first) == "uDepth"; });
            ASSERT_NE(it, inputs.end());
            auto* depth = it->second;
            auto* guide = MakeImage(w, h, TextureFormat::R32G32_FLOAT);
            auto* linear = MakeImage(depth->Width, depth->Height, TextureFormat::R32_FLOAT);
            Run("gtao_prepare", depth->Width, depth->Height, {{"uDepth", depth}}, {{"uGuide", guide}, {"uDepthOut", linear}});
            inputs.emplace_back("uGuide", guide);
        }
        auto* queries = CaptureTimings ? Device->GetQueryPool() : nullptr;
        if (queries)
            queries->BeginFrame(0);
        auto cl = Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        if (queries)
            queries->ResetQueries(cl.get());
        DescriptorSetDesc dd{};
        dd.layout = program.Layout;
        dd.transient = true;
        auto ds = Device->CreateDescriptorSet(dd);
        ASSERT_TRUE(ds.IsValid());
        NamedDescriptorWriter writer(Device.get(), ds, program.Meta, 0);
        if (writer.Has("ViewParams"))
        {
            auto b = Buffer(sizeof(Params), BufferUsage::Uniform, BufferMemoryUsage::Upload);
            auto* p = Device->MapBuffer(b);
            ASSERT_NE(p, nullptr);
            std::memcpy(p, &Params, sizeof(Params));
            Device->UnmapBuffer(b);
            writer.AddUniformBuffer("ViewParams", b, 0, sizeof(Params));
        }
        for (const auto& [binding, image] : inputs)
        {
            for (uint32_t mip = 0; mip < image->Levels; ++mip)
                Transition(cl.get(), image, ResourceState::ShaderResource, mip);
            writer.AddCombinedImageSampler(binding, image->Handle, Sampler);
        }
        for (const auto& binding : storage)
        {
            Transition(cl.get(), binding.Texture, ResourceState::UnorderedAccess, binding.Mip);
            uint32_t slot = ~0u;
            for (const auto& b : program.Meta.Sets)
                for (const auto& entry : b.Bindings)
                    if (entry.Name == binding.Name)
                        slot = entry.Binding;
            ASSERT_NE(slot, ~0u) << binding.Name;
            TextureViewDesc vd{};
            vd.viewType = TextureViewType::View2D;
            vd.baseMip = binding.Mip;
            vd.levelCount = vd.layerCount = 1;
            auto view = Device->CreateTextureView(binding.Texture->Handle, vd);
            Views.push_back(view);
            Device->UpdateStorageImageBinding(ds, slot, view);
        }
        writer.Flush();
        // Metal timestamp boundaries can close the active compute encoder.
        // Begin the measured span before binding encoder-local resources.
        uint32_t begin = queries ? queries->WriteTimestamp(cl.get(), TimestampPoint::SpanBegin, name) : ~0u;
        cl->SetPipeline(program.Pipeline);
        if (!program.Meta.PushConstants.empty())
        {
            NamedPushConstantWriter pc(program.Meta, program.Meta.PushConstants[0].Name);
            pc.Add("aoRadius", radius);
            pc.Add("aoIntensity", 1.0f);
            pc.Add("aoThickness", 0.5f);
            pc.Add("directions", quality.Directions);
            pc.Add("steps", SweepSteps);
            pc.Add("noiseFrame", frame);
            pc.Add("historyValid", historyValid);
            pc.Flush(cl.get());
        }
        cl->BindDescriptorSet(0, ds, program.Pipeline);
        for (uint32_t repeat = 0; repeat < BenchmarkRepeats; ++repeat)
        {
            cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
            if (repeat + 1 < BenchmarkRepeats)
                cl->Barrier(ResourceBarrier::CreateMemoryBarrier(
                    static_cast<uint64_t>(PipelineStageMask::ComputeShader), static_cast<uint64_t>(PipelineStageMask::ComputeShader),
                    static_cast<uint64_t>(ResourceAccessMask::ShaderWrite), static_cast<uint64_t>(ResourceAccessMask::ShaderWrite)));
        }
        uint32_t end = queries ? queries->WriteTimestamp(cl.get(), TimestampPoint::SpanEnd, name) : ~0u;
        Submit(cl.get());
        if (queries)
        {
            queries->EndFrame();
            QueryResult a{}, b{};
            if (queries->GetTimestampResult(begin, a) && queries->GetTimestampResult(end, b) && b.Value >= a.Value)
            {
                uint64_t ticks = b.Value - a.Value;
                (void)queries->GetTimestampSpanTicks(begin, end, ticks);
                Timings.emplace_back(name, queries->TimestampToMs(ticks) / BenchmarkRepeats);
            }
        }
    }
    struct Frame
    {
        Image* Guide;
        Image* Mips;
        Image* RawAO;
        Image* Spatial;
        Image* Filtered;
    };
    Frame Spatial(Image* depth, GtaoQualitySettings q = GetGtaoQualitySettings(GtaoQuality::Medium),
                  float radius = 2.0f, uint32_t noise = 0)
    {
        uint32_t w = depth->Width, h = depth->Height, aw = (w + q.ResolutionDivisor - 1) / q.ResolutionDivisor,
                 ah = (h + q.ResolutionDivisor - 1) / q.ResolutionDivisor, levels = 1;
        for (uint32_t s = std::max(w, h); s > 1 && levels <= q.MaxDepthMip; s >>= 1)
            ++levels;
        auto guide = MakeImage(aw, ah, TextureFormat::R32G32_FLOAT);
        auto mips = MakeImage(w, h, TextureFormat::R32_FLOAT, levels);
        Run("gtao_prepare", w, h, {{"uDepth", depth}}, {{"uGuide", guide}, {"uDepthOut", mips}});
        for (uint32_t mip = 1; mip < levels; ++mip)
            Run("gtao_depth_mip", std::max(1u, w >> mip), std::max(1u, h >> mip), {}, {{"uDepthIn", mips, mip - 1}, {"uDepthOut", mips, mip}});
        auto raw = MakeImage(aw, ah, TextureFormat::R16G16B16A16_FLOAT);
        Run("gtao", aw, ah, {{"uDepth", depth}, {"uGuide", guide}, {"uDepthMips", mips}}, {{"uAO", raw}}, q, radius, noise);
        auto blur = MakeImage(aw, ah, TextureFormat::R16G16B16A16_FLOAT);
        Run("gtao_blur", aw, ah, {{"uDepth", depth}, {"uGuide", guide}, {"uAOIn", raw}}, {{"uAOOut", blur}});
        auto output = blur;
        if (q.ResolutionDivisor == 2)
        {
            output = MakeImage(w, h, TextureFormat::R16G16B16A16_FLOAT);
            Run("gtao_upsample", w, h, {{"uDepth", depth}, {"uGuide", guide}, {"uAOIn", blur}}, {{"uAOOut", output}});
        }
        return {guide, mips, raw, output, blur};
    }
    // Read a texture the render graph owns (a pooled pipeline output) rather than
    // one this fixture allocated.
    std::vector<float> ReadPooled(TextureHandle texture, uint32_t w, uint32_t h)
    {
        const size_t count = size_t(w) * h * 4;
        auto b = Device->CreateReadbackBuffer(count * 2);
        Buffers.push_back(b);
        auto cl = Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::ShaderResource,
                                                          ResourceState::CopySource, 0, 1));
        cl->CopyTextureSubresourceToBuffer(texture, 0, 0, b, w, h, 0, 0, 0, 0);
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::CopySource,
                                                          ResourceState::ShaderResource, 0, 1));
        Submit(cl.get());
        std::vector<float> result(count);
        const void* p = Device->MapBuffer(b);
        EXPECT_NE(p, nullptr);
        if (p)
        {
            for (size_t k = 0; k < count; ++k)
                result[k] = glm::unpackHalf1x16(static_cast<const uint16_t*>(p)[k]);
            Device->UnmapBuffer(b);
        }
        return result;
    }
    void ExpectClear(const std::vector<float>& ao, float minimum = 0.99f)
    {
        for (size_t i = 0; i < ao.size(); i += 4)
        {
            EXPECT_TRUE(std::isfinite(ao[i]) && std::isfinite(ao[i + 1]) && std::isfinite(ao[i + 2]));
            EXPECT_GE(ao[i + 3], minimum) << "pixel " << i / 4;
            EXPECT_LE(ao[i + 3], 1.0f);
        }
    }
    std::unique_ptr<IDevice> Device;
    SamplerHandle Sampler;
    ViewParamsUBO Params{};
    const float Near = 0.1f, Far = 100.0f;
    std::vector<std::unique_ptr<Image>> Images;
    std::vector<BufferHandle> Buffers;
    std::vector<TextureViewHandle> Views;
    std::map<std::string, Program> Programs;
    // Radial taps per direction. Production always uses kGtaoSweepSteps; the
    // calibration test sweeps it to show which dial moves the occlusion amount.
    uint32_t SweepSteps = kGtaoSweepSteps;
    bool CaptureTimings = false;
    uint32_t BenchmarkRepeats = 1;
    std::vector<std::pair<std::string, double>> Timings;
};

TEST_F(GtaoComputeTest, SectorMomentsMatchIndependentHemisphereQuadrature)
{
    auto image = MakeImage(33, 4, TextureFormat::R32G32B32A32_FLOAT);
    Run("gtao_sectors_probe", 33, 4, {}, {{"uMoments", image}});
    auto result = Read(image);
    constexpr uint32_t masks[4] = {0u, 0xFFFFFFFFu, 0x0000FFFFu, 0x55555555u};
    constexpr double pi = 3.14159265358979323846;
    constexpr uint32_t samples = 16384;
    for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 33; ++x)
        {
            double n = -1.4 + double(x) * 2.8 / 32.0;
            double visibility = 0, tangent = 0, view = 0;
            for (uint32_t i = 0; i < samples; ++i)
            {
                if ((masks[y] & (1u << (i / (samples / 32)))) != 0)
                    continue;
                double theta = n - pi / 2 + (double(i) + 0.5) * pi / samples;
                double weight = std::cos(theta - n) * std::abs(std::sin(theta)) * pi / samples;
                visibility += weight;
                tangent += weight * std::sin(theta);
                view += weight * std::cos(theta);
            }
            const size_t pixel = (y * 33 + x) * 4;
            EXPECT_NEAR(result[pixel], visibility, 2e-5) << x << ',' << y;
            EXPECT_NEAR(result[pixel + 1], tangent, 2e-5) << x << ',' << y;
            EXPECT_NEAR(result[pixel + 2], view, 2e-5) << x << ',' << y;
        }
}

TEST_F(GtaoComputeTest, RepresentativeGuidePreservesThinPixelsAndOddEdges)
{
    SetCamera(3, 3);
    auto depth = MakeImage(3, 3, TextureFormat::R32_FLOAT);
    std::vector<float> data{Raw(5), Raw(2), 0, Raw(3), Raw(4), Raw(7), 0, 0, Raw(1)};
    Upload(depth, data);
    auto guide = MakeImage(2, 2, TextureFormat::R32G32_FLOAT);
    auto linear = MakeImage(3, 3, TextureFormat::R32_FLOAT);
    Run("gtao_prepare", 3, 3, {{"uDepth", depth}}, {{"uGuide", guide}, {"uDepthOut", linear}});
    const auto result = Read(guide);
    ASSERT_EQ(result.size(), 8u);
    EXPECT_FLOAT_EQ(result[0], Raw(2));
    EXPECT_FLOAT_EQ(result[1], 1.0f);
    EXPECT_FLOAT_EQ(result[2], Raw(7));
    EXPECT_FLOAT_EQ(result[3], 2.0f);
    EXPECT_FLOAT_EQ(result[4], 0.0f);
    EXPECT_FLOAT_EQ(result[5], 0.0f);
    EXPECT_FLOAT_EQ(result[6], Raw(1));
    EXPECT_FLOAT_EQ(result[7], 0.0f);
}

TEST_F(GtaoComputeTest, FlatPlanesStayUnoccludedIncludingImageBorders)
{
    for (bool ortho : {false, true})
        for (auto quality : {GtaoQuality::Medium, GtaoQuality::High, GtaoQuality::Ultra})
        {
            SCOPED_TRACE(int(quality));
            SCOPED_TRACE(ortho);
            SetCamera(31, 17, ortho);
            auto depth = MakeImage(31, 17, TextureFormat::R32_FLOAT);
            Upload(depth, std::vector<float>(31 * 17, Raw(5)));
            auto frame = Spatial(depth, GetGtaoQualitySettings(quality));
            ExpectClear(Read(frame.Spatial));
        }
}

TEST_F(GtaoComputeTest, SlopedPlaneDoesNotOccludeItself)
{
    SetCamera(63, 47);
    auto depth = MakeImage(63, 47, TextureFormat::R32_FLOAT);
    std::vector<float> data(63 * 47);
    for (uint32_t y = 0; y < 47; ++y)
        for (uint32_t x = 0; x < 63; ++x)
        {
            float nx = (float(x) + 0.5f) / 63 * 2 - 1;
            float ny = 1 - (float(y) + 0.5f) / 47 * 2;
            float z = 5.0f / (1.0f - 0.25f * nx / Params.ge_proj[0] - 0.2f * ny / Params.ge_proj[5]);
            data[y * 63 + x] = Raw(z);
        }
    Upload(depth, data);
    ExpectClear(Read(Spatial(depth, GetGtaoQualitySettings(GtaoQuality::Medium), 3.0f).Spatial), 0.98f);
}

TEST_F(GtaoComputeTest, DenoiserAveragesAcrossSteepPlaneThroughDepthPlaneFit)
{
    // A plane steep enough that adjacent texels differ by ~5% of their depth,
    // several times the plane tolerance: only the fitted-plane prediction keeps
    // the x-neighbours in the kernel. Vertical AO stripes then average out; a
    // depth-difference-only weight would leave them at full contrast.
    constexpr uint32_t kSize = 32;
    constexpr float kDarkStripe = 0.2f;
    SetCamera(kSize, kSize);
    auto depth = MakeImage(kSize, kSize, TextureFormat::R32_FLOAT);
    std::vector<float> data(kSize * kSize);
    for (uint32_t y = 0; y < kSize; ++y)
        for (uint32_t x = 0; x < kSize; ++x)
        {
            const float nx = (float(x) + 0.5f) / kSize * 2 - 1;
            data[y * kSize + x] = Raw(5.0f / (1.0f - 0.8f * nx / Params.ge_proj[0]));
        }
    Upload(depth, data);
    auto guide = MakeImage(kSize, kSize, TextureFormat::R32G32_FLOAT);
    auto linear = MakeImage(kSize, kSize, TextureFormat::R32_FLOAT);
    Run("gtao_prepare", kSize, kSize, {{"uDepth", depth}}, {{"uGuide", guide}, {"uDepthOut", linear}});
    auto raw = MakeImage(kSize, kSize, TextureFormat::R16G16B16A16_FLOAT);
    std::vector<float> stripes(kSize * kSize * 4);
    for (uint32_t y = 0; y < kSize; ++y)
        for (uint32_t x = 0; x < kSize; ++x)
        {
            float* texel = &stripes[(y * kSize + x) * 4];
            texel[0] = texel[1] = 0.0f;
            texel[2] = 1.0f;
            texel[3] = (x & 1u) ? kDarkStripe : 1.0f;
        }
    Upload(raw, stripes);
    auto blurred = MakeImage(kSize, kSize, TextureFormat::R16G16B16A16_FLOAT);
    Run("gtao_blur", kSize, kSize, {{"uDepth", depth}, {"uGuide", guide}, {"uAOIn", raw}}, {{"uAOOut", blurred}});
    const auto ao = Read(blurred);
    const float stripeMean = 0.5f * (1.0f + kDarkStripe);
    // Interior only: a truncated kernel at the border still blurs, just unevenly.
    for (uint32_t y = 2; y + 2 < kSize; ++y)
        for (uint32_t x = 2; x + 3 < kSize; ++x)
        {
            const float here = ao[(y * kSize + x) * 4 + 3];
            const float right = ao[(y * kSize + x + 1) * 4 + 3];
            EXPECT_NEAR(here, stripeMean, 0.06f) << "pixel " << x << "," << y;
            EXPECT_LT(std::abs(here - right), 0.1f) << "pixel " << x << "," << y;
        }
}

TEST_F(GtaoComputeTest, SkyAndSinglePixelExtentsStayFinite)
{
    for (float z : {0.0f, Raw(5)})
    {
        SetCamera(1, 1);
        auto depth = MakeImage(1, 1, TextureFormat::R32_FLOAT);
        Upload(depth, {z});
        ExpectClear(Read(Spatial(depth).Spatial));
    }
}

TEST_F(GtaoComputeTest, GeometryOutsideWorldRadiusDoesNotOcclude)
{
    SetCamera(32, 32);
    auto depth = MakeImage(32, 32, TextureFormat::R32_FLOAT);
    std::vector<float> data(32 * 32, Raw(10));
    for (uint32_t y = 0; y < 32; ++y)
        for (uint32_t x = 0; x < 16; ++x)
            data[y * 32 + x] = Raw(3);
    Upload(depth, data);
    ExpectClear(Read(Spatial(depth, GetGtaoQualitySettings(GtaoQuality::High), 0.5f).Spatial));
}

TEST_F(GtaoComputeTest, NearbyOccluderDarkensAndBendsAway)
{
    SetCamera(64, 64, true);
    auto depth = MakeImage(64, 64, TextureFormat::R32_FLOAT);
    std::vector<float> data(64 * 64, Raw(5));
    for (uint32_t y = 0; y < 64; ++y)
        for (uint32_t x = 0; x < 32; ++x)
            data[y * 64 + x] = Raw(4.5f);
    Upload(depth, data);
    auto frame = Spatial(depth, GetGtaoQualitySettings(GtaoQuality::Ultra), 2.0f);
    auto ao = Read(frame.Spatial);
    float visibility = 0.0f, bentX = 0.0f;
    for (uint32_t y = 20; y < 44; ++y)
    {
        size_t pixel = (y * 64 + 34) * 4;
        visibility += ao[pixel + 3];
        bentX += ao[pixel];
        EXPECT_LT(ao[pixel + 2], 0.0f);
    }
    EXPECT_LT(visibility / 24.0f, 0.95f);
    EXPECT_GT(bentX / 24.0f, 0.02f);
}

// Occlusion AMOUNT must not depend on the preset: a quality tier buys resolution
// and slice directions, never a different picture. The residual is the
// half-resolution sweep's own error, which has no consistent sign across
// fixtures; a systematic tier bias would show the same sign on all of them.
// Fixtures carry features several pixels wide so the half-resolution and
// full-resolution sweeps can both represent them.
// Set GE_GTAO_CALIBRATION_PROBE=1 to print the resolution x directions x taps
// grid this tolerance was derived from.
TEST_F(GtaoComputeTest, PresetsAgreeOnOcclusionAmount)
{
    constexpr double kPresetOcclusionTolerance = 0.10;
    constexpr uint32_t kSize = 64;
    constexpr uint32_t kBorder = 6;
    SetCamera(kSize, kSize, true);
    auto meanOcclusion = [&](Image* depth, GtaoQualitySettings q)
    {
        auto ao = Read(Spatial(depth, q, 2.0f).Spatial);
        double sum = 0.0;
        size_t count = 0;
        for (uint32_t y = kBorder; y + kBorder < kSize; ++y)
            for (uint32_t x = kBorder; x + kBorder < kSize; ++x)
            {
                sum += 1.0 - ao[(y * kSize + x) * 4 + 3];
                ++count;
            }
        return sum / double(count);
    };
    struct Fixture { const char* Name; std::vector<float> Depth; };
    std::vector<Fixture> fixtures;
    {
        std::vector<float> step(kSize * kSize, Raw(5.0f));
        for (uint32_t y = 0; y < kSize; ++y)
            for (uint32_t x = 0; x < kSize / 2; ++x)
                step[y * kSize + x] = Raw(4.5f);
        fixtures.push_back({"step", std::move(step)});
        std::vector<float> dome(kSize * kSize, Raw(6.0f));
        for (uint32_t y = 0; y < kSize; ++y)
            for (uint32_t x = 0; x < kSize; ++x)
            {
                const float px = (float(x) + 0.5f) / kSize * 4 - 2;
                const float py = (float(y) + 0.5f) / kSize * 4 - 2;
                const float r2 = px * px + py * py;
                if (r2 < 0.75f * 0.75f)
                    dome[y * kSize + x] = Raw(5.2f - std::sqrt(0.75f * 0.75f - r2));
            }
        fixtures.push_back({"dome", std::move(dome)});
        std::vector<float> trench(kSize * kSize, Raw(5.0f));
        for (uint32_t y = kSize / 2 - 8; y < kSize / 2 + 8; ++y)
            for (uint32_t x = 0; x < kSize; ++x)
                trench[y * kSize + x] = Raw(5.6f);
        fixtures.push_back({"trench", std::move(trench)});
    }
    const GtaoQualitySettings presets[3] = {GetGtaoQualitySettings(GtaoQuality::Medium),
                                            GetGtaoQualitySettings(GtaoQuality::High),
                                            GetGtaoQualitySettings(GtaoQuality::Ultra)};
    for (auto& fixture : fixtures)
    {
        SCOPED_TRACE(fixture.Name);
        auto depth = MakeImage(kSize, kSize, TextureFormat::R32_FLOAT);
        Upload(depth, fixture.Depth);
        const double medium = meanOcclusion(depth, presets[0]);
        const double high = meanOcclusion(depth, presets[1]);
        const double ultra = meanOcclusion(depth, presets[2]);
        RecordProperty(std::string(fixture.Name) + "_medium", std::to_string(medium));
        RecordProperty(std::string(fixture.Name) + "_high", std::to_string(high));
        RecordProperty(std::string(fixture.Name) + "_ultra", std::to_string(ultra));
        if (const char* probe = std::getenv("GE_GTAO_CALIBRATION_PROBE"); probe && std::string(probe) == "1")
        {
            std::printf("CALIB %-8s medium=%.5f high=%.5f ultra=%.5f  high/med=%.3f ultra/med=%.3f\n",
                        fixture.Name, medium, high, ultra, high / medium, ultra / medium);
            for (uint32_t divisor : {1u, 2u})
                for (uint32_t directions : {5u, 8u})
                    for (uint32_t steps : {2u, 3u, 4u, 6u, 12u, 24u})
                    {
                        SweepSteps = steps;
                        const double probed = meanOcclusion(depth, GtaoQualitySettings{divisor, directions, 5});
                        SweepSteps = kGtaoSweepSteps;
                        std::printf("PROBE %-8s div=%u dir=%u steps=%u occ=%.5f\n", fixture.Name, divisor,
                                    directions, steps, probed);
                    }
            std::fflush(stdout);
        }
        ASSERT_GT(medium, 0.01) << "fixture produces no occlusion to compare";
        EXPECT_NEAR(high, medium, kPresetOcclusionTolerance * medium);
        EXPECT_NEAR(ultra, medium, kPresetOcclusionTolerance * medium);
    }
}

TEST_F(GtaoComputeTest, MipsExcludeSkyAndSeparateDepthLayersAtOddEdges)
{
    auto source = MakeImage(5, 3, TextureFormat::R32_FLOAT);
    std::vector<float> depths(15, 0.0f);
    depths[0] = 2.0f;
    depths[1] = 20.0f;
    depths[14] = 7.0f;
    Upload(source, depths);
    auto output = MakeImage(2, 1, TextureFormat::R32_FLOAT);
    Run("gtao_depth_mip", 2, 1, {}, {{"uDepthIn", source}, {"uDepthOut", output}});
    const auto reduced = Read(output);
    ASSERT_EQ(reduced.size(), 2u);
    EXPECT_NEAR(reduced[0], 2.0f, 1e-5f);
    EXPECT_NEAR(reduced[1], 7.0f, 1e-5f);
}

// A half-res tap that sits on the other side of a silhouette carries both that
// surface's visibility and its bent normal. Blending it into a background pixel
// is what a one-pixel rim along every wire and wing edge is made of, and a bent
// normal from the wrong surface can leave the pixel BRIGHTER than with AO off.
TEST_F(GtaoComputeTest, UpsampleKeepsSilhouettesOnTheirOwnSide)
{
    constexpr uint32_t kSize = 16;
    constexpr uint32_t kHalf = kSize / 2;
    constexpr float kBackground = 50.0f;
    // 0.8 % nearer: inside the plane tolerance a relative depth test alone allows.
    constexpr float kBar = 49.6f;
    constexpr float kBarVisibility = 0.2f;
    SetCamera(kSize, kSize);
    auto depth = MakeImage(kSize, kSize, TextureFormat::R32_FLOAT);
    std::vector<float> depths(kSize * kSize, Raw(kBackground));
    for (uint32_t y = 0; y < kSize; ++y)
        for (uint32_t x = 6; x < 8; ++x) // exactly one half-resolution column
            depths[y * kSize + x] = Raw(kBar);
    Upload(depth, depths);
    auto guide = MakeImage(kHalf, kHalf, TextureFormat::R32G32_FLOAT);
    auto linear = MakeImage(kSize, kSize, TextureFormat::R32_FLOAT);
    Run("gtao_prepare", kSize, kSize, {{"uDepth", depth}}, {{"uGuide", guide}, {"uDepthOut", linear}});
    auto ao = MakeImage(kHalf, kHalf, TextureFormat::R16G16B16A16_FLOAT);
    std::vector<float> halfRes(kHalf * kHalf * 4);
    for (uint32_t y = 0; y < kHalf; ++y)
        for (uint32_t x = 0; x < kHalf; ++x)
        {
            float* texel = &halfRes[(y * kHalf + x) * 4];
            const bool onBar = x == 3;
            texel[0] = onBar ? 1.0f : 0.0f;
            texel[1] = 0.0f;
            texel[2] = onBar ? 0.0f : -1.0f;
            texel[3] = onBar ? kBarVisibility : 1.0f;
        }
    Upload(ao, halfRes);
    auto output = MakeImage(kSize, kSize, TextureFormat::R16G16B16A16_FLOAT);
    Run("gtao_upsample", kSize, kSize, {{"uDepth", depth}, {"uGuide", guide}, {"uAOIn", ao}}, {{"uAOOut", output}});
    const auto result = Read(output);
    for (uint32_t y = 2; y + 2 < kSize; ++y)
    {
        for (uint32_t x : {5u, 8u}) // background columns touching the bar
        {
            const float* texel = &result[(y * kSize + x) * 4];
            EXPECT_NEAR(texel[3], 1.0f, 1e-2f) << "background leak at " << x << "," << y;
            EXPECT_NEAR(texel[0], 0.0f, 1e-2f) << "bent normal leak at " << x << "," << y;
        }
        for (uint32_t x : {6u, 7u}) // the bar keeps its own estimate
        {
            const float* texel = &result[(y * kSize + x) * 4];
            EXPECT_NEAR(texel[3], kBarVisibility, 1e-2f) << "bar lost its estimate at " << x << "," << y;
        }
    }
}

TEST_F(GtaoComputeTest, UpsampleRejectsEveryUnrelatedGuide)
{
    SetCamera(8, 8);
    auto depth = MakeImage(8, 8, TextureFormat::R32_FLOAT);
    Upload(depth, std::vector<float>(64, Raw(5)));
    auto guide = MakeImage(4, 4, TextureFormat::R32G32_FLOAT);
    std::vector<float> guidance(32, 0.0f);
    for (size_t i = 0; i < 32; i += 2)
        guidance[i] = Raw(2);
    Upload(guide, guidance);
    auto ao = MakeImage(4, 4, TextureFormat::R16G16B16A16_FLOAT);
    Upload(ao, std::vector<float>(64, 0.0f));
    auto output = MakeImage(8, 8, TextureFormat::R16G16B16A16_FLOAT);
    Run("gtao_upsample", 8, 8, {{"uDepth", depth}, {"uGuide", guide}, {"uAOIn", ao}}, {{"uAOOut", output}});
    ExpectClear(Read(output));
}

TEST_F(GtaoComputeTest, TemporalRejectsStaleDepthAndResetHistory)
{
    SetCamera(8, 8);
    auto depth = MakeImage(8, 8, TextureFormat::R32_FLOAT);
    Upload(depth, std::vector<float>(64, Raw(5)));
    auto current = MakeImage(8, 8, TextureFormat::R16G16B16A16_FLOAT);
    std::vector<float> currentPixels(256, 0.0f);
    for (size_t i = 0; i < 256; i += 4)
    {
        currentPixels[i + 2] = -1;
        currentPixels[i + 3] = 1;
    }
    Upload(current, currentPixels);
    auto history = MakeImage(8, 8, TextureFormat::R16G16B16A16_FLOAT);
    Upload(history, std::vector<float>(256, 0.0f));
    auto surface = MakeImage(8, 8, TextureFormat::R32G32_FLOAT);
    Upload(surface, std::vector<float>(128, 0.0f));
    auto output = MakeImage(8, 8, TextureFormat::R16G16B16A16_FLOAT);
    auto surfaceOut = MakeImage(8, 8, TextureFormat::R32G32_FLOAT);
    for (uint32_t valid : {0u, 1u})
    {
        Run("gtao_temporal", 8, 8, {{"uDepth", depth}, {"uCurrent", current}, {"uHistory", history}, {"uHistorySurface", surface}},
            {{"uAOOut", output}, {"uSurfaceOut", surfaceOut}}, GetGtaoQualitySettings(GtaoQuality::Medium), 2.0f, 0, valid);
        ExpectClear(Read(output));
    }
    auto stored = Read(surfaceOut);
    for (size_t i = 0; i < stored.size(); i += 2)
        EXPECT_FLOAT_EQ(stored[i], Raw(5));
}
TEST_F(GtaoComputeTest, TemporalBlendsMatchingSurfaceAndRejectsDepthNormalAndOffscreenHistory)
{
    SetCamera(8, 8);
    auto depth = MakeImage(8, 8, TextureFormat::R32_FLOAT);
    Upload(depth, std::vector<float>(64, Raw(5)));
    auto current = MakeImage(8, 8, TextureFormat::R16G16B16A16_FLOAT);
    auto history = MakeImage(8, 8, TextureFormat::R16G16B16A16_FLOAT);
    auto surface = MakeImage(8, 8, TextureFormat::R32G32_FLOAT);
    auto output = MakeImage(8, 8, TextureFormat::R16G16B16A16_FLOAT);
    auto surfaceOut = MakeImage(8, 8, TextureFormat::R32G32_FLOAT);
    std::vector<float> now(256, 0.0f), old(256, 0.0f), guide(128, 0.0f);
    for (size_t i = 0; i < 64; ++i)
    {
        now[i * 4 + 2] = old[i * 4 + 2] = -1.0f;
        now[i * 4 + 3] = ((i / 8 + i % 8) & 1u) ? 0.55f : 0.45f;
        old[i * 4 + 3] = 0.5f;
        guide[i * 2] = Raw(5);
        guide[i * 2 + 1] = 1048575.0f; // packed (0,0,-1)
    }
    Upload(current, now);
    Upload(history, old);
    Upload(surface, guide);
    auto dispatch = [&](uint32_t valid)
    {
        Run("gtao_temporal", 8, 8, {{"uDepth", depth}, {"uCurrent", current}, {"uHistory", history}, {"uHistorySurface", surface}},
            {{"uAOOut", output}, {"uSurfaceOut", surfaceOut}}, GetGtaoQualitySettings(GtaoQuality::Medium), 2.0f, 0, valid);
        return Read(output);
    };
    const size_t pixel = (3 * 8 + 3) * 4 + 3;
    auto blended = dispatch(1);
    EXPECT_GT(blended[pixel], 0.47f);
    EXPECT_LT(blended[pixel], 0.5f);
    EXPECT_NEAR(dispatch(0)[pixel], 0.45f, 0.001f);
    for (size_t i = 0; i < 64; ++i)
        guide[i * 2] = Raw(2);
    Upload(surface, guide);
    EXPECT_NEAR(dispatch(1)[pixel], 0.45f, 0.001f);
    for (size_t i = 0; i < 64; ++i)
    {
        guide[i * 2] = Raw(5);
        guide[i * 2 + 1] = 524800.0f;
    } // (0,0,+1)
    Upload(surface, guide);
    EXPECT_NEAR(dispatch(1)[pixel], 0.45f, 0.001f);
    for (size_t i = 0; i < 64; ++i)
        guide[i * 2 + 1] = 1048575.0f;
    Upload(surface, guide);
    Params.ge_prevViewProj[12] = 20.0f;
    EXPECT_NEAR(dispatch(1)[pixel], 0.45f, 0.001f);
}

// History lives at sweep resolution, so a reprojected full-resolution UV has to be
// scaled by the HISTORY extent. An odd viewport is not twice the half-resolution
// extent - 3067 px is 1534 half-res texels, not 1533.5 - so deriving the history
// texel from the full-resolution extent slides the read by up to half a texel,
// worst at the right and bottom edges, and a still camera then accumulates a
// neighbour's history every frame.
TEST_F(GtaoComputeTest, TemporalReadsHistoryAtItsOwnTexelOnOddViewports)
{
    constexpr uint32_t kWidth = 31, kHeight = 17;      // odd: half-res is 16 x 9
    constexpr uint32_t kHalfWidth = 16, kHalfHeight = 9;
    constexpr uint32_t kMarkedColumn = 8;
    constexpr float kPackedTowardCamera = 1048575.0f;  // octahedral (0, 0, -1)
    SetCamera(kWidth, kHeight);
    auto depth = MakeImage(kWidth, kHeight, TextureFormat::R32_FLOAT);
    Upload(depth, std::vector<float>(kWidth * kHeight, Raw(5)));
    auto current = MakeImage(kHalfWidth, kHalfHeight, TextureFormat::R16G16B16A16_FLOAT);
    auto history = MakeImage(kHalfWidth, kHalfHeight, TextureFormat::R16G16B16A16_FLOAT);
    auto surface = MakeImage(kHalfWidth, kHalfHeight, TextureFormat::R32G32_FLOAT);
    auto output = MakeImage(kHalfWidth, kHalfHeight, TextureFormat::R16G16B16A16_FLOAT);
    auto surfaceOut = MakeImage(kHalfWidth, kHalfHeight, TextureFormat::R32G32_FLOAT);
    const size_t texels = size_t(kHalfWidth) * kHalfHeight;
    std::vector<float> now(texels * 4, 0.0f), old(texels * 4, 0.0f), stored(texels * 2, 0.0f);
    for (uint32_t y = 0; y < kHalfHeight; ++y)
        for (uint32_t x = 0; x < kHalfWidth; ++x)
        {
            const size_t texel = y * kHalfWidth + x;
            now[texel * 4 + 2] = -1.0f;
            now[texel * 4 + 3] = 0.5f;
            // One column of history is tilted hard in x; every other column shares
            // the current frame's direction, so any x in the result names the
            // column the history came from.
            old[texel * 4 + 0] = x == kMarkedColumn ? 0.8f : 0.0f;
            old[texel * 4 + 2] = x == kMarkedColumn ? -0.6f : -1.0f;
            old[texel * 4 + 3] = 0.5f;
            stored[texel * 2] = Raw(5);
            stored[texel * 2 + 1] = kPackedTowardCamera;
        }
    Upload(current, now);
    Upload(history, old);
    Upload(surface, stored);
    Run("gtao_temporal", kHalfWidth, kHalfHeight,
        {{"uDepth", depth}, {"uCurrent", current}, {"uHistory", history}, {"uHistorySurface", surface}},
        {{"uAOOut", output}, {"uSurfaceOut", surfaceOut}}, GetGtaoQualitySettings(GtaoQuality::Medium), 2.0f, 0, 1);
    const auto result = Read(output);
    for (uint32_t y = 1; y + 1 < kHalfHeight; ++y)
    {
        EXPECT_GT(result[(y * kHalfWidth + kMarkedColumn) * 4], 0.05f)
            << "the marked column did not read its own history at row " << y;
        EXPECT_NEAR(result[(y * kHalfWidth + kMarkedColumn + 1) * 4], 0.0f, 0.02f)
            << "history slid across a texel boundary at row " << y;
        EXPECT_NEAR(result[(y * kHalfWidth + kMarkedColumn - 1) * 4], 0.0f, 0.02f)
            << "history slid across a texel boundary at row " << y;
    }
}

TEST_F(GtaoComputeTest, TemporalReducesStaticSamplingVariance)
{
    SetCamera(64, 64, true);
    auto depth = MakeImage(64, 64, TextureFormat::R32_FLOAT);
    std::vector<float> data(64 * 64, Raw(6));
    for (uint32_t y = 0; y < 64; ++y)
        for (uint32_t x = 0; x < 64; ++x)
        {
            float px = (float(x) + 0.5f) / 64 * 4 - 2;
            float py = (float(y) + 0.5f) / 64 * 4 - 2;
            float r2 = px * px + py * py;
            if (r2 < 0.75f * 0.75f)
                data[y * 64 + x] = Raw(5.2f - std::sqrt(0.75f * 0.75f - r2));
        }
    Upload(depth, data);
    Image* history[2] = {MakeImage(32, 32, TextureFormat::R16G16B16A16_FLOAT), MakeImage(32, 32, TextureFormat::R16G16B16A16_FLOAT)};
    Image* surface[2] = {MakeImage(32, 32, TextureFormat::R32G32_FLOAT), MakeImage(32, 32, TextureFormat::R32G32_FLOAT)};
    Upload(history[1], std::vector<float>(32 * 32 * 4, 0.0f));
    Upload(surface[1], std::vector<float>(32 * 32 * 2, 0.0f));
    std::vector<double> spatialSum(1024), spatialSquares(1024), temporalSum(1024), temporalSquares(1024);
    for (uint32_t frame = 0; frame < 16; ++frame)
    {
        uint32_t write = frame & 1u, read = write ^ 1u;
        auto evaluation = Spatial(depth, GetGtaoQualitySettings(GtaoQuality::Medium), 2.0f, frame & 7u);
        auto spatial = evaluation.Filtered;
        Run("gtao_temporal", 32, 32, {{"uGuide", evaluation.Guide}, {"uDepth", depth}, {"uCurrent", spatial}, {"uHistory", history[read]}, {"uHistorySurface", surface[read]}},
            {{"uAOOut", history[write]}, {"uSurfaceOut", surface[write]}}, GetGtaoQualitySettings(GtaoQuality::Medium), 2.0f, 0, frame > 0 ? 1u : 0u);
        if (frame < 8)
            continue;
        auto current = Read(spatial), accumulated = Read(history[write]);
        for (size_t i = 0; i < 1024; ++i)
        {
            double a = current[i * 4 + 3], b = accumulated[i * 4 + 3];
            spatialSum[i] += a;
            spatialSquares[i] += a * a;
            temporalSum[i] += b;
            temporalSquares[i] += b * b;
        }
    }
    double spatialVariance = 0, temporalVariance = 0;
    for (size_t i = 0; i < 1024; ++i)
    {
        spatialVariance += spatialSquares[i] / 8 - std::pow(spatialSum[i] / 8, 2);
        temporalVariance += temporalSquares[i] / 8 - std::pow(temporalSum[i] / 8, 2);
    }
    RecordProperty("spatial_variance", spatialVariance / 1024);
    RecordProperty("temporal_variance", temporalVariance / 1024);
    EXPECT_GT(spatialVariance, 1e-6);
    EXPECT_LT(temporalVariance, spatialVariance * 0.95);
}

TEST_F(GtaoComputeTest, ProductionNodeExecutesBothResolutionPathsAndKeepsViewHistoriesSeparate)
{
    namespace P = GameEngine::Engine::Renderer::Pipeline;
    RenderServices services;
    ASSERT_TRUE(services.Initialize(Device.get()));
    {
        const auto cameraId = services.Views().AllocateCamera("GtaoTestCamera");
        CameraData camera{};
        std::memcpy(camera.view, Params.ge_view, 64);
        std::memcpy(camera.proj, Params.ge_proj, 64);
        std::memcpy(camera.viewProj, Params.ge_viewProj, 64);
        services.Views().SetCameraData(cameraId, camera);
        const auto viewA = services.Views().AllocateView("GtaoTestA", cameraId);
        const auto viewB = services.Views().AllocateView("GtaoTestB", cameraId);
        PostProcessSettings settings{};
        settings.AOIntensity = 1.0f;
        for (auto view : {viewA, viewB})
        {
            services.Views().SetViewRenderLayerMask(view, 1u);
            services.Views().SetViewPostProcessOverride(view, settings);
        }
        P::RenderPipelineNodeRegistry registry;
        ASSERT_TRUE(registry.Register("ViewParamsUpload", []
                                      { return std::make_unique<P::Nodes::ViewParamsUploadNode>(); }, true));
        ASSERT_TRUE(registry.Register("AmbientOcclusion", []
                                      { return std::make_unique<P::Nodes::AONode>(); }, true));
        P::RenderPipelineBlueprint blueprint;
        blueprint.pipelineName = "GtaoTest";
        P::RenderPipelineBlueprint::Pass upload;
        upload.id = "Params";
        upload.type = "ViewParamsUpload";
        upload.enabled = upload.perView = true;
        upload.passJson = R"({"buffer":"ViewParams"})";
        P::RenderPipelineBlueprint::Pass ao;
        ao.id = "AO";
        ao.type = "AmbientOcclusion";
        ao.enabled = ao.perView = true;
        ao.passJson = R"({"quality":"medium","temporal":true})";
        blueprint.passes = {upload, ao};
        blueprint.outputs.push_back({"AO", "View.GTAO"});
        P::RenderPipelineInstance pipeline(services, registry);
        pipeline.SetBlueprint(blueprint);
        RenderGraph::RGResourcePool persistent(Device.get());
        RenderGraph::RGTransientPool transient(Device.get());
        RenderGraph::RGUploadRing ring(Device.get(), 2, 262144);
        RenderGraph::RGFrame frame(Device.get(), &persistent, &transient, &ring);
        auto depth = MakeImage(32, 32, TextureFormat::R32_FLOAT);
        Upload(depth, std::vector<float>(1024, Raw(5)));
        auto color = MakeImage(32, 32, TextureFormat::R16G16B16A16_FLOAT);
        for (uint32_t index : {0u, 1u, 3u, 4u})
        {
            if (index == 4)
            {
                blueprint.passes[1].passJson = R"({"quality":"high","temporal":false})";
                pipeline.SetBlueprint(blueprint);
            }
            frame.BeginFrame(index);
            const auto d = frame.ImportExternalTexture("Depth", depth->Handle, depth->States[0], TextureFormat::R32_FLOAT);
            const auto c = frame.ImportExternalTexture("Color", color->Handle, color->States[0], TextureFormat::R16G16B16A16_FLOAT);
            std::vector<P::ViewTargetsRG> targets{{viewA, c, d, {}}, {viewB, c, d, {}}};
            auto views = services.Views().GetViews();
            pipeline.Declare(frame, targets, views);
            const auto a = pipeline.GetOutputRG(viewA, "AO"), b = pipeline.GetOutputRG(viewB, "AO");
            ASSERT_TRUE(a.IsValid());
            ASSERT_TRUE(b.IsValid());
            EXPECT_NE(a.Id, b.Id);
            frame.MarkOutput(a);
            frame.MarkOutput(b);
            size_t upsample = 0, temporal = 0, mipPasses = 0;
            for (uint32_t pass = 0; pass < frame.Graph().PassCount(); ++pass)
            {
                const std::string name = frame.Graph().PassName(pass);
                upsample += name.find("GTAOUpsample") != std::string::npos;
                temporal += name.find("GTAOTemporal") != std::string::npos;
                mipPasses += name.find("GTAODepthMip") != std::string::npos;
            }
            EXPECT_EQ(upsample, index == 4 ? 0u : 2u);
            EXPECT_EQ(temporal, index == 4 ? 0u : 2u);
            EXPECT_EQ(mipPasses, 10u);
            frame.Execute();
            Device->WaitForIdle();
            EXPECT_EQ(frame.Graph().LivePassCount(), frame.Graph().PassCount());
            depth->States[0] = ResourceState::ShaderResource;
        }
        settings.AOIntensity = 0.0f;
        services.Views().SetViewPostProcessOverride(viewA, settings);
        services.Views().SetViewPostProcessOverride(viewB, settings);
        frame.BeginFrame(5);
        const auto d = frame.ImportExternalTexture("Depth", depth->Handle, depth->States[0], TextureFormat::R32_FLOAT);
        const auto c = frame.ImportExternalTexture("Color", color->Handle, color->States[0], TextureFormat::R16G16B16A16_FLOAT);
        std::vector<P::ViewTargetsRG> targets{{viewA, c, d, {}}, {viewB, c, d, {}}};
        pipeline.Declare(frame, targets, services.Views().GetViews());
        EXPECT_FALSE(pipeline.GetOutputRG(viewA, "AO").IsValid());
        EXPECT_FALSE(pipeline.GetOutputRG(viewB, "AO").IsValid());
        EXPECT_EQ(frame.Graph().PassCount(), 0u);
    }
    services.Shutdown();
}

// Explicitly opt in: emits raw GPU captures and isolated-dispatch timings for
// review. Baseline shader packages are optional and supplied by the capture tool.
// What the production node decides about history, frame by frame. The editor
// cannot answer this from screenshots: a capture round trip is hundreds of
// milliseconds, which is tens of frames, and an exponential history has long
// converged by then. Here the noise phase repeats every eight frames, so frame 8
// sweeps exactly what frame 0 swept and any difference between them IS the
// history; frame 16 repeats it again after a camera jump the node must reject,
// and has to come back bit-identical to frame 0.
TEST_F(GtaoComputeTest, ProductionNodeAppliesHistoryAndDropsItOnTheFirstFrameAfterACut)
{
    namespace P = GameEngine::Engine::Renderer::Pipeline;
    constexpr uint32_t kSize = 64;
    constexpr uint32_t kNoisePeriod = 8;
    constexpr float kRadius = 2.0f;
    // Beyond CameraContinuous's max(2 * radius, 1) translation gate.
    constexpr float kCutDistance = 10.0f;
    SetCamera(kSize, kSize, true);
    auto depth = MakeImage(kSize, kSize, TextureFormat::R32_FLOAT);
    std::vector<float> data(kSize * kSize, Raw(6.0f));
    for (uint32_t y = 0; y < kSize; ++y)
        for (uint32_t x = 0; x < kSize; ++x)
        {
            const float px = (float(x) + 0.5f) / kSize * 4 - 2;
            const float py = (float(y) + 0.5f) / kSize * 4 - 2;
            const float r2 = px * px + py * py;
            if (r2 < 0.75f * 0.75f)
                data[y * kSize + x] = Raw(5.2f - std::sqrt(0.75f * 0.75f - r2));
        }
    Upload(depth, data);

    RenderServices services;
    ASSERT_TRUE(services.Initialize(Device.get()));
    {
        const auto cameraId = services.Views().AllocateCamera("GtaoCutCamera");
        auto SetCameraAt = [&](float x)
        {
            CameraData camera{};
            const glm::mat4 view = glm::translate(glm::mat4(1.0f), glm::vec3(x, 0.0f, 0.0f));
            glm::mat4 proj(1.0f);
            std::memcpy(&proj[0][0], Params.ge_proj, 64);
            const glm::mat4 viewProj = proj * view;
            std::memcpy(camera.view, &view[0][0], 64);
            std::memcpy(camera.proj, Params.ge_proj, 64);
            std::memcpy(camera.viewProj, &viewProj[0][0], 64);
            std::memcpy(camera.viewRel, &view[0][0], 64);
            std::memcpy(camera.viewProjRel, &viewProj[0][0], 64);
            camera.cameraPos[0] = x;
            services.Views().SetCameraData(cameraId, camera);
        };
        SetCameraAt(0.0f);
        const auto view = services.Views().AllocateView("GtaoCutView", cameraId);
        services.Views().SetViewRenderLayerMask(view, 1u);
        PostProcessSettings settings{};
        settings.AOIntensity = 1.0f;
        settings.AORadius = kRadius;
        services.Views().SetViewPostProcessOverride(view, settings);

        P::RenderPipelineNodeRegistry registry;
        ASSERT_TRUE(registry.Register("ViewParamsUpload", []
                                      { return std::make_unique<P::Nodes::ViewParamsUploadNode>(); }, true));
        ASSERT_TRUE(registry.Register("AmbientOcclusion", []
                                      { return std::make_unique<P::Nodes::AONode>(); }, true));
        P::RenderPipelineBlueprint blueprint;
        blueprint.pipelineName = "GtaoCut";
        P::RenderPipelineBlueprint::Pass upload;
        upload.id = "Params";
        upload.type = "ViewParamsUpload";
        upload.enabled = upload.perView = true;
        upload.passJson = R"({"buffer":"ViewParams"})";
        P::RenderPipelineBlueprint::Pass ao;
        ao.id = "AO";
        ao.type = "AmbientOcclusion";
        ao.enabled = ao.perView = true;
        ao.passJson = R"({"quality":"medium","temporal":true})";
        blueprint.passes = {upload, ao};
        blueprint.outputs.push_back({"AO", "View.GTAO"});
        P::RenderPipelineInstance pipeline(services, registry);
        pipeline.SetBlueprint(blueprint);

        RenderGraph::RGResourcePool persistent(Device.get());
        RenderGraph::RGTransientPool transient(Device.get());
        RenderGraph::RGUploadRing ring(Device.get(), 2, 262144);
        RenderGraph::RGFrame frame(Device.get(), &persistent, &transient, &ring);
        auto color = MakeImage(kSize, kSize, TextureFormat::R16G16B16A16_FLOAT);
        std::vector<float> first, accumulated, afterCut;
        for (uint32_t index = 0; index <= 2 * kNoisePeriod; ++index)
        {
            // Away for the middle stretch, back for the last frame: the return is a
            // jump the node has to reject, and it restores frame 0's exact inputs.
            SetCameraAt(index > kNoisePeriod && index < 2 * kNoisePeriod ? kCutDistance : 0.0f);
            frame.BeginFrame(index);
            const auto d = frame.ImportExternalTexture("Depth", depth->Handle, depth->States[0], TextureFormat::R32_FLOAT);
            const auto c = frame.ImportExternalTexture("Color", color->Handle, color->States[0], TextureFormat::R16G16B16A16_FLOAT);
            std::vector<P::ViewTargetsRG> targets{{view, c, d, {}}};
            pipeline.Declare(frame, targets, services.Views().GetViews());
            const auto out = pipeline.GetOutputRG(view, "AO");
            ASSERT_TRUE(out.IsValid()) << "frame " << index;
            frame.MarkOutput(out);
            const auto texture = frame.PhysicalTexture(out);
            frame.Execute();
            Device->WaitForIdle();
            depth->States[0] = ResourceState::ShaderResource;
            ASSERT_TRUE(texture.IsValid()) << "frame " << index;
            if (index == 0)
                first = ReadPooled(texture, kSize, kSize);
            else if (index == kNoisePeriod)
                accumulated = ReadPooled(texture, kSize, kSize);
            else if (index == 2 * kNoisePeriod)
                afterCut = ReadPooled(texture, kSize, kSize);
        }
        ASSERT_EQ(first.size(), accumulated.size());
        ASSERT_EQ(first.size(), afterCut.size());
        size_t blended = 0, differsAfterCut = 0, occluded = 0;
        for (size_t i = 3; i < first.size(); i += 4)
        {
            occluded += first[i] < 0.999f;
            blended += std::abs(first[i] - accumulated[i]) > 1e-3f;
            differsAfterCut += first[i] != afterCut[i];
        }
        const size_t pixels = first.size() / 4;
        RecordProperty("occluded_pixels", std::to_string(occluded));
        RecordProperty("history_blended_pixels", std::to_string(blended));
        // The fixture has to occlude something, or nothing below discriminates.
        ASSERT_GT(occluded, pixels / 20);
        // Same noise phase, same inputs, seven frames of history in between.
        EXPECT_GT(blended, pixels / 20) << "the node never applied history";
        // The cut is rejected, so this frame is the un-accumulated sweep again.
        EXPECT_EQ(differsAfterCut, 0u) << "history survived a camera cut";
    }
    services.Shutdown();
}

TEST_F(GtaoComputeTest, DISABLED_CaptureQualityComparison)
{
    const char* directory = std::getenv("GE_GTAO_CAPTURE_DIR");
    if (!directory)
        GTEST_SKIP() << "Set GE_GTAO_CAPTURE_DIR to capture artifacts";
    const uint32_t size = 512;
    BenchmarkRepeats = 32;
    SetCamera(size, size, true);
    auto depth = MakeImage(size, size, TextureFormat::R32_FLOAT);
    std::vector<float> data(size * size, Raw(6));
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x)
        {
            float px = (float(x) + 0.5f) / size * 4 - 2;
            float py = (float(y) + 0.5f) / size * 4 - 2;
            float r2 = px * px + py * py;
            if (r2 < 0.75f * 0.75f)
                data[y * size + x] = Raw(5.2f - std::sqrt(0.75f * 0.75f - r2));
            if (x == 380 && y > 120 && y < 400)
                data[y * size + x] = Raw(5.4f);
        }
    Upload(depth, data);
    std::filesystem::create_directories(directory);
    auto save = [&](const std::string& name, const std::vector<float>& pixels)
    {
        ASSERT_FALSE(pixels.empty());
        ASSERT_TRUE(std::all_of(pixels.begin(), pixels.end(), [](float value) { return std::isfinite(value); }));
        ASSERT_TRUE(std::any_of(pixels.begin(), pixels.end(), [](float value) { return value != 0.0f; })) << "Empty GPU capture: " << name;
        std::ofstream file(std::filesystem::path(directory) / (name + ".f32"), std::ios::binary);
        file.write(reinterpret_cast<const char*>(pixels.data()), pixels.size() * sizeof(float));
    };
    save("depth", data);
    std::ofstream csv(std::filesystem::path(directory) / "timings.csv");
    csv << "quality,pass,gpu_ms\n";
    for (auto quality : {GtaoQuality::Medium, GtaoQuality::High, GtaoQuality::Ultra})
    {
        const char* label = quality == GtaoQuality::Medium ? "medium" : quality == GtaoQuality::High ? "high" : "ultra";
        for (uint32_t iteration = 0; iteration < 6; ++iteration)
        {
            Timings.clear();
            CaptureTimings = iteration > 0;
            auto frame = Spatial(depth, GetGtaoQualitySettings(quality));
            CaptureTimings = false;
            for (const auto& [pass, ms] : Timings)
                csv << label << ',' << pass << ',' << ms << '\n';
            if (iteration == 5)
                save(label, Read(frame.Spatial));
        }
    }
    // Baseline packages keep the parent commit's source/metadata separate.
    if (std::getenv("GE_GTAO_CAPTURE_BASELINE"))
    {
        for (uint32_t iteration = 0; iteration < 6; ++iteration)
        {
            Timings.clear();
            CaptureTimings = iteration > 0;
            auto raw = MakeImage(size / 2, size / 2, TextureFormat::R16G16B16A16_FLOAT);
            auto blur = MakeImage(size / 2, size / 2, TextureFormat::R16G16B16A16_FLOAT);
            auto out = MakeImage(size, size, TextureFormat::R16G16B16A16_FLOAT);
            Run("gtao_baseline", size / 2, size / 2, {{"uDepth", depth}}, {{"uAO", raw}});
            Run("gtao_blur_baseline", size / 2, size / 2, {{"uDepth", depth}, {"uAOIn", raw}}, {{"uAOOut", blur}});
            Run("gtao_upsample_baseline", size, size, {{"uDepth", depth}, {"uAOIn", blur}}, {{"uAOOut", out}});
            CaptureTimings = false;
            for (const auto& [pass, ms] : Timings)
                csv << "baseline," << pass << ',' << ms << '\n';
            if (iteration == 5)
                save("baseline", Read(out));
        }
    }
}

} // namespace
