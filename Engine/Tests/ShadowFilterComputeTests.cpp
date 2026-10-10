#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "TestDeviceHelper.h"
#include "Platform/Shell.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{
class ShadowFilterComputeTest : public ::testing::Test
{
  protected:
    struct Query
    {
        std::array<float, 4> UVDepthLayer{0.5f, 0.5f, 0.5f, 0.0f};
        std::array<float, 4> Bias{0.5f, 0.02f, 0.02f, 1.5f};
        std::array<float, 4> Control{3.0f, 0.0f, 0.2f, 1.0f};
        std::array<float, 4> PlaneDx{};
        std::array<float, 4> PlaneDy{};
        // Linear depth, max shadow distance, ranged (cascade) term, terrain term.
        std::array<float, 4> FadeDepth{0.0f, 1.0f, 0.0f, 0.0f};
        std::array<float, 4> FadeControl{0.1f, 0.0f, 0.0f, 0.0f};
        // Shadow-UV change per screen pixel along x, disk radius in texels, tap count.
        std::array<float, 4> FootprintDx{0.0f, 0.0f, 1.0f, 32.0f};
        // Shadow-UV change per screen pixel along y, receiver depth change per pixel along x and y.
        std::array<float, 4> FootprintDy{};
    };
    // [0..3]: filtered, reference, normal bias, receiver-plane gradient error.
    // [4]: distance fade. [5]: GE_JoinTerrainAfterFade of FadeDepth's two terms.
    // [8]: the disk radius raised to the screen-pixel floor (texels).
    // [9]: the disk kernel's visibility at that radius. [10]: 1 where the floor binds.
    using Result = std::array<float, 12>;

    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
        std::ifstream input(GameEngine::Platform::GetExecutablePath().parent_path() /
            "TestData/ShadowFilter/shadow_filter_test.spv", std::ios::binary);
        ASSERT_TRUE(input.good()) << "Shadow filter regression shader was not built";
        auto bytes = std::make_shared<const std::vector<uint8_t>>(
            std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        for (uint32_t binding = 0; binding < 3; ++binding)
        {
            DescriptorBinding b{};
            b.binding = binding;
            b.type = binding == 0 ? DescriptorType::CombinedImageSampler : DescriptorType::StorageBuffer;
            b.count = 1;
            b.shaderStages = kShaderStageCompute;
            m_Layout.bindings.push_back(b);
        }
        ComputePipelineDesc desc{};
        desc.ComputeShader = bytes;
        desc.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
        desc.DebugName = "Shadow.FilterRegression";
        m_Pipeline = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(desc));
        ASSERT_TRUE(m_Pipeline.IsValid());
        m_Sampler = m_Device->CreateSampler(SamplerDesc::ShadowComparePCF("Shadow.FilterRegression"));
        ASSERT_TRUE(m_Sampler.IsValid());
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        for (auto set : m_Sets)
            m_Device->DestroyDescriptorSet(set);
        for (auto texture : m_Textures)
            m_Device->DestroyTexture(texture);
        for (auto buffer : m_Buffers)
            m_Device->DestroyBuffer(buffer);
        if (m_Sampler.IsValid())
            m_Device->DestroySampler(m_Sampler);
    }

    BufferHandle Buffer(uint64_t size, BufferUsage usage, BufferMemoryUsage memory)
    {
        BufferDesc desc{};
        desc.size = size;
        desc.usage = static_cast<uint32_t>(usage);
        desc.memoryUsage = memory;
        desc.debugName = "Shadow.FilterRegression.Buffer";
        auto buffer = m_Device->CreateBuffer(desc);
        m_Buffers.push_back(buffer);
        return buffer;
    }

    void Dispatch(uint32_t width, uint32_t height, const std::vector<float>& depth,
                   const std::vector<Query>& queries)
    {
        ASSERT_EQ(depth.size(), width * height * 2u);
        ASSERT_FALSE(queries.empty());
        TextureDesc td{};
        td.width = width;
        td.height = height;
        td.depth = td.mipLevels = td.sampleCount = 1;
        td.arrayLayers = 2;
        td.format = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
        td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst |
                                         TextureUsage::DepthStencil);
        td.debugName = "Shadow.FilterRegression.Depth";
        const auto texture = m_Device->CreateTexture(td);
        m_Textures.push_back(texture);
        ASSERT_TRUE(texture.IsValid());
        const auto upload = Buffer(depth.size() * sizeof(float), BufferUsage::TransferSrc,
                                    BufferMemoryUsage::Upload);
        const auto parameters = Buffer(queries.size() * sizeof(Query), BufferUsage::Storage,
                                        BufferMemoryUsage::Upload);
        const auto output = Buffer(queries.size() * sizeof(Result), BufferUsage::Storage,
                                    BufferMemoryUsage::Readback);
        for (auto buffer : {upload, parameters, output})
            ASSERT_TRUE(buffer.IsValid());
        void* mapped = m_Device->MapBuffer(upload);
        ASSERT_NE(mapped, nullptr);
        std::memcpy(mapped, depth.data(), depth.size() * sizeof(float));
        m_Device->UnmapBuffer(upload);
        mapped = m_Device->MapBuffer(parameters);
        ASSERT_NE(mapped, nullptr);
        std::memcpy(mapped, queries.data(), queries.size() * sizeof(Query));
        m_Device->UnmapBuffer(parameters);

        DescriptorSetDesc dsDesc{};
        dsDesc.layout = m_Layout;
        const auto set = m_Device->CreateDescriptorSet(dsDesc);
        m_Sets.push_back(set);
        m_Device->UpdateCombinedImageSamplerBinding(set, 0, texture, m_Sampler);
        m_Device->UpdateStorageBufferBinding(set, 1, parameters, 0, queries.size() * sizeof(Query));
        m_Device->UpdateStorageBufferBinding(set, 2, output, 0,
                                              queries.size() * sizeof(Result));
        auto commands = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        commands->Begin();
        for (uint32_t layer = 0; layer < 2; ++layer)
            commands->CopyBufferToTextureSubresource(upload, texture, 0, layer, width, height,
                layer * width * height * sizeof(float));
        commands->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::CopyDest,
                                                                 ResourceState::ShaderResource));
        commands->SetPipeline(m_Pipeline);
        commands->BindDescriptorSet(0, set, m_Pipeline);
        commands->Dispatch((static_cast<uint32_t>(queries.size()) + 63) / 64, 1, 1);
        commands->End();
        m_Device->ExecuteCommandLists({commands.get()});
        m_Device->WaitForIdle();
        mapped = m_Device->MapBuffer(output);
        ASSERT_NE(mapped, nullptr);
        m_Results.resize(queries.size());
        std::memcpy(m_Results.data(), mapped, m_Results.size() * sizeof(m_Results[0]));
        m_Device->UnmapBuffer(output);
    }

    std::unique_ptr<IDevice> m_Device;
    DescriptorSetLayoutDesc m_Layout{};
    PipelineHandle m_Pipeline{};
    SamplerHandle m_Sampler{};
    std::vector<TextureHandle> m_Textures;
    std::vector<BufferHandle> m_Buffers;
    std::vector<DescriptorSetHandle> m_Sets;
    std::vector<Result> m_Results;
};

TEST_F(ShadowFilterComputeTest, NineSamplesMatchFullConvolutionAcrossPhasesLayersAndBorders)
{
    constexpr uint32_t kWidth = 17, kHeight = 13;
    std::vector<float> depths(kWidth * kHeight * 2);
    for (uint32_t layer = 0; layer < 2; ++layer)
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
                depths[(layer * kHeight + y) * kWidth + x] = layer == 0
                    ? ((x * 13 + y * 7) % 17) / 16.0f : (x < kWidth / 2 ? 0.8f : 0.0f);
    std::vector<Query> queries;
    for (float layer : {0.0f, 1.0f})
        for (float depth : {0.0f, 0.4f, 0.75f, 1.0f})
            for (uint32_t y = 0; y <= 32; ++y)
                for (uint32_t x = 0; x <= 32; ++x)
                {
                    Query q;
                    q.UVDepthLayer = {x / 32.0f, y / 32.0f, depth, layer};
                    queries.push_back(q);
                }
    Dispatch(kWidth, kHeight, depths, queries);
    ASSERT_EQ(m_Results.size(), queries.size());
    float largestError = 0.0f;
    uint32_t fractional = 0;
    for (size_t i = 0; i < m_Results.size(); ++i)
    {
        const auto& r = m_Results[i];
        ASSERT_TRUE(std::isfinite(r[0]) && std::isfinite(r[1]));
        EXPECT_GE(r[0], 0.0f);
        EXPECT_LE(r[0], 1.00001f);
        largestError = std::max(largestError, std::abs(r[0] - r[1]));
        if (r[0] > 0.01f && r[0] < 0.99f)
            ++fractional;
        if (queries[i].UVDepthLayer[2] == 1.0f)
            EXPECT_NEAR(r[0], 1.0f, 0.00001f); // reverse-Z fully-lit invariant
    }
    EXPECT_GT(fractional, 1000u); // ensure the equality was tested on edges, not just constants
    EXPECT_LT(largestError, 0.004f); // permits hardware subtexel filtering precision
    RecordProperty("maximum_filter_error", largestError);
}

TEST_F(ShadowFilterComputeTest, NormalBiasPreservesAuthoredCapsAndShrinksWithTexels)
{
    std::vector<Query> queries;
    for (float quality : {0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f})
        for (float authored : {0.0f, 0.001f, 0.5f})
            for (float texel : {0.01f, 0.02f, 0.1f})
                for (float cosine : {0.0f, 0.5f, 1.0f})
                {
                    Query q;
                    q.Bias = {authored, texel, texel, 1.5f};
                    q.Control = {quality, cosine, 0.2f, 1.0f};
                    queries.push_back(q);
                }
    Dispatch(4, 4, std::vector<float>(32, 0.0f), queries);
    ASSERT_EQ(m_Results.size(), queries.size());
    for (size_t i = 0; i < queries.size(); ++i)
    {
        const auto& q = queries[i];
        const float bias = m_Results[i][2];
        const float authored = q.Bias[0] * (1.0f - q.Control[1]);
        EXPECT_GE(bias, 0.0f);
        EXPECT_LE(bias, authored + 1e-7f);
        if (q.Bias[0] <= 0.001f)
            EXPECT_NEAR(bias, authored, 1e-7f);
        if (q.Bias[0] == 0.5f && q.Control[0] == 3.0f && q.Control[1] == 0.0f)
            EXPECT_NEAR(bias, 2.0f * q.Bias[1], 1e-7f);
    }
}

TEST_F(ShadowFilterComputeTest, BiasIsContinuousThroughCascadeTransition)
{
    std::vector<Query> queries;
    for (uint32_t i = 0; i <= 100; ++i)
    {
        const float t = i / 100.0f;
        const float weight = t * t * (3.0f - 2.0f * t);
        Query q;
        q.Bias[1] = 0.02f * (1.0f - weight) + 0.1f * weight;
        queries.push_back(q);
    }
    Dispatch(4, 4, std::vector<float>(32, 0.0f), queries);
    ASSERT_EQ(m_Results.size(), queries.size());
    EXPECT_NEAR(m_Results.front()[2], 0.04f, 1e-7f);
    EXPECT_NEAR(m_Results.back()[2], 0.2f, 1e-7f);
    for (size_t i = 1; i < m_Results.size(); ++i)
    {
        EXPECT_GE(m_Results[i][2], m_Results[i - 1][2]);
        EXPECT_LT(m_Results[i][2] - m_Results[i - 1][2], 0.0025f);
    }
}

TEST_F(ShadowFilterComputeTest, ReceiverPlaneCorrectionSurvivesResolutionAndCascadeScaling)
{
    std::vector<Query> queries;
    for (float scale : {1.0f, 0.001f, 0.000001f})
        for (float slopeX : {-2.0f, -0.25f, 0.0f, 0.5f, 8.0f})
            for (float slopeY : {-1.0f, 0.0f, 0.75f})
            {
                Query q;
                // Oblique screen basis, including a mirrored UV axis.
                q.PlaneDx = {scale, 0.5f * scale, scale * (slopeX + 0.5f * slopeY), slopeX};
                q.PlaneDy = {0.25f * scale, -scale, scale * (0.25f * slopeX - slopeY), slopeY};
                queries.push_back(q);
            }
    Query degenerate;
    degenerate.PlaneDx = {0.001f, 0.002f, 0.3f, 0.0f};
    degenerate.PlaneDy = {0.001f, 0.002f, -0.2f, 0.0f};
    queries.push_back(degenerate);
    Dispatch(4, 4, std::vector<float>(32, 0.0f), queries);
    ASSERT_EQ(m_Results.size(), queries.size());
    for (const auto& result : m_Results)
        EXPECT_NEAR(result[3], 0.0f, 0.00001f);
}

TEST_F(ShadowFilterComputeTest, GentleSlopesKeepEnoughGeometricNormalBias)
{
    std::vector<Query> queries;
    for (float cosine : {0.99f, 0.9f, 0.5f})
    {
        Query q;
        q.Control[1] = cosine;
        queries.push_back(q);
    }
    Dispatch(4, 4, std::vector<float>(32, 0.0f), queries);
    ASSERT_EQ(m_Results.size(), queries.size());
    for (size_t i = 0; i < queries.size(); ++i)
    {
        const float cosine = queries[i].Control[1];
        const float geometricOffset = 0.04f * std::sqrt(1.0f - cosine * cosine);
        const float authoredOffset = 0.5f * (1.0f - cosine);
        EXPECT_NEAR(m_Results[i][2], std::min(geometricOffset, authoredOffset), 1e-7f);
    }
}

// Directional shadows end at MaxShadowDistance through a smooth fade over its
// last tenth, never a step. Swept at several distances so the band is shown to
// scale with the distance rather than being a fixed width in metres.
TEST_F(ShadowFilterComputeTest, AuthoredDistanceFadeWidthsIncludeZeroAndFullRange)
{
    constexpr std::array<float, 5> widths{0.0f, 0.05f, 0.1f, 0.25f, 1.0f};
    std::vector<Query> queries;
    for (float width : widths)
        for (float depth : {0.0f, 50.0f, 90.0f, 95.0f, 99.0f, 100.0f, 110.0f})
        {
            Query query;
            query.FadeDepth = {depth, 100.0f, 0.2f, 0.7f};
            query.FadeControl[0] = width;
            queries.push_back(query);
        }
    Dispatch(4, 4, std::vector<float>(32, 0.0f), queries);
    ASSERT_EQ(m_Results.size(), queries.size());
    for (size_t i = 0; i < queries.size(); ++i)
    {
        const float width = queries[i].FadeControl[0];
        const float depth = queries[i].FadeDepth[0];
        SCOPED_TRACE(::testing::Message() << "width=" << width << " depth=" << depth);
        const float t = width > 0.0f ? std::clamp((depth / 100.0f - (1.0f - width)) / width, 0.0f, 1.0f)
                                    : (depth >= 100.0f ? 1.0f : 0.0f);
        const float expected = t * t * (3.0f - 2.0f * t);
        EXPECT_TRUE(std::isfinite(m_Results[i][4]));
        EXPECT_NEAR(m_Results[i][4], expected, 1e-5f);
        EXPECT_NEAR(m_Results[i][5], std::min(0.2f + 0.8f * expected, 0.7f), 1e-5f);
    }
}

TEST_F(ShadowFilterComputeTest, DistanceFadeEndsShadowsSmoothlyAtMaxShadowDistance)
{
    constexpr std::array<float, 4> kMaxDistances{20.0f, 150.0f, 200.0f, 1000.0f};
    constexpr uint32_t kSteps = 240; // 0.5 to 1.1 of the distance, 0.25% apart
    std::vector<Query> queries;
    for (float maxDistance : kMaxDistances)
        for (uint32_t i = 0; i <= kSteps; ++i)
        {
            Query q;
            q.FadeDepth = {maxDistance * (0.5f + 0.0025f * static_cast<float>(i)), maxDistance, 0.0f, 0.0f};
            queries.push_back(q);
        }
    Dispatch(4, 4, std::vector<float>(32, 0.0f), queries);
    ASSERT_EQ(m_Results.size(), queries.size());
    for (size_t d = 0; d < kMaxDistances.size(); ++d)
    {
        const float maxDistance = kMaxDistances[d];
        for (uint32_t i = 0; i <= kSteps; ++i)
        {
            const size_t index = d * (kSteps + 1) + i;
            const float depth = queries[index].FadeDepth[0];
            const float fade = m_Results[index][4];
            ASSERT_TRUE(std::isfinite(fade));
            if (depth <= 0.9f * maxDistance)
                EXPECT_LE(fade, 1e-6f) << "shadow weakened before the band at " << depth << " of " << maxDistance;
            if (depth >= maxDistance)
                EXPECT_EQ(fade, 1.0f) << "shadow survives past the distance at " << depth << " of " << maxDistance;
            if (i > 0)
            {
                const float step = fade - m_Results[index - 1][4];
                EXPECT_GE(step, 0.0f) << "fade reverses at " << depth << " of " << maxDistance;
                // Smoothstep over a 10% band peaks at 1.5 / band, so a 0.25%
                // step moves at most 0.0375: a cut would move the full 1.0.
                EXPECT_LE(step, 0.04f) << "fade steps at " << depth << " of " << maxDistance;
            }
        }
        // The smoothstep shape, read where it differs from a linear ramp.
        EXPECT_NEAR(m_Results[d * (kSteps + 1) + 170][4], 0.15625f, 1e-4f);
        EXPECT_NEAR(m_Results[d * (kSteps + 1) + 180][4], 0.5f, 1e-4f);
        EXPECT_NEAR(m_Results[d * (kSteps + 1) + 190][4], 0.84375f, 1e-4f);
    }
}

// The distance fade ends the ranged terms (the cascades, the traced rays) and leaves the terrain's
// clearance-map term, which has no range, alone. A receiver the terrain shadows stays shadowed
// through the band and past MaxShadowDistance; one only a cascade shadows fades to lit by the
// distance; a partly shadowed cascade term fades by the same weight; and the terrain term joins by
// a minimum at every depth.
TEST_F(ShadowFilterComputeTest, DistanceFadeEndsTheCascadeTermAndLeavesTheTerrainTerm)
{
    struct Terms
    {
        float Ranged;
        float Terrain;
    };
    constexpr std::array<Terms, 6> kTerms{{
        {0.0f, 1.0f},  // a cascade shadows it, the terrain does not
        {1.0f, 0.0f},  // the terrain shadows it, no cascade does
        {0.0f, 0.0f},  // both
        {0.25f, 1.0f}, // a cascade penumbra, terrain lit
        {0.25f, 0.0f}, // a cascade penumbra over terrain shadow
        {1.0f, 1.0f},  // lit
    }};
    constexpr std::array<float, 3> kMaxDistances{20.0f, 150.0f, 1000.0f};
    constexpr std::array<float, 10> kDepthFractions{0.5f, 0.9f, 0.925f, 0.95f, 0.975f,
                                                    1.0f, 1.001f, 1.2f, 3.0f, 20.0f};
    std::vector<Query> queries;
    for (float maxDistance : kMaxDistances)
        for (float fraction : kDepthFractions)
            for (const Terms& t : kTerms)
            {
                Query q;
                q.FadeDepth = {maxDistance * fraction, maxDistance, t.Ranged, t.Terrain};
                queries.push_back(q);
            }
    Dispatch(4, 4, std::vector<float>(32, 0.0f), queries);
    ASSERT_EQ(m_Results.size(), queries.size());
    for (size_t i = 0; i < queries.size(); ++i)
    {
        const auto& q = queries[i];
        const float depth = q.FadeDepth[0];
        const float maxDistance = q.FadeDepth[1];
        const float ranged = q.FadeDepth[2];
        const float terrain = q.FadeDepth[3];
        const float fade = m_Results[i][4];
        const float joined = m_Results[i][5];
        ASSERT_TRUE(std::isfinite(joined));
        const std::string where = "ranged " + std::to_string(ranged) + ", terrain " +
                                  std::to_string(terrain) + " at " + std::to_string(depth) + " of " +
                                  std::to_string(maxDistance);
        if (terrain == 0.0f)
            EXPECT_EQ(joined, 0.0f) << "the terrain's shadow must not fade: " << where;
        else
            EXPECT_NEAR(joined, ranged + (1.0f - ranged) * fade, 1e-6f)
                << "the cascade term must fade by the band's weight: " << where;
        if (terrain == 1.0f && depth >= maxDistance)
            EXPECT_EQ(joined, 1.0f) << "a cascade-only shadow must be gone past the distance: " << where;
        if (terrain == 1.0f && depth <= 0.9f * maxDistance)
            EXPECT_NEAR(joined, ranged, 1e-6f) << "the cascade term must be whole before the band: " << where;
    }
    // Mid-band, a cascade-only shadow is half faded, and a terrain-shadowed point is not faded at all.
    const size_t perDistance = kDepthFractions.size() * kTerms.size();
    const size_t midBand = 3 * kTerms.size(); // 0.95 of the distance
    for (size_t d = 0; d < kMaxDistances.size(); ++d)
    {
        EXPECT_NEAR(m_Results[d * perDistance + midBand + 0][5], 0.5f, 1e-4f);
        EXPECT_EQ(m_Results[d * perDistance + midBand + 1][5], 0.0f);
    }
}

// The 10-90 % rise of the disk kernel's visibility ([9]) over a sweep of steps + 1
// results starting at `first`, sampled 1/16 px apart, in screen pixels.
float EdgeWidthPixels(const std::vector<std::array<float, 12>>& results, size_t first, int steps)
{
    int rise10 = -1;
    int rise90 = -1;
    for (int i = 0; i <= steps; ++i)
    {
        const float visibility = results[first + i][9];
        if (rise10 < 0 && visibility >= 0.1f)
            rise10 = i;
        if (rise90 < 0 && visibility >= 0.9f)
            rise90 = i;
    }
    EXPECT_GE(rise10, 0);
    EXPECT_GT(rise90, rise10);
    return static_cast<float>(rise90 - rise10) / 16.0f;
}

// The disk kernels' radius is a world size, so a fitted cascade range can make it
// a fraction of a texel while the texel projects to about one screen pixel: the
// edge is then the texel staircase. The floor holds the radius at
// kShadowFilterFloorPixels (2) screen pixels, set by the longer screen axis,
// and leaves a radius that already spans it untouched.
TEST_F(ShadowFilterComputeTest, DiskKernelSpansTheScreenPixelFloorAtAOnePixelTexel)
{
    constexpr uint32_t kWidth = 64, kHeight = 8;
    constexpr float kEdgeTexel = 32.0f;
    std::vector<float> depths(kWidth * kHeight * 2);
    for (uint32_t layer = 0; layer < 2; ++layer)
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
                depths[(layer * kHeight + y) * kWidth + x] = x < kEdgeTexel ? 0.8f : 0.0f;

    // One texel per screen pixel on both axes, in UV.
    const std::array<float, 2> onePixelDx{1.0f / kWidth, 0.0f};
    const std::array<float, 2> onePixelDy{0.0f, 1.0f / kHeight};
    struct Sweep
    {
        float RadiusTexels;
        std::array<float, 2> Dx;
        std::array<float, 2> Dy;
    };
    const std::array<Sweep, 2> sweeps{{
        {0.25f, onePixelDx, onePixelDy},  // an outer cascade's world-sized radius, floored
        {0.25f, {0.0f, 0.0f}, {0.0f, 0.0f}}, // no screen footprint: the radius as given
    }};
    constexpr int kSteps = 192; // -6 to +6 px around the edge, 1/16 px apart
    std::vector<Query> queries;
    for (const Sweep& sweep : sweeps)
        for (int i = 0; i <= kSteps; ++i)
        {
            Query q;
            const float texel = kEdgeTexel - 6.0f + static_cast<float>(i) / 16.0f;
            q.UVDepthLayer = {texel / kWidth, 0.5f, 0.5f, 0.0f};
            q.FootprintDx = {sweep.Dx[0], sweep.Dx[1], sweep.RadiusTexels, 32.0f};
            q.FootprintDy = {sweep.Dy[0], sweep.Dy[1], 0.0f, 0.0f};
            queries.push_back(q);
        }
    // Radius only: unchanged where it spans the floor, the longer axis sets the floor.
    const size_t radiusQueries = queries.size();
    for (const auto& [radius, dx, dy] : std::array<std::array<float, 3>, 3>{{
             {3.0f, 1.0f, 1.0f}, {0.5f, 2.0f, 0.5f}, {1.5f, 0.5f, 0.5f}}})
    {
        Query q;
        q.FootprintDx = {dx / kWidth, 0.0f, radius, 32.0f};
        q.FootprintDy = {0.0f, dy / kHeight, 0.0f, 0.0f};
        queries.push_back(q);
    }
    Dispatch(kWidth, kHeight, depths, queries);
    ASSERT_EQ(m_Results.size(), queries.size());

    const size_t perSweep = kSteps + 1;
    EXPECT_FLOAT_EQ(m_Results[0][8], 2.0f);
    EXPECT_EQ(m_Results[perSweep][8], 0.25f);
    const float floored = EdgeWidthPixels(m_Results, 0, kSteps);
    const float unfloored = EdgeWidthPixels(m_Results, perSweep, kSteps);
    RecordProperty("edge_width_px_floored", std::to_string(floored));
    RecordProperty("edge_width_px_unfloored", std::to_string(unfloored));
    EXPECT_GE(floored, 2.5f) << "the floor must spread the edge over at least 2.5 px";
    EXPECT_LE(floored, 3.5f) << "the floor must not blur past about 3 px";
    EXPECT_LT(unfloored, 1.5f) << "without a footprint the sweep must show the bare sub-texel kernel";

    EXPECT_EQ(m_Results[radiusQueries + 0][8], 3.0f) << "a radius past the floor stays bit-exact";
    EXPECT_FLOAT_EQ(m_Results[radiusQueries + 1][8], 4.0f) << "the longer screen axis sets the floor";
    EXPECT_EQ(m_Results[radiusQueries + 2][8], 1.5f) << "a radius over the 1-texel floor at half a texel per pixel stays";
}

// At a grazing view the longer screen axis spans many texels per pixel. The floor
// stops at kShadowFilterFloorMaxTexels (8), and a floored disk follows the
// receiver's plane in depth, so a lit receiver sloped in light space stays lit
// across the whole disk instead of shadowing itself on its uphill side.
TEST_F(ShadowFilterComputeTest, FlooredDiskStaysLitOnASlopedReceiverAtAGrazingFootprint)
{
    constexpr uint32_t kWidth = 64, kHeight = 64;
    constexpr float kDepthPerTexel = 0.002f; // the receiver's slope along u, reverse-Z depth per texel
    std::vector<float> depths(kWidth * kHeight * 2);
    for (uint32_t layer = 0; layer < 2; ++layer)
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
                depths[(layer * kHeight + y) * kWidth + x] =
                    0.5f + kDepthPerTexel * (static_cast<float>(x) + 0.5f - kWidth * 0.5f);

    constexpr float kTexelsPerPixel = 10.0f; // along x; one along y
    std::vector<Query> queries;
    for (float u : {0.4f, 0.5f, 0.6f})
    {
        Query q;
        const float planeDepth = 0.5f + kDepthPerTexel * (u * kWidth - kWidth * 0.5f);
        q.UVDepthLayer = {u, 0.5f, planeDepth + 1e-4f, 0.0f};
        q.FootprintDx = {kTexelsPerPixel / kWidth, 0.0f, 0.25f, 16.0f};
        q.FootprintDy = {0.0f, 1.0f / kHeight, kDepthPerTexel * kTexelsPerPixel, 0.0f};
        queries.push_back(q);
    }
    Dispatch(kWidth, kHeight, depths, queries);
    ASSERT_EQ(m_Results.size(), queries.size());
    for (size_t i = 0; i < queries.size(); ++i)
    {
        SCOPED_TRACE(::testing::Message() << "u=" << queries[i].UVDepthLayer[0]);
        EXPECT_FLOAT_EQ(m_Results[i][8], 8.0f) << "the floor stops at 8 texels however grazing the view";
        EXPECT_EQ(m_Results[i][10], 1.0f) << "the floor binds on a quarter-texel disk";
        EXPECT_GE(m_Results[i][9], 0.99f) << "a lit sloped receiver must not shadow itself across the floored disk";
    }
}
} // namespace
