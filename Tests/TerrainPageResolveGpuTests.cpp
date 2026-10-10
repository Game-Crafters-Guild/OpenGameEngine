// The paged height resolve on the device: cbt_page.glsl dispatched through a probe kernel
// (TerrainPageResolveProbe.comp) over a cache texture, page table and slot fades uploaded from a
// driven PagedField, read back and held to the CPU mirror (PagedHeightSampler) at the same points.
// The CPU oracles prove the GLSL text is the mirror bit for bit; this proves the device runs it
// as written, with the hardware's bilinear filter in the cache tap. Two field shapes: a unified
// tiled terrain's (the DEM's 8193 x 2049 lattice) and an atlas tiled terrain's (16 x 4 tiles of
// 1025 samples, 16385 x 4097).

#include "TerrainPagedField.h"

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::TerrainECS;
using namespace GameEngine::TerrainECS::Test;
using GameEngine::PageStreaming::PageAddress;

namespace
{

// The hardware's bilinear weights carry 8 fractional bits (the Vulkan subTexelPrecisionBits every
// desktop vendor reports), so a tap may differ from the exact bilinear by up to 2^-8 of the texel
// differences along each axis: the quantum of the comparison.
constexpr float32 kSubTexelQuantum = 1.0f / 256.0f;

struct FieldShape
{
    const char* Name;
    uint32 SamplesX;
    uint32 SamplesZ;
};

// The CBTPageField and CBTPageLevel of cbt_page.glsl, std430.
struct GpuPageField
{
    uint32 Level0SamplesX;
    uint32 Level0SamplesZ;
    uint32 LevelCount;
    uint32 SlotsPerRow;
};

struct GpuPageLevel
{
    uint32 FirstEntry;
    uint32 PagesX;
    uint32 PagesZ;
    uint32 Pad;
};

struct QueryPoint
{
    float32 U;
    float32 V;
    float32 Lambda;
    float32 Unused;
};

std::vector<uint8_t> ReadProbeSpirv()
{
    std::ifstream file(TERRAIN_PAGE_PROBE_SPV, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// Every page of the levels from 2 up, a corner of level 1 and a larger corner of level 0, with a
// level-1 page and a level-0 page held back (the fallback walk), then the held level-1 page and a
// child of it arriving 0.1 s apart (a page and its parent both mid-fade).
void DriveToMixedResidency(PagedField& field)
{
    std::vector<PageAddress> wanted;
    for (const PageAddress& page : field.AllPages())
    {
        const bool level1Corner = page.Level == 1 && page.X < 8 && page.Z < 4;
        const bool level0Corner = page.Level == 0 && page.X < 12 && page.Z < 8;
        const bool heldBack = (page.Level == 1 && page.X == 1 && page.Z == 1) ||
                              (page.Level == 0 && page.X == 5 && page.Z == 6);
        if ((page.Level >= 2 || level1Corner || level0Corner) && !heldBack)
            wanted.push_back(page);
    }
    for (int frame = 0; frame < 60; ++frame)
        field.Step(wanted, 1.0f / 60.0f, 1024);
    wanted.push_back(PageAddress{0, 1, 1, 1});
    field.Step(wanted, 0.1f, 1024);
    wanted.push_back(PageAddress{0, 0, 3, 2});
    field.Step(wanted, 0.1f, 1024);
}

// The largest difference between two neighboring texels of any resident slot: the bound's scale.
float32 LargestNeighborDifference(const PagedField& field)
{
    float32 largest = 0.0f;
    const uint32 stride = PageStreaming::kPageStrideSamples;
    for (const PageStreaming::CachedPage& page : field.Cache.Resident())
    {
        const uint32 slot = field.Cache.SlotOf(page);
        for (uint32 r = 0; r < stride; ++r)
            for (uint32 c = 0; c < stride; ++c)
            {
                const auto at = [&](uint32 sx, uint32 sz) {
                    return field.Texels[std::size_t(field.Geometry.TexelZ(slot, sz)) * field.Geometry.CacheDim +
                                        field.Geometry.TexelX(slot, sx)];
                };
                if (c + 1 < stride)
                    largest = std::max(largest, std::fabs(at(c + 1, r) - at(c, r)));
                if (r + 1 < stride)
                    largest = std::max(largest, std::fabs(at(c, r + 1) - at(c, r)));
            }
    }
    return largest;
}

// A grid over the detailed corner (where levels 0 and 1 are resident) and one over the whole
// field, each at every lambda.
std::vector<QueryPoint> QueryPoints(const FieldShape& shape)
{
    std::vector<QueryPoint> points;
    const float32 cornerU = 16.0f * 128.0f / float32(shape.SamplesX - 1);
    const float32 cornerV = 12.0f * 128.0f / float32(shape.SamplesZ - 1);
    for (float32 lambda : {0.0f, 0.5f, 0.8f, 0.9375f, 1.6f, 2.9f, 4.0f, 30.0f})
    {
        for (uint32 i = 0; i < 128; ++i)
            for (uint32 k = 0; k < 64; ++k)
                points.push_back({cornerU * (float32(i) + 0.37f) / 128.0f, cornerV * (float32(k) + 0.61f) / 64.0f, lambda, 0.0f});
        for (uint32 i = 0; i <= 64; ++i)
            for (uint32 k = 0; k <= 32; ++k)
                points.push_back({float32(i) / 64.0f, float32(k) / 32.0f, lambda, 0.0f});
    }
    return points;
}

class TerrainPageResolveGpu : public ::testing::TestWithParam<FieldShape>
{
protected:
    void SetUp() override
    {
        DeviceDesc desc{};
        desc.preferredAPI = GraphicsAPI::Vulkan;
        desc.enableDynamicRendering = true;
        m_Device = DeviceFactory::CreateDevice(desc);
        if (!m_Device || !m_Device->Initialize(desc))
        {
            m_Device.reset();
            GTEST_SKIP() << "no Vulkan device";
        }
        const std::vector<uint8_t> spirv = ReadProbeSpirv();
        ASSERT_FALSE(spirv.empty()) << "the probe kernel is missing: " << TERRAIN_PAGE_PROBE_SPV;

        m_Layout.debugName = "TerrainPageResolveProbe_SetLayout";
        for (uint32_t binding = 0; binding < 6u; ++binding)
        {
            DescriptorBinding b{};
            b.binding = binding;
            b.type = binding == 0u ? DescriptorType::CombinedImageSampler : DescriptorType::StorageBuffer;
            b.count = 1u;
            b.shaderStages = kShaderStageCompute;
            m_Layout.bindings.push_back(b);
        }
        ComputePipelineDesc pipeline{};
        pipeline.ComputeShader = std::make_shared<const std::vector<uint8_t>>(spirv);
        pipeline.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
        pipeline.DebugName = "TerrainPageResolveProbe";
        m_Pipeline = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(pipeline));
        ASSERT_TRUE(m_Pipeline.IsValid());
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        for (BufferHandle buffer : m_Buffers)
            m_Device->DestroyBuffer(buffer);
        if (m_Texture.IsValid())
            m_Device->DestroyTexture(m_Texture);
        if (m_Sampler.IsValid())
            m_Device->DestroySampler(m_Sampler);
        m_Device->Shutdown();
    }

    BufferHandle MakeBuffer(const void* data, size_t bytes, const char* name)
    {
        BufferDesc desc{};
        desc.size = std::max<size_t>(bytes, 16);
        desc.usage = static_cast<uint32_t>(BufferUsage::Storage) | static_cast<uint32_t>(BufferUsage::TransferDst);
        desc.memoryUsage = BufferMemoryUsage::Upload;
        desc.debugName = name;
        BufferHandle buffer = m_Device->CreateBuffer(desc);
        m_Buffers.push_back(buffer);
        if (data != nullptr)
            m_Device->UpdateBuffer(buffer, 0, bytes, data);
        return buffer;
    }

    void UploadCache(const PagedField& field)
    {
        TextureDesc desc{};
        desc.width = field.Geometry.CacheDim;
        desc.height = field.Geometry.CacheDim;
        desc.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
        desc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) | static_cast<uint32_t>(TextureUsage::TransferDst);
        desc.persistent = true;
        desc.debugName = "TerrainPageResolveProbe.Cache";
        m_Texture = m_Device->CreateTexture(desc);
        ASSERT_TRUE(m_Texture.IsValid());
        const size_t bytes = field.Texels.size() * sizeof(float32);
        BufferHandle staging = m_Device->CreateUploadBuffer(bytes, "TerrainPageResolveProbe.CacheStaging");
        m_Device->UpdateBuffer(staging, 0, bytes, field.Texels.data());
        auto list = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        list->Begin();
        list->CopyBufferToTextureSubresource(staging, m_Texture, 0, 0, field.Geometry.CacheDim, field.Geometry.CacheDim, 0,
                                             size_t(field.Geometry.CacheDim) * sizeof(float32), 1, 0, 0, 0,
                                             ResourceState::Undefined);
        list->Barrier(ResourceBarrier::CreateTextureBarrier(m_Texture, ResourceState::CopyDest, ResourceState::ShaderResource));
        list->End();
        m_Device->ExecuteCommandLists({list.get()});
        m_Device->WaitForIdle();
        m_Device->DestroyBuffer(staging);
        m_Sampler = m_Device->CreateSampler(SamplerDesc::MaterialLinearClamp("TerrainPageResolveProbe.LinearClamp"));
        ASSERT_TRUE(m_Sampler.IsValid());
    }

    // The device resolve at every point.
    std::vector<float32> Dispatch(const PagedField& field, const std::vector<QueryPoint>& points)
    {
        std::vector<GpuPageLevel> levels;
        for (const PageStreaming::PageStoreLevel& level : field.Table.Levels)
            levels.push_back({level.FirstEntry, level.PagesX, level.PagesZ, 0u});
        const GpuPageField shape{field.Table.Levels.front().SamplesX, field.Table.Levels.front().SamplesZ,
                                 uint32(field.Table.Levels.size()), field.Geometry.SlotsPerRow};
        std::vector<uint8_t> queries(sizeof(GpuPageField) + 16u + points.size() * sizeof(QueryPoint));
        const uint32 count[4] = {uint32(points.size()), 0u, 0u, 0u};
        std::memcpy(queries.data(), &shape, sizeof(shape));
        std::memcpy(queries.data() + sizeof(shape), count, sizeof(count));
        std::memcpy(queries.data() + sizeof(shape) + 16u, points.data(), points.size() * sizeof(QueryPoint));

        const BufferHandle entries = MakeBuffer(field.Table.Entries.data(), field.Table.Entries.size() * sizeof(uint32), "Probe.Entries");
        const BufferHandle fades = MakeBuffer(field.Table.SlotFade.data(), field.Table.SlotFade.size() * sizeof(float32), "Probe.Fades");
        const BufferHandle shapes = MakeBuffer(levels.data(), levels.size() * sizeof(GpuPageLevel), "Probe.Levels");
        const BufferHandle query = MakeBuffer(queries.data(), queries.size(), "Probe.Queries");
        const size_t resultBytes = points.size() * sizeof(float32);
        const BufferHandle results = MakeBuffer(nullptr, resultBytes, "Probe.Results");

        DescriptorSetDesc setDesc{};
        setDesc.layout = m_Layout;
        setDesc.transient = true;
        setDesc.debugName = "TerrainPageResolveProbe.Set";
        const DescriptorSetHandle set = m_Device->CreateDescriptorSet(setDesc);
        m_Device->UpdateCombinedImageSamplerBinding(set, 0, m_Texture, m_Sampler);
        m_Device->UpdateStorageBufferBinding(set, 1, entries, 0, field.Table.Entries.size() * sizeof(uint32));
        m_Device->UpdateStorageBufferBinding(set, 2, fades, 0, field.Table.SlotFade.size() * sizeof(float32));
        m_Device->UpdateStorageBufferBinding(set, 3, shapes, 0, levels.size() * sizeof(GpuPageLevel));
        m_Device->UpdateStorageBufferBinding(set, 4, query, 0, queries.size());
        m_Device->UpdateStorageBufferBinding(set, 5, results, 0, resultBytes);

        auto list = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        list->Begin();
        list->SetPipeline(m_Pipeline);
        list->BindDescriptorSet(0, set, m_Pipeline);
        list->Dispatch((uint32(points.size()) + 63u) / 64u, 1, 1);
        list->End();
        m_Device->ExecuteCommandLists({list.get()});
        m_Device->WaitForIdle();

        std::vector<float32> heights(points.size());
        const void* mapped = m_Device->MapBuffer(results);
        if (mapped != nullptr)
            std::memcpy(heights.data(), mapped, resultBytes);
        m_Device->UnmapBuffer(results);
        return heights;
    }

    std::unique_ptr<IDevice> m_Device;
    DescriptorSetLayoutDesc m_Layout{};
    PipelineHandle m_Pipeline{};
    TextureHandle m_Texture{};
    SamplerHandle m_Sampler{};
    std::vector<BufferHandle> m_Buffers;
};

} // namespace

TEST_P(TerrainPageResolveGpu, TheDeviceResolveIsTheMirrorWithinTheSubTexelQuantum)
{
    const FieldShape shape = GetParam();
    PagedField field(640, 0.4f, shape.SamplesX, shape.SamplesZ);
    DriveToMixedResidency(field);
    ASSERT_NE(field.Cache.SlotOf(PagedField::Page(PageAddress{0, 0, 3, 2})), PageStreaming::kNoResidentSlot);
    ASSERT_LT(field.Cache.FadeOf(PagedField::Page(PageAddress{0, 1, 1, 1})), 1.0f) << "the fade chain is not mid-fade";
    ASSERT_EQ(field.Cache.SlotOf(PagedField::Page(PageAddress{0, 0, 5, 6})), PageStreaming::kNoResidentSlot);

    UploadCache(field);
    const std::vector<QueryPoint> points = QueryPoints(shape);
    const std::vector<float32> device = Dispatch(field, points);
    ASSERT_EQ(device.size(), points.size());

    const float32 bound = 2.0f * kSubTexelQuantum * LargestNeighborDifference(field) + 1.0e-4f;
    const PagedHeightSampler mirror = field.Sampler();
    float32 worst = 0.0f;
    uint32 outside = 0;
    for (std::size_t i = 0; i < points.size(); ++i)
    {
        const float32 expected = mirror.Sample(points[i].U, points[i].V, points[i].Lambda);
        const float32 error = std::fabs(device[i] - expected);
        worst = std::max(worst, error);
        if (!(error <= bound) && outside++ < 8)
            ADD_FAILURE() << shape.Name << " uv " << points[i].U << "," << points[i].V << " lambda " << points[i].Lambda
                          << ": device " << device[i] << " mirror " << expected << " (bound " << bound << ")";
    }
    EXPECT_EQ(outside, 0u) << outside << " of " << points.size() << " points outside the bound " << bound;
    std::printf("[ %s ] %zu points, worst |device - mirror| %.6g, bound %.6g\n", shape.Name, points.size(), worst, bound);
}

INSTANTIATE_TEST_SUITE_P(FieldShapes, TerrainPageResolveGpu,
                         ::testing::Values(FieldShape{"UnifiedTiled8193x2049", 8193u, 2049u},
                                           FieldShape{"AtlasTiled16385x4097", 16385u, 4097u}),
                         [](const ::testing::TestParamInfo<FieldShape>& info) { return std::string(info.param.Name); });
