#include <gtest/gtest.h>

#include "Ocean/OceanClipSim.h"
#include "Ocean/OceanRenderFeature.h"
#include "Ocean/OceanSplineRaster.h"
#include "Ocean/OceanTypes.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/ShaderReflection.h"
#include "StagedTestPaths.h"
#include "TestDeviceHelper.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Ocean;

namespace
{
// The probes sample one-LOD cascades of 4x4 one-metre texels. Every row of the
// probe texture holds the same four columns.
constexpr uint32_t kProbeExtent = 4;
constexpr uint32_t kSampledCascades = static_cast<uint32_t>(OceanSampledCascade::Count);
// Set 2 binding 4 is the OceanCascadeLayout block.
constexpr uint32_t kCascadeLayoutSet = 2;
constexpr uint32_t kCascadeLayoutBinding = 4;

// The clip probe's cascade: columns x < 2 m hold water (0), columns x >= 2 m are
// clipped (1), so the body's east edge sits at x = 2 m.
constexpr float kWater = 0.0f;
constexpr float kClipped = 1.0f;
// Returned outside every baked cascade; distinct from both baked values.
constexpr float kDefaultClippingState = 0.5f;

// The slots probe's texture: 1 only in column 2, so a helper reads 1 only at its
// own point (column 2 of its own layout) and 0 or its unavailable value when it
// places the point with another cascade's layout (outside the tile, or clamped
// to column 0 or 3).
constexpr std::array<float, kProbeExtent> kSlotColumns{0.0f, 0.0f, 1.0f, 0.0f};
// World distance between the cascades' origins in the slots probe: far beyond
// one 4 m tile.
constexpr float kSlotOriginSpacing = 100.0f;

struct ClipProbeData
{
    std::array<float, 8> PointsXZ{};
    std::array<float, 4> Result{};
};

struct SlotsProbeData
{
    std::array<std::array<float, 4>, kSampledCascades> PointsXZ{};
    std::array<float, 8> Results{};
};

std::vector<uint8_t> LoadProbe(const char* fileName)
{
    return Utils::ReadFile((TestPaths::StagedRoot() / "Shaders" / fileName).string());
}

DescriptorBinding ComputeBinding(uint32_t slot, DescriptorType type)
{
    DescriptorBinding value{};
    value.binding = slot;
    value.type = type;
    value.count = 1;
    value.shaderStages = kShaderStageCompute;
    return value;
}

// One-LOD layout with 1 m texels and its origin at (originX, 0).
OceanCascadeLayoutGPU ProbeLayout(float originX)
{
    OceanCascadeLayoutGPU layout{};
    layout.CascadeOriginScale[0][0] = originX;
    layout.CascadeOriginScale[0][2] = 1.0f;
    layout.CascadeOriginScale[0][3] = 1.0f;
    layout.LodCount = 1;
    return layout;
}

class OceanCascadeLayoutTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        if (m_View.IsValid())
            m_Device->DestroyTextureView(m_View);
        if (m_Texture.IsValid())
            m_Device->DestroyTexture(m_Texture);
        if (m_Sampler.IsValid())
            m_Device->DestroySampler(m_Sampler);
        for (const auto buffer : m_Buffers)
        {
            if (buffer.IsValid())
                m_Device->DestroyBuffer(buffer);
        }
        m_Device->Shutdown();
    }

    BufferHandle MakeBuffer(size_t size, BufferUsage usage, const void* data)
    {
        BufferDesc desc{};
        desc.size = size;
        desc.usage = static_cast<uint32_t>(usage);
        desc.memoryUsage = BufferMemoryUsage::Upload;
        auto buffer = m_Device->CreateBuffer(desc);
        m_Buffers.push_back(buffer);
        if (buffer.IsValid())
        {
            void* mapped = m_Device->MapBuffer(buffer);
            EXPECT_NE(mapped, nullptr);
            if (mapped)
                std::memcpy(mapped, data, size);
            m_Device->UnmapBuffer(buffer);
        }
        return buffer;
    }

    // A one-layer R32_FLOAT cascade texture whose every row holds `columns`,
    // with a linear-clamp sampler: the texture every probe binding samples.
    void CreateProbeTexture(const std::array<float, kProbeExtent>& columns)
    {
        TextureDesc textureDesc{};
        textureDesc.width = textureDesc.height = kProbeExtent;
        textureDesc.depth = textureDesc.mipLevels = textureDesc.arrayLayers = textureDesc.sampleCount = 1;
        textureDesc.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
        textureDesc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
        m_Texture = m_Device->CreateTexture(textureDesc);
        ASSERT_TRUE(m_Texture.IsValid());
        TextureViewDesc viewDesc{};
        viewDesc.viewType = TextureViewType::View2DArray;
        viewDesc.levelCount = viewDesc.layerCount = 1;
        m_View = m_Device->CreateTextureView(m_Texture, viewDesc);
        ASSERT_TRUE(m_View.IsValid());
        m_Sampler = m_Device->CreateSampler(SamplerDesc::MaterialLinearClamp("CascadeProbe"));
        ASSERT_TRUE(m_Sampler.IsValid());
        std::array<float, kProbeExtent * kProbeExtent> texels{};
        for (uint32_t row = 0; row < kProbeExtent; ++row)
            for (uint32_t column = 0; column < kProbeExtent; ++column)
                texels[row * kProbeExtent + column] = columns[column];
        const auto upload = MakeBuffer(sizeof(texels), BufferUsage::TransferSrc, texels.data());
        ASSERT_TRUE(upload.IsValid());
        auto copy = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        copy->Begin();
        copy->Barrier(ResourceBarrier::CreateTextureBarrier(m_Texture, ResourceState::Undefined, ResourceState::CopyDest));
        copy->CopyBufferToTextureSubresource(upload, m_Texture, 0, 0, kProbeExtent, kProbeExtent);
        copy->Barrier(ResourceBarrier::CreateTextureBarrier(m_Texture, ResourceState::CopyDest, ResourceState::ShaderResource));
        copy->End();
        m_Device->ExecuteCommandLists({copy.get()});
        m_Device->WaitForIdle();
    }

    // One dispatch of `pipeline` with the probe set at set 0 and the ocean set at set 2.
    void DispatchOnce(PipelineHandle pipeline, DescriptorSetHandle probeSet, DescriptorSetHandle oceanSet)
    {
        auto command = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        command->Begin();
        command->SetPipeline(pipeline);
        command->BindDescriptorSet(0, probeSet, pipeline);
        command->BindDescriptorSet(2, oceanSet, pipeline);
        command->Dispatch(1, 1, 1);
        command->End();
        m_Device->ExecuteCommandLists({command.get()});
        m_Device->WaitForIdle();
    }

    // A confined spline river along X from x 550 to 700 (8 m half width), baked
    // through OceanClipSim with `extraSources` and rasterized through
    // OceanSplineRaster with the camera at (cameraX, 0) and the default state
    // clipped, then sampled with the shipped OceanSampleClip at the four XZ points.
    std::array<float, 4> BakeRiverAndSampleClip(float cameraX, const OceanClipSourceGPU* extraSources,
                                                uint32 extraSourceCount, const std::array<float, 8>& pointsXZ)
    {
        std::array<float, 4> result{-17.0f, -17.0f, -17.0f, -17.0f};
        OceanClipSim clip;
        EXPECT_TRUE(clip.Initialize(m_Device.get()));
        clip.SetDefaultClippingState(kClipped);
        clip.SetSources(extraSources, extraSourceCount);
        OceanSplineRaster raster;
        OceanRibbonStyle style{};
        style.Width = 8.0f;
        style.Feather = 1.0f;
        style.Flags = 4u | 16u; // clip + confine: restore water inside the band
        raster.SetTriangles(BuildOceanRibbon({{550, 0, 0, 1, 0}, {700, 0, 0, 1, 0}}, style));
        clip.SetSplineActive(raster.HasField(2), true);
        float bounds[4] = {};
        EXPECT_TRUE(raster.ClipBoundsXZ(bounds[0], bounds[1], bounds[2], bounds[3]));
        clip.SetSplineBounds(true, bounds[0], bounds[1], bounds[2], bounds[3]);
        EXPECT_TRUE(clip.BeginFrame(cameraX, 0.0f));

        RenderGraph::RGResourcePool persistent(m_Device.get());
        RenderGraph::RGTransientPool transient(m_Device.get());
        RenderGraph::RGUploadRing ring(m_Device.get(), 2, 262144);
        {
            RenderGraph::RGFrame frame(m_Device.get(), &persistent, &transient, &ring);
            frame.BeginFrame(0);
            const auto params = frame.AllocUpload<OceanClipParamsGPU>();
            EXPECT_TRUE(params.Valid());
            if (!params.Valid())
                return result;
            clip.FillParams(*params.Ptr);
            const RenderGraph::RGTexture clipRG = clip.ImportRG(frame);
            frame.AddPass(
                "OceanClipSim", PassPhase::kEarlySetup,
                [&](RenderGraph::RGPassBuilder& p) { p.Write(clipRG, RenderGraph::RGTextureWrite::Storage); },
                [&clip, buffer = params.Buffer, offset = params.Offset](RenderGraph::RGContext& ctx)
                { clip.RecordDispatch(ctx.GetDevice(), ctx.Cmd, buffer, offset); });
            frame.MarkOutput(clipRG, RenderGraph::RGImageLayout::ShaderReadOnly);
            EXPECT_TRUE(raster.Declare(frame, m_Device.get(), 2u, clipRG, clip.GetLayout(), clip.GetResolution()));
            frame.Execute();
            m_Device->FinalizeFrame();
            m_Device->WaitForIdle();
        }

        const auto bytes = LoadProbe("ocean_clip_probe.comp.spv");
        EXPECT_FALSE(bytes.empty());
        DescriptorSetLayoutDesc probeLayout{}, emptyLayout{}, oceanLayout{};
        probeLayout.bindings = {ComputeBinding(0, DescriptorType::StorageBuffer)};
        oceanLayout.bindings = {ComputeBinding(1, DescriptorType::StorageBuffer),
                                ComputeBinding(4, DescriptorType::UniformBuffer),
                                ComputeBinding(10, DescriptorType::CombinedImageSampler)};
        ComputePipelineDesc pipelineDesc{};
        pipelineDesc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(bytes);
        for (const auto& layout : {probeLayout, emptyLayout, oceanLayout})
            pipelineDesc.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(layout));
        const auto pipeline =
            m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(pipelineDesc));
        EXPECT_TRUE(pipeline.IsValid());

        OceanParamsGPU oceanParams{};
        oceanParams.ClipAvailable = 1;
        oceanParams.DefaultClippingState = kClipped;
        OceanSampledCascadeLayoutsGPU cascades{};
        cascades[OceanSampledCascade::Clip] = clip.GetLayout();
        ClipProbeData probe{};
        probe.PointsXZ = pointsXZ;
        probe.Result = result;
        const auto paramsBuffer = MakeBuffer(sizeof(oceanParams), BufferUsage::Storage, &oceanParams);
        const auto cascadeBuffer = MakeBuffer(sizeof(cascades), BufferUsage::Uniform, &cascades);
        const auto probeBuffer = MakeBuffer(sizeof(probe), BufferUsage::Storage, &probe);
        DescriptorSetDesc descriptor{};
        descriptor.layout = probeLayout;
        const auto probeSet = m_Device->CreateDescriptorSet(descriptor);
        descriptor.layout = oceanLayout;
        const auto oceanSet = m_Device->CreateDescriptorSet(descriptor);
        m_Device->UpdateStorageBufferBinding(probeSet, 0, probeBuffer, 0, sizeof(probe));
        m_Device->UpdateStorageBufferBinding(oceanSet, 1, paramsBuffer, 0, sizeof(oceanParams));
        m_Device->UpdateBufferBinding(oceanSet, 4, cascadeBuffer, 0, sizeof(cascades));
        m_Device->UpdateCombinedImageSamplerBinding(oceanSet, 10, clip.GetClipTexture(), clip.GetClipSampler());
        DispatchOnce(pipeline, probeSet, oceanSet);
        ClipProbeData read{};
        if (void* mapped = m_Device->MapBuffer(probeBuffer))
        {
            std::memcpy(&read, mapped, sizeof(read));
            m_Device->UnmapBuffer(probeBuffer);
        }
        return read.Result;
    }

    std::unique_ptr<IDevice> m_Device;
    std::vector<BufferHandle> m_Buffers;
    TextureHandle m_Texture{};
    TextureViewHandle m_View{};
    SamplerHandle m_Sampler{};
};
} // namespace

// The clip cascade and the foam cascade snap to the camera on different frames,
// so when only the camera moves the foam layout can differ from the layout the
// clip was baked with. The body's edges must stay where the clip was baked.
TEST_F(OceanCascadeLayoutTest, ClipRegionEdgesStayPutWhenOnlyTheCameraMoves)
{
    const auto bytes = LoadProbe("ocean_clip_probe.comp.spv");
    ASSERT_FALSE(bytes.empty());
    DescriptorSetLayoutDesc probeLayout{}, emptyLayout{}, oceanLayout{};
    probeLayout.bindings = {ComputeBinding(0, DescriptorType::StorageBuffer)};
    oceanLayout.bindings = {ComputeBinding(1, DescriptorType::StorageBuffer),
                            ComputeBinding(4, DescriptorType::UniformBuffer),
                            ComputeBinding(10, DescriptorType::CombinedImageSampler)};
    ComputePipelineDesc pipelineDesc{};
    pipelineDesc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(bytes);
    for (const auto& layout : {probeLayout, emptyLayout, oceanLayout})
        pipelineDesc.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(layout));
    const auto pipeline = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(pipelineDesc));
    ASSERT_TRUE(pipeline.IsValid());
    ASSERT_NO_FATAL_FAILURE(CreateProbeTexture({kWater, kWater, kClipped, kClipped}));

    OceanParamsGPU params{};
    params.ClipAvailable = 1;
    params.DefaultClippingState = kDefaultClippingState;
    OceanSampledCascadeLayoutsGPU cascades{};
    cascades[OceanSampledCascade::Clip] = ProbeLayout(0.0f);
    OceanCascadeLayoutGPU& foamLayout = cascades[OceanSampledCascade::Foam];
    foamLayout = ProbeLayout(0.0f);
    // Texel centres either side of the east edge, on the middle row.
    ClipProbeData probe{};
    probe.PointsXZ = {0.5f, 1.5f, 1.5f, 1.5f, 2.5f, 1.5f, 3.5f, 1.5f};
    probe.Result = {-17.0f, -17.0f, -17.0f, -17.0f};
    const auto paramsBuffer = MakeBuffer(sizeof(params), BufferUsage::Storage, &params);
    const auto cascadeBuffer = MakeBuffer(sizeof(cascades), BufferUsage::Uniform, &cascades);
    const auto probeBuffer = MakeBuffer(sizeof(probe), BufferUsage::Storage, &probe);
    ASSERT_TRUE(paramsBuffer.IsValid());
    ASSERT_TRUE(cascadeBuffer.IsValid());
    ASSERT_TRUE(probeBuffer.IsValid());
    DescriptorSetDesc descriptor{};
    descriptor.layout = probeLayout;
    const auto probeSet = m_Device->CreateDescriptorSet(descriptor);
    descriptor.layout = oceanLayout;
    const auto oceanSet = m_Device->CreateDescriptorSet(descriptor);
    ASSERT_TRUE(probeSet.IsValid());
    ASSERT_TRUE(oceanSet.IsValid());
    m_Device->UpdateStorageBufferBinding(probeSet, 0, probeBuffer, 0, sizeof(probe));
    m_Device->UpdateStorageBufferBinding(oceanSet, 1, paramsBuffer, 0, sizeof(params));
    m_Device->UpdateBufferBinding(oceanSet, 4, cascadeBuffer, 0, sizeof(cascades));
    m_Device->UpdateCombinedImageSamplerBinding(oceanSet, 10, m_View, m_Sampler);

    // The camera moves along x by whole texels; only the foam cascade has
    // re-snapped to it.
    for (const float foamOriginX : {0.0f, -2.0f, 2.0f, -64.0f})
    {
        SCOPED_TRACE(::testing::Message() << "foam origin x=" << foamOriginX);
        foamLayout.CascadeOriginScale[0][0] = foamOriginX;
        m_Device->UpdateBuffer(cascadeBuffer, 0, sizeof(cascades), &cascades);
        m_Device->UpdateBuffer(probeBuffer, 0, sizeof(probe), &probe);
        DispatchOnce(pipeline, probeSet, oceanSet);
        ClipProbeData read{};
        void* mapped = m_Device->MapBuffer(probeBuffer);
        ASSERT_NE(mapped, nullptr);
        std::memcpy(&read, mapped, sizeof(read));
        m_Device->UnmapBuffer(probeBuffer);
        EXPECT_FLOAT_EQ(read.Result[0], kWater);
        EXPECT_FLOAT_EQ(read.Result[1], kWater);
        EXPECT_FLOAT_EQ(read.Result[2], kClipped);
        EXPECT_FLOAT_EQ(read.Result[3], kClipped);
    }
}

// Every sample helper that reads the layout block places its texture with its
// own cascade's slot: each cascade's layout sits 100 m from the others, and each
// helper reads its marked texel only through its own slot.
TEST_F(OceanCascadeLayoutTest, EachHelperSamplesWithItsOwnCascadeLayout)
{
    const auto bytes = LoadProbe("ocean_cascade_slots_probe.comp.spv");
    ASSERT_FALSE(bytes.empty());
    // The bindings of the seven sampled cascade textures, in OceanSampledCascade
    // order: foam, seabed depth, flow, dynamic waves, wave mask, clip, albedo.
    constexpr std::array<uint32_t, kSampledCascades> textureBindings{3, 6, 8, 9, 16, 10, 11};
    DescriptorSetLayoutDesc probeLayout{}, emptyLayout{}, oceanLayout{};
    probeLayout.bindings = {ComputeBinding(0, DescriptorType::StorageBuffer)};
    oceanLayout.bindings = {ComputeBinding(1, DescriptorType::StorageBuffer),
                            ComputeBinding(4, DescriptorType::UniformBuffer)};
    for (const uint32_t slot : textureBindings)
        oceanLayout.bindings.push_back(ComputeBinding(slot, DescriptorType::CombinedImageSampler));
    ComputePipelineDesc pipelineDesc{};
    pipelineDesc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(bytes);
    for (const auto& layout : {probeLayout, emptyLayout, oceanLayout})
        pipelineDesc.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(layout));
    const auto pipeline = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(pipelineDesc));
    ASSERT_TRUE(pipeline.IsValid());
    ASSERT_NO_FATAL_FAILURE(CreateProbeTexture(kSlotColumns));

    OceanParamsGPU params{};
    params.SeabedDepthAvailable = 1;
    params.FlowAvailable = 1;
    params.DynamicWavesAvailable = 1;
    params.WaveMaskAvailable = 1;
    params.ClipAvailable = 1;
    params.DefaultClippingState = kDefaultClippingState;
    params.AlbedoAvailable = 1;
    OceanSampledCascadeLayoutsGPU cascades{};
    SlotsProbeData probe{};
    for (uint32_t index = 0; index < kSampledCascades; ++index)
    {
        const float originX = kSlotOriginSpacing * static_cast<float>(index);
        cascades.Layouts[index] = ProbeLayout(originX);
        // Column 2, row 2 of this cascade's own tile.
        probe.PointsXZ[index] = {originX + 2.5f, 2.5f, 0.0f, 0.0f};
    }
    probe.Results.fill(-17.0f);
    const auto paramsBuffer = MakeBuffer(sizeof(params), BufferUsage::Storage, &params);
    const auto cascadeBuffer = MakeBuffer(sizeof(cascades), BufferUsage::Uniform, &cascades);
    const auto probeBuffer = MakeBuffer(sizeof(probe), BufferUsage::Storage, &probe);
    ASSERT_TRUE(paramsBuffer.IsValid());
    ASSERT_TRUE(cascadeBuffer.IsValid());
    ASSERT_TRUE(probeBuffer.IsValid());
    DescriptorSetDesc descriptor{};
    descriptor.layout = probeLayout;
    const auto probeSet = m_Device->CreateDescriptorSet(descriptor);
    descriptor.layout = oceanLayout;
    const auto oceanSet = m_Device->CreateDescriptorSet(descriptor);
    ASSERT_TRUE(probeSet.IsValid());
    ASSERT_TRUE(oceanSet.IsValid());
    m_Device->UpdateStorageBufferBinding(probeSet, 0, probeBuffer, 0, sizeof(probe));
    m_Device->UpdateStorageBufferBinding(oceanSet, 1, paramsBuffer, 0, sizeof(params));
    m_Device->UpdateBufferBinding(oceanSet, 4, cascadeBuffer, 0, sizeof(cascades));
    for (const uint32_t slot : textureBindings)
        m_Device->UpdateCombinedImageSamplerBinding(oceanSet, slot, m_View, m_Sampler);

    DispatchOnce(pipeline, probeSet, oceanSet);
    SlotsProbeData read{};
    void* mapped = m_Device->MapBuffer(probeBuffer);
    ASSERT_NE(mapped, nullptr);
    std::memcpy(&read, mapped, sizeof(read));
    m_Device->UnmapBuffer(probeBuffer);
    constexpr std::array<const char*, kSampledCascades> names{
        "foam", "seabed depth", "flow", "dynamic waves", "wave mask", "clip", "albedo"};
    for (uint32_t index = 0; index < kSampledCascades; ++index)
        EXPECT_FLOAT_EQ(read.Results[index], 1.0f) << names[index];
}

// The block the helpers read is the array the CPU uploads: one
// OceanCascadeLayoutGPU per OceanSampledCascade, std140, nothing else.
TEST(OceanCascadeLayoutBlock, ReflectedBlockMatchesTheUploadedStructs)
{
    const auto bytes = LoadProbe("ocean_cascade_slots_probe.comp.spv");
    ASSERT_FALSE(bytes.empty());
    ASSERT_EQ(bytes.size() % sizeof(uint32_t), 0u);
    std::vector<uint32_t> words(bytes.size() / sizeof(uint32_t));
    std::memcpy(words.data(), bytes.data(), bytes.size());
    StageReflectionResult stage{};
    ReflectionOptions options{};
    std::string error;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Compute, words.data(), words.size(),
                            options, stage, &error)) << error;
    const auto meta = MergeStages({stage});
    const BlockLayout* block = nullptr;
    for (const auto& set : meta.Sets)
    {
        if (set.Set != kCascadeLayoutSet)
            continue;
        for (const auto& binding : set.Bindings)
            if (binding.Binding == kCascadeLayoutBinding && binding.Block)
                block = &*binding.Block;
    }
    ASSERT_NE(block, nullptr);
    EXPECT_EQ(block->Size, sizeof(OceanSampledCascadeLayoutsGPU));
    ASSERT_EQ(block->Members.size(), 1u);
    const Member& cascades = block->Members[0];
    EXPECT_EQ(cascades.Offset, 0u);
    ASSERT_EQ(cascades.Type.ArrayDims, std::vector<uint32_t>{kSampledCascades});
    ASSERT_TRUE(cascades.ArrayStride.has_value());
    EXPECT_EQ(*cascades.ArrayStride, sizeof(OceanCascadeLayoutGPU));
    // Reflection records nested members' types, not their offsets. A vec4 per LOD
    // then a uvec4 is std140 offsets 0 and 16 * kMaxOceanLodCascades: the CPU
    // struct's CascadeOriginScale and LodCount lane.
    ASSERT_EQ(cascades.Type.StructMembers.size(), 2u);
    const TypeDesc& originScale = cascades.Type.StructMembers[0].Type;
    EXPECT_EQ(originScale.Kind, TypeKind::Vector);
    EXPECT_EQ(originScale.Base, BaseType::Float);
    EXPECT_EQ(originScale.VecSize, 4u);
    EXPECT_EQ(originScale.ArrayDims, std::vector<uint32_t>{kMaxOceanLodCascades});
    EXPECT_EQ(offsetof(OceanCascadeLayoutGPU, LodCount), 16u * kMaxOceanLodCascades);
    const TypeDesc& lodCountLane = cascades.Type.StructMembers[1].Type;
    EXPECT_EQ(lodCountLane.Kind, TypeKind::Vector);
    EXPECT_EQ(lodCountLane.Base, BaseType::UInt);
    EXPECT_EQ(lodCountLane.VecSize, 4u);
    EXPECT_TRUE(lodCountLane.ArrayDims.empty());
}

// The CPU fill: each slot carries its own sim's snapped layout, clamped by the
// authored LOD limit except the clip's, whose coarsest layer covers all authored
// water; a cascade the consumer does not bind keeps LodCount 0.
TEST_F(OceanCascadeLayoutTest, SampledCascadeLayoutsTakesEachSimsOwnLayout)
{
    OceanRenderFeature feature;
    IDevice* device = m_Device.get();
    ASSERT_TRUE(feature.GetFoamSim().Initialize(device));
    ASSERT_TRUE(feature.GetSeabedDepth().Initialize(device));
    ASSERT_TRUE(feature.GetFlow().Initialize(device));
    ASSERT_TRUE(feature.GetDynWaves().Initialize(device));
    ASSERT_TRUE(feature.GetWaveMask().Initialize(device));
    ASSERT_TRUE(feature.GetClip().Initialize(device));
    ASSERT_TRUE(feature.GetAlbedo().Initialize(device));
    // Each sim snapped to a different camera, as when they snap on different frames.
    feature.GetFoamSim().BeginFrame(-1000.0f, 0.0f);
    feature.GetSeabedDepth().BeginFrame(-600.0f, 0.0f);
    feature.GetFlow().BeginFrame(-200.0f, 0.0f);
    feature.GetDynWaves().BeginFrame(200.0f, 0.0f);
    feature.GetWaveMask().BeginFrame(600.0f, 0.0f);
    feature.GetClip().BeginFrame(1000.0f, 0.0f);
    feature.GetAlbedo().BeginFrame(1400.0f, 0.0f);
    const std::array<const OceanCascadeLayoutGPU*, kSampledCascades> own{
        &feature.GetFoamSim().GetFoamLayout(), &feature.GetSeabedDepth().GetLayout(),
        &feature.GetFlow().GetLayout(),        &feature.GetDynWaves().GetLayout(),
        &feature.GetWaveMask().GetLayout(),    &feature.GetClip().GetLayout(),
        &feature.GetAlbedo().GetLayout()};

    OceanRenderFeature::SampledCascadeAvailability available{};
    for (uint32_t index = 0; index < kSampledCascades; ++index)
        available.Available[index] = true;
    const OceanSampledCascadeLayoutsGPU all = feature.SampledCascadeLayouts(available);
    for (uint32_t index = 0; index < kSampledCascades; ++index)
    {
        SCOPED_TRACE(::testing::Message() << "cascade " << index);
        ASSERT_GT(own[index]->LodCount, 2u);
        EXPECT_EQ(std::memcmp(&all.Layouts[index], own[index], sizeof(OceanCascadeLayoutGPU)), 0);
        for (uint32_t other = 0; other < index; ++other)
            EXPECT_NE(all.Layouts[index].CascadeOriginScale[0][0], all.Layouts[other].CascadeOriginScale[0][0]);
    }

    available[OceanSampledCascade::Albedo] = false;
    feature.SetLodCountLimit(2);
    const OceanSampledCascadeLayoutsGPU limited = feature.SampledCascadeLayouts(available);
    EXPECT_EQ(limited[OceanSampledCascade::Albedo].LodCount, 0u);
    for (uint32_t index = 0; index < kSampledCascades; ++index)
    {
        if (index == static_cast<uint32_t>(OceanSampledCascade::Albedo))
            continue;
        SCOPED_TRACE(::testing::Message() << "cascade " << index);
        const bool clip = index == static_cast<uint32_t>(OceanSampledCascade::Clip);
        EXPECT_EQ(limited.Layouts[index].LodCount, clip ? own[index]->LodCount : 2u);
        EXPECT_EQ(std::memcmp(limited.Layouts[index].CascadeOriginScale, own[index]->CascadeOriginScale,
                              sizeof(own[index]->CascadeOriginScale)),
                  0);
    }
}

// A confined river far from the camera: its spline water lies beyond every
// camera-snapped clip layer (more than 235.5 m away at the default 64 m base
// scale), so only the anchored layer over the authored water can carry it. The
// water must be there wherever the camera is.
TEST_F(OceanCascadeLayoutTest, ConfinedSplineWaterBeyondTheCameraLayersStaysWater)
{
    // On the river 600 m and 650 m from the camera; 40 m beside it; at the camera.
    const auto read = BakeRiverAndSampleClip(0.0f, nullptr, 0u,
                                             {600.0f, 0.0f, 650.0f, 0.0f, 600.0f, 40.0f, 0.0f, 0.0f});
    EXPECT_FLOAT_EQ(read[0], kWater) << "on the river, 600 m out";
    EXPECT_FLOAT_EQ(read[1], kWater) << "on the river, 650 m out";
    EXPECT_FLOAT_EQ(read[2], kClipped) << "beside the river";
    EXPECT_FLOAT_EQ(read[3], kClipped) << "at the camera, no water authored";
}

// Authored water far away widens the anchored layer until its texels are wider
// than a river. The anchored layer is added beyond the camera layers, not in
// place of the outermost one, so a river 150 m from the camera stays inside the
// camera layers and reads water however far away the other water is.
TEST_F(OceanCascadeLayoutTest, DistantWaterDoesNotCoarsenTheCameraLayers)
{
    OceanClipSourceGPU far{};
    far.OriginExtent[0] = -5000.0f;
    far.OriginExtent[2] = 20.0f;
    far.OriginExtent[3] = 20.0f;
    far.ClipState[0] = kWater;
    const auto read = BakeRiverAndSampleClip(450.0f, &far, 1u,
                                             {600.0f, 0.0f, 650.0f, 0.0f, 600.0f, 40.0f, 450.0f, 0.0f});
    EXPECT_FLOAT_EQ(read[0], kWater) << "on the river, 150 m from the camera";
    EXPECT_FLOAT_EQ(read[1], kWater) << "on the river, 200 m from the camera";
    EXPECT_FLOAT_EQ(read[2], kClipped) << "beside the river";
    EXPECT_FLOAT_EQ(read[3], kClipped) << "at the camera, no water authored";
}
