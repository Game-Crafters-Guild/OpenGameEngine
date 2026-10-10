#include <gtest/gtest.h>
#include "TestDeviceHelper.h"
#include "StorageImageLayoutHelper.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

using namespace GameEngine::Rendering;
namespace
{
using Pixel = std::array<float, 4>;
// Run the shipped compute kernels, with authored current/history/depth inputs.
// Native uses 8x8; TAAU uses an 8x8 raster and a 16x16 history/output grid.
class TaaResolveHistoryTests : public testing::TestWithParam<bool>
{
protected:
    std::unique_ptr<IDevice> device;
    void SetUp() override
    {
        device = CreateVulkanDeviceFast();
        if (!device) GTEST_SKIP() << "No Vulkan device";
    }
    void TearDown() override { if (device) device->Shutdown(); }

    float Resolve(float currentValue, float historyValue, float historyDepth,
                  bool mixedDepth = false, bool isolatedHistory = false, bool debugRejection = false, float surfaceDepth = .5f,
                  std::vector<Pixel>* feedback = nullptr, int phase = 0,
                  bool sceneMoving = false, bool currentContrast = false,
                  bool stableScene = false, bool thinOverGround = false, bool varyingHistory = false)
    {
        const uint32_t outputSize = GetParam() ? 16u : 8u;
        const float jitterY = feedback && phase >= 0 ? ((phase % 8) - 3.5f) / 8.f : 0.f;
        ShaderPackage pkg{};
        std::string error;
        const char* path = GetParam() ? "Shaders/taa_resolve_upscale.shaderpkg" : "Shaders/taa_resolve.shaderpkg";
        if (!LoadShaderPkg(path, device->PreferredShaderSource(), pkg, &error))
        {
            ADD_FAILURE() << path << ": " << error;
            return NAN;
        }
        DescriptorSetLayoutDesc layout{};
        for (uint32_t i = 0; i < 7; ++i)
        {
            DescriptorBinding b{};
            b.binding = i;
            b.count = 1;
            b.shaderStages = kShaderStageCompute;
            b.type = i < 2 ? DescriptorType::StorageImage : i < 6 ? DescriptorType::CombinedImageSampler : DescriptorType::UniformBuffer;
            layout.bindings.push_back(b);
        }
        ComputePipelineDesc pd{};
        pd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(pkg.stageBytes.at("cs"));
        pd.DescriptorSetLayouts.push_back(device->InternDescriptorSetLayout(layout));
        auto pipeline = device->GetOrCreateComputePipeline(device->InternComputePipeline(pd));
        EXPECT_TRUE(pipeline.IsValid());
        DescriptorSetDesc sd{};
        sd.layout = layout;
        sd.transient = true;
        auto set = device->CreateDescriptorSet(sd);
        auto sampler = device->CreateSampler(SamplerDesc::MaterialLinearClamp("TaaOracle"));
        std::vector<TextureHandle> textures;
        std::vector<BufferHandle> buffers;
        auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        for (uint32_t binding = 0; binding < 6; ++binding)
        {
            const uint32_t size = binding < 2 || binding == 5 ? outputSize : 8u;
            TextureDesc td{};
            td.width = td.height = size;
            td.depth = td.mipLevels = td.arrayLayers = td.sampleCount = 1;
            td.format = static_cast<uint32_t>(binding < 2 ? TextureFormat::R16G16B16A16_FLOAT : TextureFormat::R32G32B32A32_FLOAT);
            td.usage = static_cast<uint32_t>(TextureUsage::UnorderedAccess | TextureUsage::ShaderResource | TextureUsage::TransferSrc | TextureUsage::TransferDst);
            auto texture = device->CreateTexture(td);
            textures.push_back(texture);
            if (binding < 2)
            {
                TransitionForStorageAccess(cl.get(), texture, ResourceState::Undefined, 0, 1);
                device->UpdateStorageImageBinding(set, binding, texture);
                continue;
            }
            std::vector<Pixel> pixels(size * size);
            for (uint32_t y = 0; y < size; ++y)
                for (uint32_t x = 0; x < size; ++x)
                {
                    const float current = currentContrast && (x & 1u)
                        ? 2.f * historyValue - currentValue : currentValue;
                    Pixel p{current,current,current,1};
                    if (binding == 3) p = {mixedDepth ? (x < 4 ? .25f : .75f) : surfaceDepth,0,0,0};
                    if (binding == 4) p = {100,100,0,0}; // analytic stationary reprojection
                    if (binding == 5)
                    {
                        float v = isolatedHistory && (x != size / 2 || y != size / 2) ? currentValue : historyValue;
                        if (varyingHistory) v *= .5f + float(x) / float(size);
                        p = {v,v,v,historyDepth};
                    }
                    if (feedback)
                    {
                        // Analytic raster samples of one stationary silhouette
                        // across eight jitter phases, not a changing material.
                        const float sampleY = float(y) - jitterY;
                        if (binding == 2 && phase >= 0)
                        {
                            const float v = sampleY >= 3.5f && sampleY < 3.75f ? 32.f : .25f;
                            p = {v,v,v,1};
                        }
                        if (binding == 3 && phase >= 0)
                        {
                            // The steep grazing facet moves between raster rows
                            // 3 and 4; the flatter interior remains visible. Sky
                            // may sit two input texels from the evaluated pixel.
                            const float d = sampleY < 3.25f ? 0.f : sampleY < 4.f ?
                                .46f + (sampleY - 3.25f) * .05f : .4975f + (sampleY - 4.f) * .005f;
                            p = {d,0,0,0};
                            if (thinOverGround)
                                p[0] = sampleY >= 3.5f && sampleY < 3.75f ? .75f : .25f;
                        }
                        if (binding == 5 && !feedback->empty())
                            p = (*feedback)[y * size + x];
                    }
                    pixels[y * size + x] = p;
                }
            BufferDesc bd{};
            bd.size = pixels.size() * sizeof(Pixel);
            bd.usage = static_cast<uint32_t>(BufferUsage::TransferSrc);
            bd.memoryUsage = BufferMemoryUsage::Upload;
            auto upload = device->CreateBuffer(bd);
            buffers.push_back(upload);
            auto mapped = device->MapBuffer(upload);
            EXPECT_NE(mapped, nullptr);
            if (mapped) std::memcpy(mapped, pixels.data(), bd.size);
            device->UnmapBuffer(upload);
            cl->CopyBufferToTextureSubresource(upload, texture, 0, 0, size, size);
            cl->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::CopyDest, ResourceState::ShaderResource));
            device->UpdateCombinedImageSamplerBinding(set, binding, texture, sampler);
        }
        std::vector<float> params(32, 0);
        for (int i = 0; i < 16; i += 5) params[i] = params[i + 16] = 1;
        auto append = [&](std::initializer_list<float> values) { params.insert(params.end(), values); };
        append({8,8,1.f/8,1.f/8});
        if (GetParam()) append({float(outputSize),float(outputSize),1.f/outputSize,1.f/outputSize});
        append({.95f,1,debugRejection ? 2.f : 0.f,0});
        append({1.25f,.01f,0,.5f});
        append({0,jitterY / 8.f,0,sceneMoving ? 1.f : stableScene ? -1.f : 0.f});
        BufferDesc bd{};
        bd.size = params.size() * sizeof(float);
        bd.usage = static_cast<uint32_t>(BufferUsage::Uniform);
        bd.memoryUsage = BufferMemoryUsage::Upload;
        auto ubo = device->CreateBuffer(bd);
        buffers.push_back(ubo);
        auto mapped = device->MapBuffer(ubo);
        EXPECT_NE(mapped, nullptr);
        if (mapped) std::memcpy(mapped, params.data(), bd.size);
        device->UnmapBuffer(ubo);
        device->UpdateBufferBinding(set, 6, ubo, 0, bd.size);
        auto readback = device->CreateReadbackBuffer(outputSize * outputSize * 8);
        buffers.push_back(readback);
        cl->SetPipeline(pipeline);
        cl->BindDescriptorSet(0, set, pipeline);
        cl->Dispatch(outputSize / 8, outputSize / 8, 1);
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(textures[debugRejection ? 1 : 0], ResourceState::UnorderedAccess, ResourceState::CopySource));
        cl->CopyTextureSubresourceToBuffer(textures[debugRejection ? 1 : 0], 0, 0, readback, outputSize, outputSize, 0, 0, 0, 0);
        cl->End();
        device->ExecuteCommandLists({cl.get()});
        device->FinalizeFrame();
        device->WaitForIdle();
        float result = NAN;
        if (const auto* data = static_cast<const uint16_t*>(device->MapBuffer(readback)))
        {
            auto decode = [](uint16_t h)
            {
                const int exponent = (h >> 10) & 31;
                const float mantissa = float((h & 1023) + (exponent ? 1024 : 0));
                return exponent == 31 ? std::numeric_limits<float>::quiet_NaN() :
                    (h & 0x8000 ? -1.f : 1.f) * std::ldexp(mantissa, exponent ? exponent - 25 : -24);
            };
            result = decode(data[(outputSize / 2 * outputSize + outputSize / 2) * 4]);
            if (feedback)
            {
                feedback->resize(outputSize * outputSize);
                for (size_t i = 0; i < feedback->size(); ++i)
                    for (size_t c = 0; c < 4; ++c)
                        (*feedback)[i][c] = decode(data[i * 4 + c]);
            }
            device->UnmapBuffer(readback);
        }
        EXPECT_TRUE(std::isfinite(result));
        for (auto t : textures) device->DestroyTexture(t);
        for (auto b : buffers) device->DestroyBuffer(b);
        device->DestroySampler(sampler);
        return result;
    }
};
TEST_P(TaaResolveHistoryTests, RetainsBrightDetailWhenCurrentPhaseMissesIt)
{
    EXPECT_GT(Resolve(0, 1, .5f, false, true), .8f);
}
TEST_P(TaaResolveHistoryTests, RetainsDarkDetailWhenCurrentPhaseMissesIt)
{
    EXPECT_LT(Resolve(1, 0, .5f, false, true), .2f);
}
// Shadow receivers remain stationary when a caster moves elsewhere. Their
// depth and motion vectors are unchanged, but stale lighting must be clipped.
TEST_P(TaaResolveHistoryTests, ClipsOldBrightOutlineOnStationaryMovingShadowReceiver)
{
    EXPECT_NEAR(Resolve(0, 1, .5f, false, true, false, .5f, nullptr, 0, true), 0.f, .001f);
}
TEST_P(TaaResolveHistoryTests, ClipsOldDarkOutlineOnStationaryMovingShadowReceiver)
{
    EXPECT_NEAR(Resolve(1, 0, .5f, false, true, false, .5f, nullptr, 0, true), 1.f, .001f);
}
TEST_P(TaaResolveHistoryTests, AccumulatesInRangeHistoryWhileOtherObjectsMove)
{
    EXPECT_GT(Resolve(.4f, .6f, .5f, false, false, false, .5f, nullptr, 0, true, true), .53f);
}
TEST_P(TaaResolveHistoryTests, RejectsDepthBetweenButAbsentFromCurrentSurfaces)
{
    EXPECT_GT(Resolve(.25f, 1, .5f, true, true, true), .9f);
}
TEST_P(TaaResolveHistoryTests, RetainsDepthThatActuallyOccursInTheFootprint)
{
    EXPECT_GT(Resolve(.25f, 1, .25f, true, true), .8f);
}
TEST_P(TaaResolveHistoryTests, RejectsSmallDepthChangesOnFlatSurfaces)
{
    EXPECT_GT(Resolve(.25f, 1, .505f, false, true, true), .9f);
}
TEST_P(TaaResolveHistoryTests, RejectsSurfaceHistoryWhenOnlySkyRemains)
{
    EXPECT_GT(Resolve(.25f, 1, .5f, false, true, true, 0), .9f);
}
TEST_P(TaaResolveHistoryTests, HdrSkyCoverageAccumulatesWithoutDarkHistoryLock)
{
    const float appearing = Resolve(1000, 0, 0, false, true, false, 0);
    EXPECT_GT(appearing, 10); // old dark samples must not dominate HDR input
    EXPECT_LT(appearing, 200); // retain missed dark coverage instead of clipping it away
    const float missed = Resolve(0, 1000, 0, false, true, false, 0);
    EXPECT_GT(missed, 800);
    EXPECT_LT(missed, 1000);
}
TEST_P(TaaResolveHistoryTests, UniformHdrLightingChangesDoNotLockDarkHistory)
{
    EXPECT_NEAR(Resolve(1000, 0, .5f), 1000, 1);
    EXPECT_NEAR(Resolve(0, 1000, .5f), 0, .002f);
}
TEST_P(TaaResolveHistoryTests, SlopedSilhouetteAccumulatesAcrossJitterPhases)
{
    std::vector<Pixel> history;
    std::vector<float> settled;
    for (int frame = 0; frame < 96; ++frame)
    {
        // Feed the actual FP16 GPU history (color AND point depth) into the
        // next resolve. A single hand-authored history input misses this bug.
        const float result = Resolve(.25f, .25f, .5f, false, false, false, .5f, &history, frame);
        ASSERT_TRUE(std::isfinite(result));
        if (frame >= 64) settled.push_back(result);
    }
    const auto [low, high] = std::minmax_element(settled.begin(), settled.end());
    EXPECT_GT(*low, 2.f) << "retain the HDR rim even when a raster phase misses it";
    EXPECT_LT(*high - *low, 3.f) << "do not reset accumulated coverage on each depth phase";
    // Replacing the sloped foreground with a flat background must still
    // discard the bright accumulated rim immediately, not leave a ghost.
    // Use the depth of the old interior, so an unconstrained neighbor-history
    // fallback would accept it and ghost despite the rim facet being gone.
    EXPECT_NEAR(Resolve(.25f, .25f, .5f, false, false, false, .5f, &history, -1), .25f, .002f);
}
// Every sample of this subpixel strip can miss the foreground in one jitter
// phase. A finite-depth ground plane remains, with no current foreground anchor.
TEST_P(TaaResolveHistoryTests, ThinStripOverGroundAccumulatesWithoutFlashing)
{
    std::vector<Pixel> history;
    std::vector<float> settled;
    float expected = .25f;
    for (int frame = 0; frame < 96; ++frame)
    {
        const float result = Resolve(.25f, .25f, .25f, false, false, false, .25f,
                                     &history, frame, false, false, true, true);
        const float jitter = ((frame % 8) - 3.5f) / 8.f;
        const float rasterCenter = GetParam() ? 3.75f : 4.f;
        const float coverage = frame % 8 >= 6 ? 1.f - std::abs(rasterCenter + jitter - 4.f) : 0.f;
        const float current = .25f + (32.f - .25f) * coverage;
        expected = .95f * expected + .05f * current;
        if (frame >= 64)
        {
            settled.push_back(result);
            EXPECT_NEAR(result, expected, .08f) << "linear jitter-coverage integration, frame " << frame;
        }
    }
    const auto [low, high] = std::minmax_element(settled.begin(), settled.end());
    EXPECT_GT(*low, 2.f) << "integrate foreground coverage even on fully missed phases";
    EXPECT_LT(*high - *low, 3.f) << "a missed foreground must not reset its accumulated color";
    // The world's render-content epoch changes on removal. The same ground
    // sample must then clear the strip on the first frame, without a trail.
    EXPECT_NEAR(Resolve(.25f, .25f, .25f, false, false, false, .25f,
                        &history, -1, true), .25f, .002f);
}
TEST_P(TaaResolveHistoryTests, CertifiedStaticCoverageCanHaveDisjointColorNeighborhoods)
{
    // A fully missed phase sees only background. Accumulated mixed coverage
    // need not contain that pure background even in the reconstruction footprint.
    EXPECT_NEAR(Resolve(4, 1, .5f, false, false, false, .5f, nullptr, 0,
                        false, false, true, false, true), 1.15f, .01f);
    // The same color difference on a changed scene is stale lighting.
    EXPECT_NEAR(Resolve(4, 1, .5f, false, false, false, .5f, nullptr, 0,
                        true, false, false, false, true), 4.f, .002f);
}
TEST_P(TaaResolveHistoryTests, UntrackedUniformLightingChangesStillConvergeAtRest)
{
    EXPECT_NEAR(Resolve(1000, 0, .5f, false, false, false, .5f, nullptr, 0,
                        false, false, true), 1000, 1);
    EXPECT_NEAR(Resolve(0, 1000, .5f, false, false, false, .5f, nullptr, 0,
                        false, false, true), 0, .002f);
}
INSTANTIATE_TEST_SUITE_P(NativeAndUpscale, TaaResolveHistoryTests, testing::Values(false, true));
}
