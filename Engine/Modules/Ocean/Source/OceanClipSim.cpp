#include "Ocean/OceanClipSim.h"
#include "Ocean/OceanShaderProgram.h"
#include "Rendering/Materials/ShaderLayoutShape.h"
#include "OceanShaderDirectory.h"

#include "Logger/Logger.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/ShaderCompileService.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

namespace GameEngine::Ocean
{

namespace fs = std::filesystem;
using namespace ::GameEngine::Rendering;

namespace
{

// Grow `bounds` (min X, min Z, max X, max Z) to hold `box` in the same order.
void ExpandBounds(float bounds[4], bool& hasBounds, const float box[4])
{
    bounds[0] = hasBounds ? std::min(bounds[0], box[0]) : box[0];
    bounds[1] = hasBounds ? std::min(bounds[1], box[1]) : box[1];
    bounds[2] = hasBounds ? std::max(bounds[2], box[2]) : box[2];
    bounds[3] = hasBounds ? std::max(bounds[3], box[3]) : box[3];
    hasBounds = true;
}
} // anonymous namespace

bool OceanClipSim::Initialize(IDevice* device)
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

    // R8_UNORM clip cascade (surface clip state). Single persistent array (the bake
    // is stateless, no ping-pong).
    if (!m_Clip.Initialize(device, kClipResolution, kClipLodCount, TextureFormat::R8_UNORM,
                           kClipBaseScale, "Ocean_Clip"))
    {
        Logger::Log::Warning("OceanClipSim: clip cascade allocation failed");
        return false;
    }
    m_Resolution = m_Clip.GetResolution();
    m_LodCount = m_Clip.GetLodCount();

    m_Sampler = device->CreateSampler(SamplerDesc::MaterialLinearClamp("Ocean_Clip_Sampler"));
    if (!m_Sampler.IsValid())
        return false;

    const fs::path shaderDir = OceanShaderDirectory("ocean_clip_sim.comp");
    if (shaderDir.empty())
    {
        Logger::Log::Error("OceanClipSim: ocean_clip_sim.comp not found");
        return false;
    }
    const fs::path includeDir = shaderDir.parent_path(); // Assets/Shaders, for "Ocean/..." includes
    const fs::path cacheRoot = fs::path(".Cache") / "Shaders";

    ShaderProgramCompileRequest req{};
    req.debugName = "ocean_clip_sim";
    req.baseDirectory = shaderDir;
    req.cacheRoot = cacheRoot;
    req.includeDirs = {includeDir};
    req.stages = {{"cs", "ocean_clip_sim.comp", "main", {}}};

    ShaderProgramCompileResult result{};
    std::string err;
    if (!LoadOceanShaderProgram(req, device->PreferredShaderSource(), result, &err))
    {
        Logger::Log::Error("OceanClipSim: compile failed: {}", err);
        return false;
    }
    auto it = result.stageBytes.find("cs");
    if (it == result.stageBytes.end() || it->second.empty())
    {
        Logger::Log::Error("OceanClipSim: no SPIR-V");
        return false;
    }
    auto shaderBytes = std::make_shared<const std::vector<uint8_t>>(std::move(it->second));

    // Layout: UBO(0), output-clip storage image(1).
    m_Layout = DescriptorSetLayoutDesc{};
    m_Layout.debugName = "OceanClip_Set0";
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

    ComputePipelineDesc cd{};
    cd.ComputeShader = shaderBytes;
    Rendering::ApplyMetaImageShapeToLayout(result.meta, m_Layout);
    cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
    cd.DebugName = "OceanClip";
    m_Pipe = m_Device->InternComputePipeline(cd);
    if (!m_Pipe.IsValid())
        return false;

    m_Ready = true;
    Logger::Log::Info("OceanClipSim: ready ({}x{} x{} LODs)", m_Resolution, m_Resolution, m_LodCount);
    return true;
}

bool OceanClipSim::BeginFrame(float cameraX, float cameraZ)
{
    // The union of every source's XZ bounds: outside it the bake writes the
    // default state, so a coarsest layer over it holds all authored water.
    float bounds[4] = {};
    bool hasBounds = false;
    for (uint32 i = 0; i < m_Params.SourceCount; ++i)
    {
        const float* rect = m_Params.Sources[i].OriginExtent;
        const float box[4] = {rect[0] - rect[2], rect[1] - rect[3], rect[0] + rect[2], rect[1] + rect[3]};
        ExpandBounds(bounds, hasBounds, box);
    }
    for (uint32 i = 0; i < m_Params.PolygonCount; ++i)
        ExpandBounds(bounds, hasBounds, m_Params.Polygons[i].Bounds);
    if (m_HasSpline && m_HasSplineBounds)
        ExpandBounds(bounds, hasBounds, m_SplineBounds);
    if (hasBounds)
        m_Clip.AnchorCoarsestLevel(bounds[0], bounds[1], bounds[2], bounds[3]);
    else
        m_Clip.ClearCoarsestAnchor();

    if (m_Clip.SnapToCamera(cameraX, cameraZ))
        m_DispatchDirty = true;
    return m_DispatchDirty;
}

void OceanClipSim::SetSources(const OceanClipSourceGPU* sources, uint32 count)
{
    const uint32 n = sources ? std::min(count, kMaxOceanClipSources) : 0u;
    const bool changed =
        m_Params.SourceCount != n ||
        (n > 0 && std::memcmp(m_Params.Sources, sources,
                              static_cast<size_t>(n) * sizeof(OceanClipSourceGPU)) != 0);
    m_Params.SourceCount = n;
    if (sources && n > 0)
        std::memcpy(m_Params.Sources, sources, static_cast<size_t>(n) * sizeof(OceanClipSourceGPU));
    if (changed)
        m_DispatchDirty = true;
}

void OceanClipSim::SetPolygonSources(const OceanClipPolygonGPU* polygons, uint32 count)
{
    const uint32 n = polygons ? std::min(count, kMaxOceanClipPolygons) : 0u;
    const bool changed =
        m_Params.PolygonCount != n ||
        (n > 0 && std::memcmp(m_Params.Polygons, polygons,
                              static_cast<size_t>(n) * sizeof(OceanClipPolygonGPU)) != 0);
    m_Params.PolygonCount = n;
    if (polygons && n > 0)
        std::memcpy(m_Params.Polygons, polygons,
                    static_cast<size_t>(n) * sizeof(OceanClipPolygonGPU));
    if (changed)
        m_DispatchDirty = true;
}

void OceanClipSim::FillParams(OceanClipParamsGPU& out)
{
    m_Params.Resolution = m_Resolution;
    m_Params.LodCount = m_LodCount;
    m_Params.Cascade = m_Clip.GetLayout();
    out = m_Params;
}

void OceanClipSim::RecordDispatch(IDevice* device, CommandList* cl,
                                  BufferHandle paramsBuffer, uint64 paramsOffset)
{
    if (!m_Ready || !cl || !device || !paramsBuffer.IsValid())
        return;

    const TextureHandle clip = m_Clip.GetTexture();
    const uint32 layers = m_LodCount;

    PipelineHandle pipe = device->GetOrCreateComputePipeline(m_Pipe);
    if (!pipe)
        return;

    // Sync is graph-derived: the pass declares Write on the cascade, the first
    // declared reader restores ShaderReadOnly, and MarkOutput covers reader-less
    // frames. No manual barriers in here.

    cl->SetPipeline(pipe);

    DescriptorSetDesc dsDesc{};
    dsDesc.layout = m_Layout;
    dsDesc.transient = true;
    dsDesc.debugName = "OceanClip_DS";
    DescriptorSetHandle ds = device->CreateDescriptorSet(dsDesc);

    device->UpdateBufferBinding(ds, 0, paramsBuffer, paramsOffset, sizeof(OceanClipParamsGPU));
    device->UpdateStorageImageBinding(ds, 1, clip);

    cl->BindDescriptorSet(0, ds, pipe);

    const uint32 g8 = (m_Resolution + 7u) / 8u;
    cl->Dispatch(g8, g8, layers);

    m_DispatchDirty = false;
}

RenderGraph::RGTexture OceanClipSim::ImportRG(RenderGraph::RGFrame& frame) const
{
    return m_Clip.ImportRG(frame);
}

} // namespace GameEngine::Ocean
