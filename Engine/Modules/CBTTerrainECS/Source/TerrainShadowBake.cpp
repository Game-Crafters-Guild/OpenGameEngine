#include "CBTTerrainECS/TerrainShadowBake.h"

#include "Engine/Rendering/Pipeline/Nodes/PipelineNodeUtils.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Engine/Rendering/TerrainShadowMap.h"
#include "Logger/Logger.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <string>

namespace GameEngine::CBTTerrainECS
{

namespace
{
namespace RG = Rendering::RenderGraph;

// After CBT.Update (kEarlySetup + 1), ahead of every shading pass that reads the map.
constexpr int32_t kBakePhase = Rendering::PassPhase::kEarlySetup + 2;

// GLSL mirror: TerrainShadowBakeParams in terrain_shadow_bake.comp (std140).
struct BakeParamsUBO
{
    float CenterTerrain[4]; // grid centre X, Z in the terrain's local frame; terrain extent X, Z
    float SunTexel[4];      // sun horizontal direction X, Z; tan(elevation); texel (m)
    float Range[4];         // u of sample 0; v of line 0; samples per line; lines
    float Bake[4];          // first line of this dispatch; metres per normalized height; tan of the
                            // sun disc's lower edge's elevation
};

CBTTerrain::TerrainShadowExtent ExtentOf(const TerrainShadowBakeInputs& inputs, uint32_t heightWidth,
                                         uint32_t heightHeight)
{
    CBTTerrain::TerrainShadowExtent extent;
    extent.SizeX = inputs.SizeX;
    extent.SizeZ = inputs.SizeZ;
    extent.CellsX = heightWidth > 1u ? heightWidth - 1u : 0u;
    extent.CellsZ = heightHeight > 1u ? heightHeight - 1u : 0u;
    return extent;
}

// The primary shadow-casting directional light's direction toward the light and tan of its angular
// radius, as the cascades and the ray-traced mask take them; false when the world has none.
bool SunTowardLight(Engine::Renderer::RenderServices& rs, uint64_t worldId, float out[3], float& tanHalfAngle)
{
    const Engine::Renderer::ExtractedLight* primary =
        Engine::Renderer::SelectPrimaryDirectional(rs.GetWorldLights(worldId));
    if (!primary || primary->castsShadows == 0)
        return false;
    out[0] = -primary->directionWS[0];
    out[1] = -primary->directionWS[1];
    out[2] = -primary->directionWS[2];
    tanHalfAngle = Engine::Renderer::ResolveShadowTanHalfAngle(primary->shadowAngularDiameter);
    return true;
}
} // namespace

TerrainShadowBake::TerrainShadowBake() = default;
TerrainShadowBake::~TerrainShadowBake() = default;

void TerrainShadowBake::DeclareForView(Engine::Renderer::RenderServices& rs, RG::RGFrame& frame,
                                       Rendering::ViewId viewId, uint64_t worldId,
                                       const TerrainShadowBakeInputs& inputs)
{
    if (m_Frame.Graph != &frame || m_Frame.FrameIndex != frame.FrameIndex())
    {
        m_Frame = FrameMap{};
        m_Frame.Graph = &frame;
        m_Frame.FrameIndex = frame.FrameIndex();
        m_Frame.Present = DeclareFrameMap(rs, frame, worldId, inputs);
        if (!m_Frame.Present)
            ReleaseMapBindless(rs);
    }
    if (m_Frame.Present)
        Publish(rs, frame, viewId, inputs);
}

void TerrainShadowBake::OnDeviceRebuilt()
{
    // Nothing is destroyed here: the rebuild already freed the pipeline, the sampler and the pool's
    // map, and these handles only look valid. The height sampler is left as it is: clearing the load
    // latch makes EnsurePipeline create it again before any use.
    m_LoadAttempted = false;
    m_PipelineId = {};
    m_Meta.reset();
    m_Set0Layout = {};
    m_MapTexture = {};
    m_MapBindlessIndex = 0;
    m_MapSide = 0;
    // A map and height texture re-created on the rebuilt device may come back with the handle ids the
    // record holds, which would make the next frame's inputs compare equal and skip the bake.
    m_Baked.reset();
    m_Frame = FrameMap{};
}

void TerrainShadowBake::ReleaseMapBindless(Engine::Renderer::RenderServices& rs)
{
    // On a frame with no map the pool may age the texture out: its slot is dropped first, so it never
    // describes a freed image, and the next map registers afresh.
    if (m_MapTexture.IsValid())
        rs.Textures().InvalidateBindless(m_MapTexture);
    m_MapTexture = {};
    m_MapBindlessIndex = 0;
}

bool TerrainShadowBake::EnsurePipeline(Rendering::IDevice& device)
{
    if (m_LoadAttempted)
        return m_PipelineId.IsValid();
    m_LoadAttempted = true;

    Rendering::ShaderPackage pkg{};
    std::string loadErr;
    if (!Rendering::LoadShaderPkg("Shaders/terrain_shadow_bake.shaderpkg", device.PreferredShaderSource(), pkg,
                                  &loadErr))
    {
        Logger::Log::Error("TerrainShadowBake: failed to load terrain_shadow_bake.shaderpkg ({}); the terrain "
                           "casts no shadow until the shader package is staged",
                           loadErr);
        return false;
    }
    auto cs = pkg.stageBytes.find("cs");
    if (cs == pkg.stageBytes.end() || cs->second.empty())
    {
        Logger::Log::Error("TerrainShadowBake: terrain_shadow_bake.shaderpkg has no compute stage");
        return false;
    }
    m_Meta = std::make_unique<Rendering::ShaderMeta>(std::move(pkg.meta));

    Rendering::ComputePipelineDesc cd{};
    cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(cs->second));
    cd.DebugName = "TerrainShadow.Bake";
    auto captureSet0 = [this](uint32_t setIndex, Rendering::DescriptorSetLayoutDesc& layout)
    {
        if (setIndex == 0)
            m_Set0Layout = layout;
    };
    std::string err;
    Rendering::MaterialHelper::ApplyShaderMetaToComputeDesc(device, *m_Meta, cd,
                                                            Rendering::MaterialBuilder::MergeMode::Auto,
                                                            {true, 128}, captureSet0, &err);
    m_PipelineId = device.InternComputePipeline(std::move(cd));
    m_HeightSampler = device.CreateSampler(Rendering::SamplerDesc::MaterialLinearClamp("TerrainShadow.Height"));
    return m_PipelineId.IsValid() && m_HeightSampler.IsValid();
}

bool TerrainShadowBake::DeclareFrameMap(Engine::Renderer::RenderServices& rs, RG::RGFrame& frame,
                                        uint64_t worldId, const TerrainShadowBakeInputs& inputs)
{
    Rendering::IDevice* device = rs.GetDevice();
    if (!device || !inputs.Source.CastShadows || !inputs.Source.HeightTexture.IsValid() ||
        inputs.Source.HeightBindlessIndex == 0u ||
        !rs.Textures().IsBindlessEnabled())
        return false;
    float towardSun[3];
    float tanHalfAngle = 0.0f;
    if (!SunTowardLight(rs, worldId, towardSun, tanHalfAngle))
        return false;

    uint32_t heightWidth = 0;
    uint32_t heightHeight = 0;
    device->GetTextureSize(inputs.Source.HeightTexture, heightWidth, heightHeight);
    const CBTTerrain::TerrainShadowExtent extent = ExtentOf(inputs, heightWidth, heightHeight);
    const std::optional<CBTTerrain::TerrainShadowGrid> grid = CBTTerrain::ComputeTerrainShadowGrid(extent, towardSun, tanHalfAngle);
    const uint32_t side = CBTTerrain::TerrainShadowMapSide(extent);
    if (!grid || side == 0u || !EnsurePipeline(*device))
        return false;

    Rendering::TextureDesc desc{};
    desc.width = side;
    desc.height = side;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.sampleCount = 1;
    desc.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16_FLOAT);
    desc.usage = static_cast<uint32_t>(Rendering::TextureUsage::UnorderedAccess |
                                       Rendering::TextureUsage::ShaderResource |
                                       Rendering::TextureUsage::TransferSrc);
    // GENERAL for its whole lifetime: the bake stores to it and every receiver samples it through
    // the bindless array, so the sampled descriptor claims GENERAL rather than ping-ponging layouts.
    desc.sampledInGeneralLayout = true;
    desc.initialState = Rendering::ResourceState::UnorderedAccess;
    desc.debugName = "TerrainShadow.ClearanceMap";
    bool fresh = false;
    const RG::RGTexture map = frame.ImportPersistentTexture("TerrainShadow.ClearanceMap", desc, &fresh);
    if (!map.IsValid())
        return false;

    // A new physical (first use, a resize, a device rebuild, an age-out) drops the old bindless slot
    // before registering the new one, so the slot never outlives the image it describes.
    const Rendering::TextureHandle physical = frame.PhysicalTexture(map);
    if (physical != m_MapTexture)
    {
        if (m_MapTexture.IsValid())
            rs.Textures().InvalidateBindless(m_MapTexture);
        m_MapTexture = physical;
        m_MapBindlessIndex = physical.IsValid() ? rs.Textures().GetBindlessIndex(physical) : 0u;
    }
    m_MapSide = side;
    if (m_MapBindlessIndex == 0u)
        return false;

    const BakeKey key{*grid, extent, inputs.HeightScale, inputs.Source.HeightTexture.id, physical.id};
    CBTTerrain::TerrainShadowLines lines{};
    const bool full = fresh || !m_Baked || !(*m_Baked == key);
    if (full)
        lines = {0u, grid->SamplesV};
    else
        lines = CBTTerrain::TerrainShadowDirtyLines(*grid, extent, inputs.DirtyMinU, inputs.DirtyMinV,
                                                    inputs.DirtyMaxU, inputs.DirtyMaxV);
    if (lines.Count > 0u)
    {
        DeclareBakePass(frame, map, *grid, inputs, lines);
        if (full)
        {
            frame.MarkPersistentTextureInitialized(map);
            m_Baked = key;
        }
    }

    m_Frame.Map = map;
    m_Frame.Grid = *grid;
    return true;
}

void TerrainShadowBake::DeclareBakePass(RG::RGFrame& frame, RG::RGTexture map,
                                        const CBTTerrain::TerrainShadowGrid& grid,
                                        const TerrainShadowBakeInputs& inputs, CBTTerrain::TerrainShadowLines lines)
{
    auto ub = frame.AllocUpload<BakeParamsUBO>();
    if (!ub.Valid())
        return;
    BakeParamsUBO params{};
    params.CenterTerrain[0] = 0.5f * inputs.SizeX;
    params.CenterTerrain[1] = 0.5f * inputs.SizeZ;
    params.CenterTerrain[2] = inputs.SizeX;
    params.CenterTerrain[3] = inputs.SizeZ;
    params.SunTexel[0] = grid.SunX;
    params.SunTexel[1] = grid.SunZ;
    params.SunTexel[2] = grid.TanElevation;
    params.SunTexel[3] = grid.Texel;
    params.Range[0] = grid.UMin;
    params.Range[1] = grid.VMin;
    params.Range[2] = static_cast<float>(grid.SamplesU);
    params.Range[3] = static_cast<float>(grid.SamplesV);
    params.Bake[0] = static_cast<float>(lines.First);
    params.Bake[1] = inputs.HeightScale;
    params.Bake[2] = grid.TanLowerEdge;
    *ub.Ptr = params;

    const RG::RGTexture height = frame.ImportExternalTexture("CBT.HeightSource", inputs.Source.HeightTexture,
                                                             Rendering::ResourceState::ShaderResource);
    // One workgroup per line (terrain_shadow_bake.comp).
    const uint32_t groups = lines.Count;
    const RG::RGPass bake = frame.AddPass(
        "TerrainShadow.Bake", kBakePhase,
        [&](RG::RGPassBuilder& p)
        {
            p.Read(height, RG::RGTextureRead::SampledCompute);
            p.Write(map, RG::RGTextureWrite::Storage);
        },
        [this, height, map, ubBuffer = ub.Buffer, ubOffset = ub.Offset, groups](RG::RGContext& ctx)
        {
            Rendering::IDevice* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            const Rendering::TextureHandle heightTex = ctx.GetTexture(height);
            const Rendering::TextureHandle mapTex = ctx.GetTexture(map);
            if (!dev || !cl || !heightTex.IsValid() || !mapTex.IsValid())
                return;
            Rendering::DescriptorSetDesc dsDesc{};
            dsDesc.layout = m_Set0Layout;
            dsDesc.transient = true;
            dsDesc.debugName = "TerrainShadow.Bake.Set0";
            const Rendering::DescriptorSetHandle ds = dev->CreateDescriptorSet(dsDesc);
            Rendering::NamedDescriptorWriter wd(dev, ds, *m_Meta, 0);
            wd.AddUniformBuffer("TerrainShadowBakeParams", ubBuffer, ubOffset, sizeof(BakeParamsUBO));
            wd.AddCombinedImageSampler("uHeight", heightTex, m_HeightSampler);
            wd.Flush();
            if (!Engine::Renderer::Pipeline::Nodes::Detail::BindStorageImageByName(dev, ds, *m_Meta, "uClearance",
                                                                                   mapTex))
            {
                Logger::Log::Error("TerrainShadowBake: terrain_shadow_bake.comp reflects no 'uClearance' image; "
                                   "bake skipped");
                return;
            }
            const Rendering::PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(m_PipelineId);
            if (!pipe.IsValid())
                return;
            cl->SetMarker("TerrainShadow.Bake");
            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            cl->Dispatch(groups, 1, 1);
        });
    if (inputs.HeightUploadPass != RG::kInvalidId && bake.IsValid())
        frame.AddOrderingEdge(RG::RGPass{inputs.HeightUploadPass}, bake);
}

void TerrainShadowBake::Publish(Engine::Renderer::RenderServices& rs, RG::RGFrame& frame, Rendering::ViewId viewId,
                                const TerrainShadowBakeInputs& inputs)
{
    auto* shadows = rs.GetFeature<Engine::Renderer::ShadowMapRenderFeature>();
    if (!shadows)
        return;
    // The view's shading passes sample the map through the bindless array; the world pass declares
    // the read so the graph orders it after this frame's bake.
    rs.EmitForwardSampledRead(frame, viewId, m_Frame.Map);

    Engine::Renderer::TerrainShadowMap published{};
    published.Map = m_Frame.Map;
    published.MapTexture = m_MapTexture;
    published.MapBindlessIndex = m_MapBindlessIndex;
    published.MapSide = m_MapSide;
    published.HeightTexture = inputs.Source.HeightTexture;
    published.HeightBindlessIndex = inputs.Source.HeightBindlessIndex;
    published.TerrainX = inputs.TerrainX;
    published.TerrainZ = inputs.TerrainZ;
    published.BaseY = inputs.BaseY;
    published.TerrainSizeX = inputs.SizeX;
    published.TerrainSizeZ = inputs.SizeZ;
    published.HeightScale = inputs.HeightScale;
    published.SunX = m_Frame.Grid.SunX;
    published.SunZ = m_Frame.Grid.SunZ;
    published.TanElevation = m_Frame.Grid.TanElevation;
    published.Texel = m_Frame.Grid.Texel;
    published.UMin = m_Frame.Grid.UMin;
    published.VMin = m_Frame.Grid.VMin;
    published.SamplesU = static_cast<float>(m_Frame.Grid.SamplesU);
    published.SamplesV = static_cast<float>(m_Frame.Grid.SamplesV);
    shadows->PublishTerrainShadowMap(viewId, frame.FrameIndex(), published, rs);
}

} // namespace GameEngine::CBTTerrainECS
