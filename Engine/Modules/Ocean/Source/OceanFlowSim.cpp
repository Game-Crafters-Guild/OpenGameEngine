#include "Ocean/OceanFlowSim.h"
#include "Ocean/OceanShaderProgram.h"
#include "Rendering/Materials/ShaderLayoutShape.h"
#include "OceanShaderDirectory.h"

#include "Logger/Logger.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Vector2.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/Utils/TextureUploadHelpers.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace GameEngine::Ocean
{

namespace fs = std::filesystem;
using namespace ::GameEngine::Rendering;

namespace
{

TextureHandle CreateNeutralFlowMapTexture(IDevice* device)
{
    if (!device)
        return {};

    TextureDesc td{};
    td.width = 1;
    td.height = 1;
    td.depth = 1;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.format = static_cast<uint32>(TextureFormat::R16G16_FLOAT);
    td.usage = static_cast<uint32>(TextureUsage::ShaderResource) |
               static_cast<uint32>(TextureUsage::TransferDst);
    td.sampleCount = 1;
    td.initialState = ResourceState::Undefined;
    td.debugName = "Ocean_FlowMap_Neutral";

    TextureHandle texture = device->CreateTexture(td);
    if (!texture.IsValid())
        return {};

    const uint16_t neutralRgHalf[2] = {0x3800u, 0x3800u}; // half-float 0.5, 0.5
    UploadTexture2D(device, texture, neutralRgHalf, 1, 1, sizeof(neutralRgHalf),
                    "Ocean_FlowMap_Neutral_Upload");
    return texture;
}

float SmoothStep(float edge0, float edge1, float x)
{
    const float t = std::clamp((x - edge0) / std::max(edge1 - edge0, 1e-6f), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float SegmentDistance(float px, float pz, float ax, float az, float bx, float bz)
{
    const float abx = bx - ax;
    const float abz = bz - az;
    const float denom = std::max(abx * abx + abz * abz, 1e-12f);
    const float t = std::clamp(((px - ax) * abx + (pz - az) * abz) / denom, 0.0f, 1.0f);
    const float cx = ax + abx * t;
    const float cz = az + abz * t;
    const float dx = px - cx;
    const float dz = pz - cz;
    return std::sqrt(dx * dx + dz * dz);
}

float PolygonWeight(float worldX, float worldZ, const OceanFlowPolygonGPU& poly)
{
    const uint32 count = static_cast<uint32>(
        std::clamp(poly.Meta[0], 0.0f, static_cast<float32>(kMaxOceanFlowPolygonPoints)));
    if (count < 3u)
        return 0.0f;
    if (worldX < poly.Bounds[0] || worldZ < poly.Bounds[1] ||
        worldX > poly.Bounds[2] || worldZ > poly.Bounds[3])
    {
        return 0.0f;
    }

    std::array<Mathematics::Vector2, kMaxOceanFlowPolygonPoints> storage;
    for (uint32 i = 0u; i < count; ++i)
        storage[i] = Mathematics::Vector2(poly.Points[i][0], poly.Points[i][1]);
    const std::span<const Mathematics::Vector2> points(storage.data(), count);
    if (!Mathematics::PointInPolygon(Mathematics::Vector2(worldX, worldZ), points))
        return 0.0f;

    const float feather = std::max(poly.Meta[1], 0.0f);
    if (feather <= 1e-3f)
        return 1.0f;
    float minEdgeDist = 1e20f;
    for (std::size_t i = 0, j = points.size() - 1; i < points.size(); j = i++)
        minEdgeDist = std::min(minEdgeDist,
                               SegmentDistance(worldX, worldZ, points[i].x, points[i].y, points[j].x, points[j].y));
    return SmoothStep(0.0f, feather, minEdgeDist);
}

float FlowMapFootprintWeight(float worldX, float worldZ, const OceanFlowMapSourceGPU& source)
{
    const float halfX = std::max(source.OriginExtent[2], 1e-3f);
    const float halfZ = std::max(source.OriginExtent[3], 1e-3f);
    const float dx = std::abs(worldX - source.OriginExtent[0]);
    const float dz = std::abs(worldZ - source.OriginExtent[1]);
    if (dx > halfX || dz > halfZ)
        return 0.0f;
    const float feather = std::max(source.StrengthFeather[1], 0.0f);
    if (feather <= 1e-3f)
        return 1.0f;
    return SmoothStep(0.0f, feather, std::min(halfX - dx, halfZ - dz));
}

} // anonymous namespace

bool OceanFlowSim::Initialize(IDevice* device)
{
    // The cascade bakes into a narrow storage format, which this backend's
    // storage-image list does not include; the texture would be refused at
    // creation and its invalid handle would poison every submit that binds
    // it. Declining here keeps the surface: the caller already degrades.
    if (device && !device->GetCapabilities().supportsNarrowStorageFormats)
        return false;
    if (m_Ready)
        return true;
    if (!device)
        return false;
    m_Device = device;

    // RG16F flow cascade (world XZ velocity). Single persistent array (the bake is
    // stateless, no ping-pong).
    if (!m_Flow.Initialize(device, kFlowResolution, kFlowLodCount, TextureFormat::R16G16_FLOAT,
                           kFlowBaseScale, "Ocean_Flow"))
    {
        Logger::Log::Warning("OceanFlowSim: flow cascade allocation failed");
        return false;
    }
    m_Resolution = m_Flow.GetResolution();
    m_LodCount = m_Flow.GetLodCount();

    m_Sampler = device->CreateSampler(SamplerDesc::MaterialLinearClamp("Ocean_Flow_Sampler"));
    if (!m_Sampler.IsValid())
        return false;
    m_DummyFlowMapTexture = CreateNeutralFlowMapTexture(device);
    if (!m_DummyFlowMapTexture.IsValid())
        return false;

    const fs::path shaderDir = OceanShaderDirectory("ocean_flow_sim.comp");
    if (shaderDir.empty())
    {
        Logger::Log::Error("OceanFlowSim: ocean_flow_sim.comp not found");
        return false;
    }
    const fs::path includeDir = shaderDir.parent_path(); // Assets/Shaders, for "Ocean/..." includes
    const fs::path cacheRoot = fs::path(".Cache") / "Shaders";

    ShaderProgramCompileRequest req{};
    req.debugName = "ocean_flow_sim";
    req.baseDirectory = shaderDir;
    req.cacheRoot = cacheRoot;
    req.includeDirs = {includeDir};
    req.stages = {{"cs", "ocean_flow_sim.comp", "main", {}}};

    ShaderProgramCompileResult result{};
    std::string err;
    if (!LoadOceanShaderProgram(req, device->PreferredShaderSource(), result, &err))
    {
        Logger::Log::Error("OceanFlowSim: compile failed: {}", err);
        return false;
    }
    auto it = result.stageBytes.find("cs");
    if (it == result.stageBytes.end() || it->second.empty())
    {
        Logger::Log::Error("OceanFlowSim: no SPIR-V");
        return false;
    }
    auto shaderBytes = std::make_shared<const std::vector<uint8_t>>(std::move(it->second));

    // Layout: UBO(0), output-flow storage image(1), flow-map samplers(2..5).
    m_Layout = DescriptorSetLayoutDesc{};
    m_Layout.debugName = "OceanFlow_Set0";
    auto addBinding = [&](uint32 binding, DescriptorType type) {
        DescriptorBinding b{};
        b.binding = binding;
        b.type = type;
        b.count = 1;
        b.shaderStages = kShaderStageCompute;
        m_Layout.bindings.push_back(b);
    };
    addBinding(0, DescriptorType::UniformBuffer);
    addBinding(1, DescriptorType::StorageImage);
    for (uint32 i = 0; i < kMaxOceanFlowMapSources; ++i)
        addBinding(2u + i, DescriptorType::CombinedImageSampler);

    ComputePipelineDesc cd{};
    cd.ComputeShader = shaderBytes;
    Rendering::ApplyMetaImageShapeToLayout(result.meta, m_Layout);
    cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
    cd.DebugName = "OceanFlow";
    m_Pipe = m_Device->InternComputePipeline(cd);
    if (!m_Pipe.IsValid())
        return false;

    m_Ready = true;
    Logger::Log::Info("OceanFlowSim: ready ({}x{} x{} LODs)", m_Resolution, m_Resolution, m_LodCount);
    return true;
}

bool OceanFlowSim::BeginFrame(float cameraX, float cameraZ)
{
    if (m_Flow.SnapToCamera(cameraX, cameraZ))
        m_DispatchDirty = true;
    return m_DispatchDirty;
}

void OceanFlowSim::SetSources(const OceanFlowSourceGPU* sources, uint32 count)
{
    const uint32 n = sources ? std::min(count, kMaxOceanFlowSources) : 0u;
    const bool changed =
        m_Params.SourceCount != n ||
        (n > 0 && std::memcmp(m_Params.Sources, sources,
                              static_cast<size_t>(n) * sizeof(OceanFlowSourceGPU)) != 0);
    m_Params.SourceCount = n;
    if (sources && n > 0)
        std::memcpy(m_Params.Sources, sources, static_cast<size_t>(n) * sizeof(OceanFlowSourceGPU));
    if (changed)
        m_DispatchDirty = true;
}

void OceanFlowSim::SetPolygonSources(const OceanFlowPolygonGPU* polygons, uint32 count)
{
    const uint32 n = polygons ? std::min(count, kMaxOceanFlowPolygons) : 0u;
    const bool changed =
        m_Params.PolygonCount != n ||
        (n > 0 && std::memcmp(m_Params.Polygons, polygons,
                              static_cast<size_t>(n) * sizeof(OceanFlowPolygonGPU)) != 0);
    m_Params.PolygonCount = n;
    if (polygons && n > 0)
        std::memcpy(m_Params.Polygons, polygons,
                    static_cast<size_t>(n) * sizeof(OceanFlowPolygonGPU));
    if (changed)
        m_DispatchDirty = true;
}

void OceanFlowSim::SetFlowMapSources(const OceanFlowMapSourceGPU* sources,
                                     const TextureHandle* textures,
                                     uint32 count,
                                     const OceanCpuTextureRG* cpuTextures)
{
    const uint32 n = (sources && textures) ? std::min(count, kMaxOceanFlowMapSources) : 0u;
    bool changed =
        m_Params.FlowMapCount != n ||
        (n > 0 && std::memcmp(m_Params.FlowMaps, sources,
                              static_cast<size_t>(n) * sizeof(OceanFlowMapSourceGPU)) != 0);
    for (uint32 i = 0; i < n; ++i)
        changed = changed || m_FlowMapTextures[i] != textures[i];

    m_Params.FlowMapCount = n;
    m_FlowMapTextures.fill({});
    m_FlowMapCpuTextures.fill({});

    if (n == 0)
    {
        if (changed)
            m_DispatchDirty = true;
        return;
    }

    std::memcpy(m_Params.FlowMaps, sources,
                static_cast<size_t>(n) * sizeof(OceanFlowMapSourceGPU));
    for (uint32 i = 0; i < n; ++i)
    {
        m_FlowMapTextures[i] = textures[i];
        if (cpuTextures)
            m_FlowMapCpuTextures[i] = cpuTextures[i];
    }
    if (changed)
        m_DispatchDirty = true;
}

OceanCurrentSample OceanFlowSim::SampleFlow(float worldX, float worldZ) const
{
    OceanCurrentSample sample{};
    sample.Valid = true;

    const uint32 rectCount = std::min(m_Params.SourceCount, kMaxOceanFlowSources);
    for (uint32 i = 0u; i < rectCount; ++i)
    {
        const OceanFlowSourceGPU& src = m_Params.Sources[i];
        const float halfX = std::max(src.OriginExtent[2], 1e-3f);
        const float halfZ = std::max(src.OriginExtent[3], 1e-3f);
        const float dx = std::abs(worldX - src.OriginExtent[0]);
        const float dz = std::abs(worldZ - src.OriginExtent[1]);
        if (dx > halfX || dz > halfZ)
            continue;

        const float edgeX = 1.0f - SmoothStep(0.7f, 1.0f, dx / halfX);
        const float edgeZ = 1.0f - SmoothStep(0.7f, 1.0f, dz / halfZ);
        const float weight = std::min(edgeX, edgeZ);
        sample.FlowX += src.FlowVelocity[0] * weight;
        sample.FlowZ += src.FlowVelocity[1] * weight;
        ++sample.RectSourceCount;
    }

    const uint32 polygonCount = std::min(m_Params.PolygonCount, kMaxOceanFlowPolygons);
    for (uint32 i = 0u; i < polygonCount; ++i)
    {
        const OceanFlowPolygonGPU& poly = m_Params.Polygons[i];
        const float weight = PolygonWeight(worldX, worldZ, poly);
        if (weight <= 0.0f)
            continue;
        sample.FlowX += poly.FlowVelocity[0] * weight;
        sample.FlowZ += poly.FlowVelocity[1] * weight;
        ++sample.PolygonSourceCount;
    }

    const uint32 mapCount = std::min(m_Params.FlowMapCount, kMaxOceanFlowMapSources);
    for (uint32 i = 0u; i < mapCount; ++i)
    {
        const OceanFlowMapSourceGPU& map = m_Params.FlowMaps[i];
        const float weight = FlowMapFootprintWeight(worldX, worldZ, map);
        if (weight <= 0.0f)
            continue;
        float encodedX = 0.5f;
        float encodedZ = 0.5f;
        const float halfX = std::max(map.OriginExtent[2], 1e-3f);
        const float halfZ = std::max(map.OriginExtent[3], 1e-3f);
        const float u = (worldX - map.OriginExtent[0]) / (2.0f * halfX) + 0.5f;
        const float v = (worldZ - map.OriginExtent[1]) / (2.0f * halfZ) + 0.5f;
        m_FlowMapCpuTextures[i].SampleLinear(u, v, encodedX, encodedZ);
        const float decodedX = (encodedX - 0.5f) * 2.0f;
        const float decodedZ = (encodedZ - 0.5f) * 2.0f;
        sample.FlowX += (decodedX * map.StrengthFeather[0] + map.StrengthFeather[2]) * weight;
        sample.FlowZ += (decodedZ * map.StrengthFeather[0] + map.StrengthFeather[3]) * weight;
        sample.TextureFlowMapContributes = true;
        ++sample.TextureFlowMapCount;
    }

    return sample;
}

void OceanFlowSim::FillParams(OceanFlowParamsGPU& out)
{
    m_Params.Resolution = m_Resolution;
    m_Params.LodCount = m_LodCount;
    // The layout BeginFrame snapped this frame; fixed before the pass is declared,
    // so the bound layout matches the dispatch.
    m_Params.Cascade = m_Flow.GetLayout();
    out = m_Params;
}

void OceanFlowSim::RecordDispatch(IDevice* device, CommandList* cl,
                                  BufferHandle paramsBuffer, uint64 paramsOffset)
{
    if (!m_Ready || !cl || !device || !paramsBuffer.IsValid())
        return;

    const TextureHandle flow = m_Flow.GetTexture();
    const uint32 layers = m_LodCount;

    PipelineHandle pipe = device->GetOrCreateComputePipeline(m_Pipe);
    if (!pipe)
        return;

    // Sync is graph-derived: the pass declares Write(flow) so the render graph
    // emits the ShaderResource->General entry transition, the first declared
    // reader (foam / the surface span) restores ShaderReadOnly, and MarkOutput
    // covers reader-less frames. No manual barriers in here.
    cl->SetPipeline(pipe);

    DescriptorSetDesc dsDesc{};
    dsDesc.layout = m_Layout;
    dsDesc.transient = true;
    dsDesc.debugName = "OceanFlow_DS";
    DescriptorSetHandle ds = device->CreateDescriptorSet(dsDesc);

    device->UpdateBufferBinding(ds, 0, paramsBuffer, paramsOffset, sizeof(OceanFlowParamsGPU));
    device->UpdateStorageImageBinding(ds, 1, flow);
    for (uint32 i = 0; i < kMaxOceanFlowMapSources; ++i)
    {
        TextureHandle mapTexture = m_FlowMapTextures[i].IsValid()
                                       ? m_FlowMapTextures[i]
                                       : m_DummyFlowMapTexture;
        device->UpdateCombinedImageSamplerBinding(ds, 2u + i, mapTexture, m_Sampler);
    }

    cl->BindDescriptorSet(0, ds, pipe);

    const uint32 g8 = (m_Resolution + 7u) / 8u;
    cl->Dispatch(g8, g8, layers);
    m_DispatchDirty = false;
}

RenderGraph::RGTexture OceanFlowSim::ImportRG(RenderGraph::RGFrame& frame) const
{
    return m_Flow.ImportRG(frame);
}

} // namespace GameEngine::Ocean
