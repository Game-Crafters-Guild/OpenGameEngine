// Execute the shipping auto-exposure shaders (auto_exposure_histogram.comp, auto_exposure_resolve.comp)
// on synthetic frames: the histogram pass's split of the frame into geometry and sky by depth, and the
// resolve's metering rule (the geometry is metered; the sky holds the exposure back only when it is a
// large share of the frame and more than two stops over the key; an all-sky frame meters the sky).
// The rule is stated in auto_exposure_resolve.comp; the expectations below restate it numerically.
#include "Engine/Rendering/Exposure.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/NamedPushConstantWriter.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "TestDeviceHelper.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{
constexpr uint32_t kBinCount = 256u;
constexpr uint32_t kSkyOffset = kBinCount;
constexpr uint32_t kHistogramWords = 2u * kBinCount;
constexpr size_t kHistogramBytes = kHistogramWords * sizeof(uint32_t);
constexpr size_t kStateBytes = 16;
constexpr float kMinLogLum = -16.0f; // auto_exposure_histogram.comp
constexpr float kLogLumRange = 30.0f;
constexpr float kKey = 0.18f;

// log2 luminance at the centre of histogram bin `bin` (1..255), as the resolve reads it.
float BinLog(uint32_t bin)
{
    return kMinLogLum + (float(bin - 1u) / float(kBinCount - 2u)) * kLogLumRange;
}

struct Program
{
    ShaderMeta Meta;
    DescriptorSetLayoutDesc Layout;
    PipelineHandle Pipeline;
};

class AutoExposureComputeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        Device = CreateVulkanDeviceFast();
        if (!Device)
            GTEST_SKIP() << "Vulkan device unavailable";
        Sampler = Device->CreateSampler(SamplerDesc::PointClamp("AutoExposure.Test"));
        Histogram = DeviceBuffer(kHistogramBytes);
        State = DeviceBuffer(kStateBytes);
    }
    void TearDown() override
    {
        if (!Device)
            return;
        Device->WaitForIdle();
        for (auto t : Textures)
            Device->DestroyTexture(t);
        for (auto b : Buffers)
            Device->DestroyBuffer(b);
        Device->Shutdown();
    }

    BufferHandle DeviceBuffer(size_t bytes)
    {
        BufferDesc d{};
        d.size = bytes;
        d.usage = static_cast<uint32_t>(BufferUsage::Storage) | static_cast<uint32_t>(BufferUsage::TransferSrc) |
                  static_cast<uint32_t>(BufferUsage::TransferDst);
        d.memoryUsage = BufferMemoryUsage::DeviceLocal;
        auto b = Device->CreateBuffer(d);
        EXPECT_TRUE(b.IsValid());
        Buffers.push_back(b);
        return b;
    }
    BufferHandle StagingWith(const void* data, size_t bytes)
    {
        BufferDesc d{};
        d.size = bytes;
        d.usage = static_cast<uint32_t>(BufferUsage::TransferSrc);
        d.memoryUsage = BufferMemoryUsage::Upload;
        auto b = Device->CreateBuffer(d);
        Buffers.push_back(b);
        void* p = Device->MapBuffer(b);
        EXPECT_NE(p, nullptr);
        if (p)
        {
            std::memcpy(p, data, bytes);
            Device->UnmapBuffer(b);
        }
        return b;
    }
    void Submit(CommandList* cl)
    {
        cl->End();
        Device->ExecuteCommandLists({cl});
        Device->WaitForIdle();
    }
    void WriteBuffer(BufferHandle dst, const void* data, size_t bytes)
    {
        auto staging = StagingWith(data, bytes);
        auto cl = Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->CopyBuffer(staging, dst, bytes);
        cl->Barrier(ResourceBarrier::CreateBufferBarrier(dst, ResourceState::CopyDest, ResourceState::UnorderedAccess));
        Submit(cl.get());
    }
    std::vector<uint32_t> ReadWords(BufferHandle src, size_t bytes)
    {
        auto readback = Device->CreateReadbackBuffer(bytes);
        Buffers.push_back(readback);
        auto cl = Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateBufferBarrier(src, ResourceState::UnorderedAccess, ResourceState::CopySource));
        cl->CopyBuffer(src, readback, bytes);
        Submit(cl.get());
        std::vector<uint32_t> words(bytes / 4);
        const void* p = Device->MapBuffer(readback);
        EXPECT_NE(p, nullptr);
        if (p)
        {
            std::memcpy(words.data(), p, bytes);
            Device->UnmapBuffer(readback);
        }
        return words;
    }
    TextureHandle Texture(uint32_t w, uint32_t h, TextureFormat format, const std::vector<float>& texels)
    {
        TextureDesc d{};
        d.width = w;
        d.height = h;
        d.depth = d.arrayLayers = d.sampleCount = d.mipLevels = 1;
        d.format = static_cast<uint32_t>(format);
        d.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
        auto t = Device->CreateTexture(d);
        EXPECT_TRUE(t.IsValid());
        Textures.push_back(t);
        auto staging = StagingWith(texels.data(), texels.size() * sizeof(float));
        auto cl = Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(t, ResourceState::Undefined, ResourceState::CopyDest));
        cl->CopyBufferToTextureSubresource(staging, t, 0, 0, w, h);
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(t, ResourceState::CopyDest, ResourceState::ShaderResource));
        Submit(cl.get());
        return t;
    }
    Program& Load(const char* name)
    {
        auto& program = Programs[name];
        if (program.Pipeline.IsValid())
            return program;
        ShaderPackage pkg{};
        std::string err;
        EXPECT_TRUE(LoadShaderPkg(std::string("Shaders/") + name + ".shaderpkg", Device->PreferredShaderSource(), pkg, &err))
            << err;
        program.Meta = std::move(pkg.meta);
        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(pkg.stageBytes.at("cs"));
        auto patch = [&](uint32_t set, DescriptorSetLayoutDesc& layout)
        { if (set == 0) program.Layout = layout; };
        MaterialHelper::ApplyShaderMetaToComputeDesc(*Device, program.Meta, cd, MaterialBuilder::MergeMode::Auto,
                                                     {true, 128}, patch, &err);
        program.Pipeline = Device->GetOrCreateComputePipeline(Device->InternComputePipeline(std::move(cd)));
        EXPECT_TRUE(program.Pipeline.IsValid()) << name << ": " << err;
        return program;
    }

    // One histogram pass over an RGBA32F colour frame and an R32F depth frame (0 = far plane).
    // Returns both histograms: [0, 256) geometry, [256, 512) sky.
    std::vector<uint32_t> RunHistogram(uint32_t w, uint32_t h, const std::vector<float>& rgba,
                                       const std::vector<float>& depth)
    {
        return RunHistogram(w, h, rgba, w, h, depth);
    }
    // The depth may be smaller than the colour (auto exposure meters after the upscale while the
    // depth stays at the internal resolution); pixels map by normalized position.
    std::vector<uint32_t> RunHistogram(uint32_t w, uint32_t h, const std::vector<float>& rgba,
                                       uint32_t depthW, uint32_t depthH, const std::vector<float>& depth)
    {
        auto colour = Texture(w, h, TextureFormat::R32G32B32A32_FLOAT, rgba);
        auto depthTexture = Texture(depthW, depthH, TextureFormat::R32_FLOAT, depth);
        const std::vector<uint32_t> zeros(kHistogramWords, 0u);
        WriteBuffer(Histogram, zeros.data(), kHistogramBytes);

        Program& program = Load("auto_exposure_histogram");
        auto cl = Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        DescriptorSetDesc dd{};
        dd.layout = program.Layout;
        dd.transient = true;
        auto ds = Device->CreateDescriptorSet(dd);
        NamedDescriptorWriter writer(Device.get(), ds, program.Meta, 0);
        writer.AddCombinedImageSampler("uHDRColor", colour, Sampler);
        writer.AddCombinedImageSampler("uSceneDepth", depthTexture, Sampler);
        writer.AddStorageBuffer("HistogramBuffer", Histogram, 0, kHistogramBytes);
        writer.Flush();
        cl->SetPipeline(program.Pipeline);
        NamedPushConstantWriter pc(program.Meta, program.Meta.PushConstants[0].Name);
        pc.Add("pixelCountX", w);
        pc.Add("pixelCountY", h);
        pc.Flush(cl.get());
        cl->BindDescriptorSet(0, ds, program.Pipeline);
        cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        Submit(cl.get());
        return ReadWords(Histogram, kHistogramBytes);
    }

    // One resolve pass over the current histogram buffer from a first-frame state (snaps to the
    // target), with a key of 0.18 and clamps wide enough never to bind. Returns the LINEAR scale.
    float RunResolve()
    {
        const uint32_t state[4] = {0u, 0u, 0u, 0u};
        WriteBuffer(State, state, kStateBytes);
        Program& program = Load("auto_exposure_resolve");
        auto cl = Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        DescriptorSetDesc dd{};
        dd.layout = program.Layout;
        dd.transient = true;
        auto ds = Device->CreateDescriptorSet(dd);
        NamedDescriptorWriter writer(Device.get(), ds, program.Meta, 0);
        writer.AddStorageBuffer("HistogramBuffer", Histogram, 0, kHistogramBytes);
        writer.AddStorageBuffer("state", State, 0, kStateBytes);
        writer.Flush();
        cl->SetPipeline(program.Pipeline);
        NamedPushConstantWriter pc(program.Meta, program.Meta.PushConstants[0].Name);
        pc.Add("deltaTime", 0.0f);
        pc.Add("speedUp", 1.0f);
        pc.Add("speedDown", 3.0f);
        pc.Add("exposureKey", kKey);
        pc.Add("minExposure", 1.0e-12f);
        pc.Add("maxExposure", 1.0e12f);
        pc.Flush(cl.get());
        cl->BindDescriptorSet(0, ds, program.Pipeline);
        cl->Dispatch(1, 1, 1);
        Submit(cl.get());
        const auto words = ReadWords(State, kStateBytes);
        float scale = 0.0f;
        std::memcpy(&scale, words.data(), sizeof(float));
        return scale;
    }

    // Resolve a histogram given as {bin, count} entries for geometry and sky. Returns the metered
    // log2 luminance: log2(key / scale).
    float MeterBins(const std::vector<std::pair<uint32_t, uint32_t>>& geometry,
                    const std::vector<std::pair<uint32_t, uint32_t>>& sky)
    {
        std::vector<uint32_t> words(kHistogramWords, 0u);
        for (const auto& [bin, count] : geometry)
            words[bin] += count;
        for (const auto& [bin, count] : sky)
            words[kSkyOffset + bin] += count;
        WriteBuffer(Histogram, words.data(), kHistogramBytes);
        const float scale = RunResolve();
        EXPECT_GT(scale, 0.0f);
        return std::log2(kKey / scale);
    }

    std::unique_ptr<IDevice> Device;
    SamplerHandle Sampler;
    BufferHandle Histogram;
    BufferHandle State;
    std::vector<BufferHandle> Buffers;
    std::vector<TextureHandle> Textures;
    std::map<std::string, Program> Programs;
};

// A geometry bin in the middle of the range (log2 luminance about -2.1); sky bins sit a whole number
// of bins above it, about 0.118 stops per bin.
constexpr uint32_t kGeometryBin = 120u;

uint32_t BinsForStops(float stops)
{
    return static_cast<uint32_t>(std::lround(stops * float(kBinCount - 2u) / kLogLumRange));
}
} // namespace

TEST_F(AutoExposureComputeTest, HistogramSplitsFarPlanePixelsIntoTheSkyHistogram)
{
    // 16x16: the top 10 rows are sky (depth 0, luminance 8), the rest geometry (depth 0.25,
    // luminance 0.5). Both are grey, so Rec.709 luminance equals the channel value.
    constexpr uint32_t w = 16, h = 16, skyRows = 10;
    std::vector<float> rgba(size_t(w) * h * 4), depth(size_t(w) * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
        {
            const bool sky = y < skyRows;
            const float lum = sky ? 8.0f : 0.5f;
            const size_t i = size_t(y) * w + x;
            rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = lum;
            rgba[i * 4 + 3] = 1.0f;
            depth[i] = sky ? 0.0f : 0.25f;
        }
    const auto words = RunHistogram(w, h, rgba, depth);
    ASSERT_EQ(words.size(), size_t(kHistogramWords));

    uint64_t geometry = 0, sky = 0;
    uint32_t geometryPeak = 0, skyPeak = 0;
    for (uint32_t b = 0; b < kBinCount; ++b)
    {
        geometry += words[b];
        sky += words[kSkyOffset + b];
        if (words[b] > words[geometryPeak])
            geometryPeak = b;
        if (words[kSkyOffset + b] > words[kSkyOffset + skyPeak])
            skyPeak = b;
    }
    EXPECT_EQ(geometry, uint64_t(w) * (h - skyRows)) << "every depth > 0 pixel is geometry";
    EXPECT_EQ(sky, uint64_t(w) * skyRows) << "every far-plane pixel is sky";
    EXPECT_NEAR(BinLog(geometryPeak), -1.0f, 30.0f / 254.0f);
    EXPECT_NEAR(BinLog(skyPeak), 3.0f, 30.0f / 254.0f);
}

TEST_F(AutoExposureComputeTest, HalfSizeDepthMapsByNormalizedPosition)
{
    // Colour 16x16, depth 8x8 (half size): the top half of the frame is sky in both. Mapping the
    // colour's texel coordinates straight into the depth would read depth rows 4..7 (geometry) for
    // colour rows 4..7 (sky) and miscount a quarter of the frame.
    constexpr uint32_t w = 16, h = 16, dw = 8, dh = 8;
    std::vector<float> rgba(size_t(w) * h * 4), depth(size_t(dw) * dh);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
        {
            const size_t i = size_t(y) * w + x;
            const float lum = y < h / 2 ? 8.0f : 0.5f;
            rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = lum;
            rgba[i * 4 + 3] = 1.0f;
        }
    for (uint32_t y = 0; y < dh; ++y)
        for (uint32_t x = 0; x < dw; ++x)
            depth[size_t(y) * dw + x] = y < dh / 2 ? 0.0f : 0.25f;
    const auto words = RunHistogram(w, h, rgba, dw, dh, depth);
    uint64_t geometry = 0, sky = 0;
    for (uint32_t b = 0; b < kBinCount; ++b)
    {
        geometry += words[b];
        sky += words[kSkyOffset + b];
    }
    EXPECT_EQ(sky, uint64_t(w) * h / 2);
    EXPECT_EQ(geometry, uint64_t(w) * h / 2);
}

TEST_F(AutoExposureComputeTest, SkyFillingMostOfTheFrameNoLongerDarkensTheScene)
{
    // The viewer's frame: 90 % sky one and a half stops brighter than the scene. Under the two-stop
    // headroom the sky does not move the meter: the scene is keyed to mid-grey.
    constexpr uint32_t w = 20, h = 20;
    const float scene = 0.25f, skyLum = 0.25f * std::exp2(1.5f);
    std::vector<float> rgba(size_t(w) * h * 4), depth(size_t(w) * h);
    for (uint32_t i = 0; i < w * h; ++i)
    {
        const bool sky = i >= w * h / 10; // first 10 % geometry
        const float lum = sky ? skyLum : scene;
        rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = lum;
        rgba[i * 4 + 3] = 1.0f;
        depth[i] = sky ? 0.0f : 0.5f;
    }
    RunHistogram(w, h, rgba, depth);
    const float metered = std::log2(kKey / RunResolve());
    EXPECT_NEAR(metered, std::log2(scene), 0.2f) << "the scene is metered, not the sky";
}

TEST_F(AutoExposureComputeTest, SmallSkyShareDoesNotPullTheMeter)
{
    // A window of daylight in a dark room: 8 % of the frame, six stops over the room. Under the 10 %
    // share threshold the sky never holds the exposure back.
    const float metered = MeterBins({{kGeometryBin, 920}}, {{kGeometryBin + BinsForStops(6.0f), 80}});
    EXPECT_NEAR(metered, BinLog(kGeometryBin), 1e-3f);
}

TEST_F(AutoExposureComputeTest, LargeBrightSkySitsTwoStopsOverTheKey)
{
    // Half the frame is sky five stops over the scene: the full pull leaves the sky's average exactly
    // two stops over the metered key.
    const uint32_t skyBin = kGeometryBin + BinsForStops(5.0f);
    const float metered = MeterBins({{kGeometryBin, 500}}, {{skyBin, 500}});
    EXPECT_NEAR(metered, BinLog(skyBin) - 2.0f, 1e-3f);
}

TEST_F(AutoExposureComputeTest, SkyPullScalesWithItsShareOfTheFrame)
{
    // 20 % sky sits halfway up the 10-30 % ramp: half the pull.
    const uint32_t skyBin = kGeometryBin + BinsForStops(5.0f);
    const float g = BinLog(kGeometryBin), s = BinLog(skyBin);
    const float metered = MeterBins({{kGeometryBin, 800}}, {{skyBin, 200}});
    EXPECT_NEAR(metered, g + 0.5f * (s - 2.0f - g), 1e-3f);
}

TEST_F(AutoExposureComputeTest, SkyWithinTheHeadroomDoesNotPull)
{
    // Half the frame is sky 1.9 stops over the scene: inside the two-stop headroom, no pull.
    const float metered = MeterBins({{kGeometryBin, 500}}, {{kGeometryBin + BinsForStops(1.9f), 500}});
    EXPECT_NEAR(metered, BinLog(kGeometryBin), 1e-3f);
}

TEST_F(AutoExposureComputeTest, AllSkyFrameMetersTheSky)
{
    // Looking up into open sky: no geometry, the sky is metered as before the split.
    const uint32_t skyBin = kGeometryBin + BinsForStops(4.0f);
    EXPECT_NEAR(MeterBins({}, {{skyBin, 1000}}), BinLog(skyBin), 1e-3f);
}

TEST_F(AutoExposureComputeTest, GeometryUnderFivePercentBlendsTowardTheSky)
{
    // 2.5 % geometry is halfway up the 0-5 % smoothstep: the geometry estimate is halfway to the sky.
    // The sky is one stop over the scene, inside the headroom, so the ceiling does not act.
    const uint32_t skyBin = kGeometryBin + BinsForStops(1.0f);
    const float g = BinLog(kGeometryBin), s = BinLog(skyBin);
    const float metered = MeterBins({{kGeometryBin, 25}}, {{skyBin, 975}});
    EXPECT_NEAR(metered, s + 0.5f * (g - s), 1e-3f);
}

TEST_F(AutoExposureComputeTest, EmptyFrameMetersTheWindowFloor)
{
    // Only near-black pixels (bin 0, excluded from metering): the meter reads the window floor.
    EXPECT_NEAR(MeterBins({{0u, 1000}}, {{0u, 1000}}), kMinLogLum, 1e-3f);
}
