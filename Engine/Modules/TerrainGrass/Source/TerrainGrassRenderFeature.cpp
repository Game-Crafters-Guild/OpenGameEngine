#include "TerrainGrass/TerrainGrassRenderFeature.h"

#include "TerrainGrass/GrassDrawMode.h"
#include "TerrainGrass/GrassPlacementElisionInputs.h"
#include "TerrainECS/TerrainRenderFeature.h"
#include "Engine/Rendering/CameraUtils.h"
#include "Engine/Rendering/DepthDrawRecorder.h"
#include "Engine/Rendering/DrawBindings.h"
#include "Engine/Rendering/IEnvironmentSource.h"
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/PipelineVariantCache.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SkyRenderFeature.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/BarrierMapping.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Common/Frustum.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Geometry/VertexAttributeFlags.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/Materials/ShaderVariantKey.h"
#include "Rendering/Utils/BufferHelpers.h"
#include "Terrain/TerrainTypes.h"
#include "AssetCore/GUID.h"
#include "AssetCore/SharedFileRead.h"
#include "Components/Terrain/TerrainGrass.h"
#include "Core/Application.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <vector>

namespace GameEngine::TerrainGrass
{

namespace
{
// One registered material per grass draw mode, all composing the same surface shader.
// alphaMode drives the pipeline class (Blend: blending, no depth write; every other mode is
// Opaque: depth write + test), and the user keyword selects the surface's alpha path
// (GE_USER_GRASS_DITHER / GE_USER_GRASS_A2C / GE_USER_GRASS_OPAQUE).
//
// GRASS_OPAQUE is a NEGATIVE keyword: it names the path that compiles the card cutout OUT. It has
// to be its own variant rather than the keyword-free default, because a discard is a static
// property of a fragment module — the hardware defers the depth test for any module that carries
// an OpKill, whether or not the kill can be reached. The Blend mode keeps the keyword-free default
// for the same reason inverted: it needs the cutout, so it must keep the instruction.
static const GUID kTerrainGrassMaterialGuid = GUID::Derive(GUID{}, "terrain_grass/default_material");
static const GUID kTerrainGrassOpaqueMaterialGuid = GUID::Derive(GUID{}, "terrain_grass/opaque_material");
static const GUID kTerrainGrassDitherMaterialGuid = GUID::Derive(GUID{}, "terrain_grass/dither_material");
static const GUID kTerrainGrassA2CMaterialGuid = GUID::Derive(GUID{}, "terrain_grass/a2c_material");

struct BladeVertex
{
    float Side = 0.0f;
    float T = 0.0f;
};

// The blade draws' layout as VertexAttributeFlags: none of the mesh streams. The blade vertex (BladeVertex)
// rides the colour pipeline's own vertex input, which the prepass heads copy, and the instances are fetched
// from set 2.
constexpr Rendering::VertexAttributeFlags kBladeColourVertexFlags = Rendering::VertexAttributeFlags::None;

struct GrassBladeInstanceGPU
{
    float WorldX = 0.0f;
    float WorldZ = 0.0f;
    float RootY = 0.0f;
    float BladeHeight = 0.0f;
    float BladeWidth = 0.0f;
    float Yaw = 0.0f;
    float Rand = 0.0f;
    float GrassWeight = 0.0f;
    uint32 TerrainIndex = 0;
    float NormalX = 0.0f; // baked terrain normal (atlas terrains only; 0 otherwise)
    float NormalZ = 0.0f;
    uint32 Flags = 0;     // GrassClumpModel.h owns the bit layout (LOD, clump code, min width)
};
static_assert(sizeof(GrassBladeInstanceGPU) == 48);

Rendering::BufferHandle CreateUniformBuffer(Rendering::IDevice& device, uint64 size, const char* name)
{
    Rendering::BufferDesc desc{};
    desc.size = size;
    // Host-visible coherent + persistently mapped so a per-frame update writes via
    // a direct memcpy. A DeviceLocal UBO is filled by UpdateBuffer through a staging
    // copy submitted to the async transfer queue, which is NOT ordered against the
    // graphics-queue placement compute that reads this UBO in the same frame (the
    // graphics submit waits on no transfer timeline). On a freshly written slot
    // the compute could then read stale/zero uDensity, zeroing the density gate so
    // the placement compute places no blades. Coherent host memory + the queue-submit
    // host-write ordering guarantee make the write visible with no cross-queue
    // race. Mirrors the terrain params SSBO the same compute reads.
    desc.usage = static_cast<uint32>(Rendering::BufferUsage::Uniform);
    desc.memoryUsage = Rendering::BufferMemoryUsage::Upload;
    // FrameSlotted: every caller creates one element of a frame-slot ring.
    desc.flags = Rendering::BufferCreateFlags::PersistentlyMapped |
                 Rendering::BufferCreateFlags::FrameSlotted;
    desc.debugName = name;
    return device.CreateBuffer(desc);
}

// First named map binding and the shared sampler, mirroring terrain_grass_place.comp's compat
// declarations. Buffers occupy 0-6 (PlaceParams is 6), so the twelve maps run 7-18 and the
// sampler lands at 19.
constexpr uint32 kGrassCompatMapBinding0 = 7u;
constexpr uint32 kGrassCompatMapCount = 12u;
constexpr uint32 kGrassCompatMapSamplerBinding = 19u;

// `namedMaps` is the compat profile: it has no binding arrays, so the maps the placement
// shader samples are named bindings in this set instead of bindless indices.
Rendering::DescriptorSetLayoutDesc MakeGrassPlaceDescriptorSetLayout(bool namedMaps)
{
    Rendering::DescriptorSetLayoutDesc layout;
    layout.debugName = "TerrainGrassPlace_DSLayout";
    // The readonly flags mirror the access classes terrain_grass_place.comp declares: a
    // backend that bakes access into the layout (WebGPU) rejects the pipeline when a
    // `readonly buffer` module meets a read_write layout entry.
    layout.bindings = {
        {0, Rendering::DescriptorType::StorageBuffer, 1, Rendering::kShaderStageCompute, "TerrainParamsBuffer",
         Rendering::kDescriptorBindingReadOnlyStorage},
        // Read-write: the plan kernel writes the cell ranges the emit kernel then reads, and both
        // pipelines share this one layout.
        {1, Rendering::DescriptorType::StorageBuffer, 1, Rendering::kShaderStageCompute,
         "TerrainGrassCellPlanBuffer"},
        {2, Rendering::DescriptorType::StorageBuffer, 1, Rendering::kShaderStageCompute, "TerrainGrassInstances"},
        {3, Rendering::DescriptorType::StorageBuffer, 1, Rendering::kShaderStageCompute, "TerrainGrassIndirectArgs"},
        {4, Rendering::DescriptorType::UniformBuffer, 1, Rendering::kShaderStageCompute, "GrassAtlasParamsBuffer"},
        {5, Rendering::DescriptorType::StorageBuffer, 1, Rendering::kShaderStageCompute, "GrassAtlasRowsBuffer",
         Rendering::kDescriptorBindingReadOnlyStorage},
        {6, Rendering::DescriptorType::UniformBuffer, 1, Rendering::kShaderStageCompute, "GrassPlaceParamsBuffer"},
    };
    if (!namedMaps)
        return layout;
    // Order and binding numbers mirror the compat declarations in terrain_grass_place.comp;
    // one sampler serves all twelve, because every tap is a clamped explicit-LOD lookup.
    static constexpr const char* kMapBindingNames[] = {
        "gGrassAtlasHeight", "gGrassAtlasHeightCoarse", "gGrassAtlasNormal",
        "gGrassAtlasNormalCoarse", "gGrassAtlasSplat", "gGrassAtlasSplatCoarse",
        "gGrassHeightmap", "gGrassNormalmap", "gGrassSplatmap",
        "gGrassAtlasControl", "gGrassAtlasControlCoarse", "gGrassControlmap",
    };
    static_assert(kGrassCompatMapCount == sizeof(kMapBindingNames) / sizeof(kMapBindingNames[0]),
                  "map binding names must cover every compat map the shader declares");
    for (uint32 i = 0; i < kGrassCompatMapCount; ++i)
    {
        Rendering::DescriptorBinding b{};
        b.binding = kGrassCompatMapBinding0 + i;
        b.type = Rendering::DescriptorType::Texture;
        b.count = 1;
        b.shaderStages = Rendering::kShaderStageCompute;
        b.debugName = kMapBindingNames[i];
        // Every map is tapped through the filtering sampler below (clamped
        // explicit-LOD lookups): a compute-stage image with no filterable
        // declaration defaults to unfilterable on WebGPU, which then rejects
        // the sampler pairing at pipeline creation.
        b.imageFilterableFloat = true;
        layout.bindings.push_back(b);
    }
    layout.bindings.push_back({kGrassCompatMapSamplerBinding, Rendering::DescriptorType::Sampler, 1,
                               Rendering::kShaderStageCompute, "gGrassMapSampler"});
    return layout;
}

// One indirection row = std430 TileAtlasSlot (mirror TerrainAtlas.h / cbt_atlas.glsl CBTTileAtlasSlot).
constexpr uint32 kGrassAtlasRowBytes = 16u;

Rendering::ResourceBarrier MakeGrassPlaceReadyBarrier()
{
    return Rendering::ResourceBarrier::CreateMemoryBarrier(
        static_cast<uint64>(Rendering::PipelineStageMask::Transfer)
            | static_cast<uint64>(Rendering::PipelineStageMask::ComputeShader),
        static_cast<uint64>(Rendering::PipelineStageMask::DrawIndirect)
            | static_cast<uint64>(Rendering::PipelineStageMask::GraphicsVertex),
        static_cast<uint64>(Rendering::ResourceAccessMask::TransferWrite)
            | static_cast<uint64>(Rendering::ResourceAccessMask::ShaderWrite),
        static_cast<uint64>(Rendering::ResourceAccessMask::IndirectCommandRead)
            | static_cast<uint64>(Rendering::ResourceAccessMask::ShaderRead));
}

void BindIblResources(Engine::Renderer::RenderServices& rs,
                      Rendering::IDevice* device,
                      std::vector<Engine::Renderer::DrawBindings::BufferEntry>& buffers,
                      std::vector<Engine::Renderer::DrawBindings::TextureEntry>& textures)
{
    if (!device)
        return;

    auto& ibl = rs.EnsureFeature<Engine::Renderer::ImageBasedLightingFeature>();
    if (!ibl.IsInitialized())
        ibl.Initialize(device);
    if (!ibl.IsInitialized())
        return;

    if (auto* sky = rs.GetFeature<Engine::Renderer::SkyRenderFeature>();
        sky && sky->HasActiveSettings())
    {
        ibl.SetIblIntensity(sky->GetSettings().iblIntensity);
    }
    if (auto* iblSource = ibl.GetEnvironmentSource(); !iblSource || iblSource->InputDigest() == 0)
        ibl.SetIblIntensity(0.0f);
    const auto envBuffer = ibl.UploadEnvData(device);

    const auto cubeSampler = ibl.GetCubeSampler();
    const auto lutSampler = ibl.GetLutSampler();
    if (cubeSampler.IsValid())
    {
        if (const auto irradiance = ibl.GetIrradianceCube(); irradiance.IsValid())
        {
            Engine::Renderer::DrawBindings::TextureEntry tex{};
            tex.Name = HashStringId("ge_irradianceCube");
            tex.Texture = irradiance;
            tex.Sampler = cubeSampler;
            textures.push_back(tex);
        }
        if (const auto prefilter = ibl.GetPrefilterCube(); prefilter.IsValid())
        {
            Engine::Renderer::DrawBindings::TextureEntry tex{};
            tex.Name = HashStringId("ge_prefilterCube");
            tex.Texture = prefilter;
            tex.Sampler = cubeSampler;
            textures.push_back(tex);
        }
    }
    if (lutSampler.IsValid())
    {
        if (const auto brdfLut = ibl.GetBrdfLut(); brdfLut.IsValid())
        {
            Engine::Renderer::DrawBindings::TextureEntry tex{};
            tex.Name = HashStringId("ge_brdfLUT");
            tex.Texture = brdfLut;
            tex.Sampler = lutSampler;
            textures.push_back(tex);
        }
    }

    if (envBuffer.IsValid())
        buffers.push_back({HashStringId("Env"), envBuffer, 0u, 0u});
}
} // namespace

class TerrainGrassForwardContributor final
{
public:
    TerrainGrassForwardContributor(TerrainGrassRenderFeature& feature,
                                   TerrainECS::TerrainRenderFeature& terrainFeature,
                                   Engine::Renderer::RenderServices& renderServices)
        : m_Feature(feature)
        , m_TerrainFeature(terrainFeature)
        , m_RenderServices(renderServices)
    {
    }

    bool EnsureMaterial(GrassDrawMode mode)
    {
        auto*& material = m_Materials[static_cast<uint32>(mode)];
        if (material)
            return true;

        struct ModeDesc
        {
            const GUID* Guid;
            const char* Name;
            const char* Keyword; // nullptr = none
            MaterialAlphaMode AlphaMode;
        };
        // Indexed by GrassDrawMode — order must match the enum.
        static const ModeDesc kModes[kGrassDrawModeCount] = {
            {&kTerrainGrassMaterialGuid, "Terrain/Grass", nullptr, MaterialAlphaMode::Blend},
            {&kTerrainGrassOpaqueMaterialGuid, "Terrain/GrassOpaque", "GRASS_OPAQUE",
             MaterialAlphaMode::Opaque},
            {&kTerrainGrassDitherMaterialGuid, "Terrain/GrassDither", "GRASS_DITHER",
             MaterialAlphaMode::Opaque},
            {&kTerrainGrassA2CMaterialGuid, "Terrain/GrassA2C", "GRASS_A2C",
             MaterialAlphaMode::Opaque},
        };
        const ModeDesc& md = kModes[static_cast<uint32>(mode)];

        auto& registry = m_RenderServices.Materials().Registry();
        material = registry.Find(*md.Guid);
        if (material)
            return true;

        MaterialDocument doc{};
        doc.materialName = md.Name;
        doc.lightingModel = "StandardPBR";
        doc.surfaceShader = "TerrainGrass/terrain_grass_surface.glsl";
        doc.vertexModifier = "TerrainGrass/terrain_grass_vertex_modifier.glsl";
        doc.alphaMode = md.AlphaMode;
        doc.doubleSided = true;
        doc.properties["roughness"] = 0.9f;
        if (md.Keyword)
            doc.keywords = {md.Keyword};

        using Rendering::MaterialKeyword;
        material = m_RenderServices.Materials().RegisterMaterialFromDocument(
            *md.Guid, doc,
            MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows |
                MaterialKeyword::Instanced | MaterialKeyword::IBL |
                MaterialKeyword::ProceduralVertexOutput);

        if (!material)
        {
            Logger::Log::Error("TerrainGrass: failed to register material '{}'", md.Name);
            return false;
        }

        Logger::Log::Info("TerrainGrass: material '{}' registered and compiled", md.Name);
        return true;
    }

    // The pinned pipeline of a mode's camera-prepass head. The blade's vertex modifier fetches the instance it
    // places from set 2, so the head is vertex-modified: the material's published depth variant (its fragment
    // stage as `head` asks) re-interned with the colour pipeline's own vertex input, as the colour pipeline is
    // re-interned from the material's, and alpha-to-coverage on where the colour pipeline has it. It draws
    // with the colour draw's layout and binds the sets its pipeline declares. Invalid until the device has
    // built that pipeline for the view's prepass (the variant compile and the build are requested on the
    // first miss; a build that failed is not retried).
    struct PrepassHeadPipeline
    {
        Rendering::GraphicsPipelineId Pipeline{};
        const Rendering::ShaderMeta* Meta = nullptr;
        // The sets the pipeline declares (set 2 included: the vertex modifier reads the blade instances there).
        uint32 SetCount = 0;
        Rendering::MaterialKeyword Keywords = Rendering::MaterialKeyword::None;
        Rendering::VertexAttributeFlags VertexFlags = Rendering::VertexAttributeFlags::None;
    };

    PrepassHeadPipeline ResolvePrepassHeadPipeline(GrassDrawMode mode, Engine::Renderer::Material& material,
                                                   GrassPrepassHead head, Rendering::GraphicsPipelineId colourPipeline,
                                                   Rendering::IDevice& device, Rendering::ViewId viewId)
    {
        using namespace Engine::Renderer;
        // Discard and AlphaToCoverage compose the depth variant's fragment stage: it evaluates the surface,
        // which discards (GRASS_DITHER) or writes the alpha the hardware turns into coverage (GRASS_A2C).
        const Rendering::MaterialKeyword keywords =
            ChooseDepthHeadPipeline(DepthPassKeywords(m_RenderServices.GetWorldPassKeywords(viewId)),
                                    DepthPassType::Prepass, /*alphaTest=*/head != GrassPrepassHead::DepthOnly,
                                    /*vertexModified=*/true, /*sharedDepthOffered=*/false,
                                    /*writesReliefDepth=*/false)
                .Keywords;
        const Rendering::VertexAttributeFlags vertexFlags = kBladeColourVertexFlags;
        PipelineVariantCache& variants = m_RenderServices.Materials().Variants();
        const PipelineVariantCache::VariantServeUnit* variant = variants.FindOrRequestPrepassVariant(
            material, vertexFlags, Rendering::PrimitiveTopology::TriangleList, keywords,
            Rendering::FrontFace::CounterClockwise);
        if (!variant)
            return {};
        PinnedPrepassHead& pinned = m_PrepassHeadPipelines[static_cast<uint32>(mode)];
        if (pinned.Source != variant->PipelineId || pinned.ColourPipeline != colourPipeline || !pinned.Pipeline.IsValid())
        {
            const auto* base = device.LookupGraphicsPipeline(variant->PipelineId);
            const auto* colour = device.LookupGraphicsPipeline(colourPipeline);
            if (!base || !colour)
                return {};
            Rendering::GraphicsPipelineDesc gd = *base;
            gd.VertexBindings = colour->VertexBindings;
            gd.VertexAttributes = colour->VertexAttributes;
            gd.ColorBlend.alphaToCoverageEnable = head == GrassPrepassHead::AlphaToCoverage;
            pinned.Pipeline = device.InternGraphicsPipeline(std::move(gd));
            pinned.Source = variant->PipelineId;
            pinned.ColourPipeline = colourPipeline;
        }
        if (!variants.EnsureConcreteWarm(material, pinned.Pipeline,
                                         m_RenderServices.Views().GetViewPrepassFormatKey(viewId)))
            return {};
        return {pinned.Pipeline, variant->VariantMeta.get(), static_cast<uint32>(variant->SetLayouts.size()),
                keywords, vertexFlags};
    }

    bool DrawsPrepassHeads(Rendering::ViewId viewId) const
    {
        const auto it = m_Scratch.find(viewId);
        return it != m_Scratch.end() && it->second.DrawsPrepassHeads;
    }

    void EmitForwardCommands(Engine::Renderer::ForwardEmitContext& ctx)
    {
        // Cleared before any early return, so a frame that emits nothing never leaves the node declaring
        // the prepass's reads of last frame's draws.
        m_Scratch[ctx.ViewId].DrawsPrepassHeads = false;

        // GE_TERRAIN_GRASS_DEBUG: trace the DRAW side, which fails INDEPENDENTLY of the placement compute.
        // The node imports the indirect args / instances / count buffers, and an imported resource
        // is marked external and seeded "needed" by the cull (RGGraph::MarkImported, and the
        // reachability seed in Compile), so the placement pass is never culled for want of a draw
        // consumer: a missing draw still leaves a TerrainGrass.Place line. Silence on THIS trace
        // with the placement still tracing means blades were placed and not drawn. Each site logs once.
        static const bool kDbg = std::getenv("GE_TERRAIN_GRASS_DEBUG") != nullptr;
        auto once = [](bool& flag) { if (flag) return false; flag = true; return true; };

        if (!m_Feature.IsInitialized())
            return;

        auto* device = ctx.Device;
        if (!device)
            return;

        // Renderer-agnostic params slot (extraction-owned; see TerrainGrassRenderNode).
        const uint32 paramsSlot = m_TerrainFeature.GetLastTerrainParamsSlot();
        const uint32 terrainParamsCount = m_TerrainFeature.GetTerrainParamsCount(paramsSlot);
        if (terrainParamsCount == 0 || m_TerrainFeature.GetTerrainGrassActiveCount(paramsSlot) == 0)
        {
            static bool logged = false;
            if (kDbg && once(logged))
                Logger::Log::Info("TerrainGrass.Forward stop=counts slot={} params={} active={}",
                                  paramsSlot, terrainParamsCount,
                                  m_TerrainFeature.GetTerrainGrassActiveCount(paramsSlot));
            return;
        }

        // The drawn mode for this view. The sample count is last frame's world-pass
        // snapshot (0 before the first world pass), so an MSAA toggle re-picks the
        // A2C-vs-discard implementation one frame late — imperceptible, self-heals.
        const auto summary = m_TerrainFeature.GetTerrainGrassPlacementSummary(paramsSlot);
        const uint32 colorSamples =
            m_RenderServices.Views().GetViewWorldColorSampleCount(ctx.ViewId);
        const GrassDrawMode mode = ResolveGrassDrawMode(summary.AllBlendMode,
                                                        summary.AnyAlphaNeeded, colorSamples);
        if (!EnsureMaterial(mode))
        {
            static bool logged = false;
            if (kDbg && once(logged)) Logger::Log::Info("TerrainGrass.Forward stop=material");
            return;
        }
        Engine::Renderer::Material* material = m_Materials[static_cast<uint32>(mode)];

        const auto paramsSSBO = m_TerrainFeature.GetTerrainParamsSSBO(paramsSlot);
        // The material table, from the SAME published slot: a params entry names its materials by
        // absolute table index, so a slot mismatch would resolve a real but wrong material.
        const auto materialsSSBO = m_TerrainFeature.GetTerrainMaterialTableSSBO(paramsSlot);
        const uint32 terrainMaterialCount = m_TerrainFeature.GetTerrainMaterialCount(paramsSlot);
        if (!paramsSSBO.IsValid() || !materialsSSBO.IsValid() ||
            terrainMaterialCount == 0 ||
            !m_Feature.GetBladeVB().IsValid() || !m_Feature.GetBladeIB().IsValid())
            return;

        auto& pipelineId = m_PipelineIds[static_cast<uint32>(mode)];
        auto& pipelineVersion = m_PipelineMaterialVersions[static_cast<uint32>(mode)];
        if (!pipelineId.IsValid() || pipelineVersion != material->GetVersion())
        {
            const auto* base = device->LookupGraphicsPipeline(material->GetGraphicsPipelineId());
            if (!base)
                return;

            Rendering::GraphicsPipelineDesc gd = *base;
            gd.VertexBindings = {{0, sizeof(BladeVertex), 0}};
            gd.VertexAttributes = {{0, 0, Rendering::Format::R32G32_FLOAT, 0}};
            // Alpha-to-coverage is pipeline state, not a material-document input, so
            // it rides the re-interned copy the same way the vertex layout does.
            if (mode == GrassDrawMode::DitherA2C)
                gd.ColorBlend.alphaToCoverageEnable = true;
            pipelineId = device->InternGraphicsPipeline(std::move(gd));
            pipelineVersion = material->GetVersion();
        }

        auto& scratch = m_Scratch[ctx.ViewId];
        scratch.Shared.clear();
        scratch.Textures.clear();
        // Always bindable once extraction has run: an empty or failed snapshot is one zero record.
        const auto& windVolumes = m_TerrainFeature.GrassWindVolumes();
        scratch.Shared.push_back({HashStringId("GrassWindVolumes"), windVolumes.Buffer(paramsSlot),
            0u, static_cast<uint64>(std::max(1u, windVolumes.Count(paramsSlot)))
                * sizeof(Rendering::WindVolumeGPU)});
        scratch.Shared.push_back({HashStringId("TerrainParamsBuffer"), paramsSSBO,
            0u, static_cast<uint64>(terrainParamsCount) * sizeof(Terrain::TerrainGPUParams)});
        // The terrain material table the blade's base colour resolves through — the same buffer the
        // terrain surface reads, which is what keeps a blade and its ground one decision.
        //
        // Every set-2 buffer is keyed by the name REFLECTION reports, which is the block's instance
        // name where it has one and the block name where it does not. The three the shaders declare
        // nameless (this one, TerrainParamsBuffer and the per-draw instances buffer) are therefore
        // given their BLOCK names; the two atlas blocks below carry instance names and are given
        // those. Getting it the wrong way round leaves the binding unresolvable, and MaterialBinder
        // then drops the draw with a throttled Error log — the symptom is invisible grass, not a
        // device loss.
        scratch.Shared.push_back({HashStringId("TerrainMaterialTableBuffer"), materialsSSBO,
            0u, static_cast<uint64>(terrainMaterialCount) * sizeof(Terrain::TerrainMaterialRecord)});
        // The eight maps the grass surface and vertex modifier declare by name under the compat
        // profile (set 2, b20-27). That profile has no binding array, so nothing else fills them —
        // a declared-but-unbound texture draws black with no error at all, which is exactly how
        // the terrain surface failed before its own bindings landed.
        //
        // Terrain 0, matching the single-terrain limit both CBT and the grass atlas already carry.
        if (m_RenderServices.GetProfile().IsCompat())
        {
            auto* terrainFeature = &m_TerrainFeature;
            const auto terrains = terrainFeature->GetActiveTerrains();
            if (!terrains.empty())
            {
                const auto primary = terrains.front().Handle;
                const auto compat = terrainFeature->GetCompatLayerTextures();
                const auto sampler = terrainFeature->GetLayerSampler();
                const auto bindTex = [&](const char* name, Rendering::TextureHandle tex) {
                    Engine::Renderer::DrawBindings::TextureEntry e{};
                    e.Name = HashStringId(name);
                    e.Texture = tex;
                    e.Sampler = sampler;
                    scratch.Textures.push_back(e);
                };
                bindTex("gGrassLayer0Albedo", compat.Albedo[0]);
                bindTex("gGrassLayer1Albedo", compat.Albedo[1]);
                bindTex("gGrassSplatmap", terrainFeature->GetSplatmapTexture(primary));
                bindTex("gGrassBladeAlbedo", compat.GrassAlbedo);
                bindTex("gGrassBladeAlpha", compat.GrassAlpha);
                bindTex("gGrassBladeNormal", compat.GrassNormal);
                bindTex("gGrassVtxHeightmap", terrainFeature->GetHeightmapTexture(primary));
                bindTex("gGrassVtxNormalmap", terrainFeature->GetNormalmapTexture(primary));
                {
                    // Shared sampler at its own binding (28), read by both stages.
                    Engine::Renderer::DrawBindings::TextureEntry e{};
                    e.Name = HashStringId("gGrassMapSampler");
                    e.Sampler = sampler;
                    scratch.Textures.push_back(e);
                }
            }
        }

        // The resident-window atlas params + indirection rows, so the blade's ground colour
        // resolves the splat through grass_atlas_splat.glsl exactly as its placement mask did.
        // Bound UNCONDITIONALLY: the surface declares both blocks always, and an unbound set-2
        // buffer makes the binder drop the draw. A non-atlas terrain reads Enabled == 0 and never
        // touches the rows.
        const auto atlas = m_Feature.GetAtlasBindingForDraw(ctx.FrameIndex);
        if (!atlas.ParamsUBO.IsValid() || !atlas.Rows.IsValid())
        {
            static bool logged = false;
            if (kDbg && once(logged))
                Logger::Log::Info("TerrainGrass.Forward stop=atlasbindings params={} rows={}",
                                  atlas.ParamsUBO.IsValid(), atlas.Rows.IsValid());
            return;
        }
        scratch.Shared.push_back({HashStringId("Atlas"), atlas.ParamsUBO,
            0u, sizeof(TerrainGrassRenderFeature::GrassAtlasParamsGPU)});
        scratch.Shared.push_back({HashStringId("gAtlasRows"), atlas.Rows,
            0u, atlas.RowsRangeBytes});
        // The budget fit's range scale, so the vertex stage's far-LOD band ends where placement
        // actually stopped spawning. Keyed by the block's INSTANCE name, like Atlas/gAtlasRows
        // above and unlike the three nameless blocks.
        const auto drawParams = m_Feature.GetDrawParamsUBO(ctx.ViewId, ctx.FrameIndex);
        if (!drawParams.IsValid())
        {
            static bool logged = false;
            if (kDbg && once(logged))
                Logger::Log::Info("TerrainGrass.Forward stop=drawparams view={} frame={}",
                                  static_cast<uint32>(ctx.ViewId), ctx.FrameIndex);
            return;
        }
        scratch.Shared.push_back({HashStringId("GrassDraw"), drawParams,
            0u, sizeof(TerrainGrassRenderFeature::GrassDrawParamsGPU)});

        const auto instances = m_Feature.GetInstanceBuffer(ctx.ViewId, ctx.FrameIndex);
        const uint32 instanceCapacity = m_Feature.GetInstanceCapacity(ctx.ViewId, ctx.FrameIndex);
        if (!instances.IsValid() || instanceCapacity == 0)
        {
            static bool logged = false;
            if (kDbg && once(logged))
                Logger::Log::Info("TerrainGrass.Forward stop=placementbuffers view={} frame={} "
                                  "instances={} capacity={} (placement buffers not ensured for this "
                                  "view/frame -> draw not emitted -> placement pass culled)",
                                  static_cast<uint32>(ctx.ViewId), ctx.FrameIndex,
                                  instances.IsValid(), instanceCapacity);
            return;
        }
        BindIblResources(m_RenderServices, device, scratch.Shared, scratch.Textures);

        Rendering::MaterialKeyword passKeywords = Rendering::MaterialKeyword::None;
        if (auto kw = m_RenderServices.GetWorldPassKeywords(ctx.ViewId))
            passKeywords = *kw;

        const auto argsBuffer = m_Feature.GetIndirectArgsBuffer(ctx.ViewId, ctx.FrameIndex);
        const auto countBuffer = m_Feature.GetIndirectCountBuffer(ctx.ViewId, ctx.FrameIndex);
        if (!argsBuffer.IsValid() || !countBuffer.IsValid())
            return;

        // The camera-prepass head this mode's draws carry (GrassPrepassHead), with its pipeline once the
        // device has built it. Until then the colour draws write their own depth (Blend: none, ever).
        const GrassPrepassHead headKind = ResolveGrassPrepassHead(mode);
        const PrepassHeadPipeline head =
            headKind == GrassPrepassHead::None
                ? PrepassHeadPipeline{}
                : ResolvePrepassHeadPipeline(mode, *material, headKind, pipelineId, *device, ctx.ViewId);
        const bool drawsHeads = head.Pipeline.IsValid();
        // The blades receive neither GTAO nor the screen-space contact shadows (the colour draw is the
        // base pipeline), so their heads do not occlude those passes either: they join the view depth
        // after DepthResolve took the occluders' copy those passes read.
        const Engine::Renderer::ForwardDrawDepth forwardDepth =
            drawsHeads                      ? Engine::Renderer::ForwardDrawDepth::PrepassNonOccluding
            : mode == GrassDrawMode::Blend ? Engine::Renderer::ForwardDrawDepth::None
                                            : Engine::Renderer::ForwardDrawDepth::ColourPass;

        // One draw per LOD over the same whole instance pool. The record's firstInstance selects
        // the LOD's range: 0 for LOD 0, capacity minus its count for LOD 1, both written by the
        // plan step before placement runs. That needs the drawIndirectFirstInstance device
        // feature, which VulkanDevice enables; Metal folds the base instance into instance_id.
        // On the first frame a view exists the plan is still unresolved (kNoPlan): the pool
        // capacity is zero, so both draws are declined below and the plan is resolved by the
        // graph declaration for the next frame.
        const auto& plan = m_Feature.GetPlan(ctx.ViewId, ctx.FrameIndex);

        // Band order per mode. Blend writes no depth and composites in emission
        // order, so the far band draws FIRST — it can then never composite over
        // near blades (the band-inversion artifact this ordering exists to kill).
        // Every depth-writing mode (Opaque and the two dither modes) draws
        // near-first instead, so early-Z rejects the far band behind already-drawn
        // near blades.
        static constexpr uint32 kLodOrderFarFirst[kGrassLodCount] = {1u, 0u};
        static constexpr uint32 kLodOrderNearFirst[kGrassLodCount] = {0u, 1u};
        const uint32* lodOrder =
            mode == GrassDrawMode::Blend ? kLodOrderFarFirst : kLodOrderNearFirst;

        for (uint32 drawIndex = 0; drawIndex < kGrassLodCount; ++drawIndex)
        {
            const uint32 lod = lodOrder[drawIndex];
            if (plan.Capacity == 0)
                continue;

            // DrawBindings holds SPANS, not owned vectors, and the emitted command is consumed
            // when the world pass records — long after this function returns. The entries must
            // therefore live in per-view member storage; a local vector here is a use-after-free
            // the binder reads at record time (all four set=2 names vanish together and the draw
            // is skipped, intermittently, depending on heap reuse).
            auto& buffers = scratch.LodBuffers[lod];
            buffers = scratch.Shared;
            // Both LODs bind the whole pool: the indirect record's first instance selects the
            // range (0 for LOD 0, capacity minus its count for LOD 1), so the slice is the GPU's
            // to decide once placement has run.
            buffers.push_back({HashStringId("TerrainGrassInstances"), instances, 0u,
                static_cast<uint64>(plan.Capacity) * sizeof(GrassBladeInstanceGPU)});

            Engine::Renderer::DrawBindings drawBindings{};
            drawBindings.Buffers = buffers;
            drawBindings.Textures = scratch.Textures;

            const auto& lodMesh = m_Feature.GetBladeLod(lod);
            Engine::Renderer::DrawCommand cmd{};
            cmd.InternedPipeline = pipelineId;
            cmd.Material = material;
            cmd.VertexFlags = kBladeColourVertexFlags;
            cmd.PassKeywords = passKeywords;
            cmd.Geometry.AltGeom = {
                m_Feature.GetBladeVB(),
                m_Feature.GetBladeIB(),
                Rendering::IndexType::Uint32
            };
            cmd.IndexCount = lodMesh.IndexCount;
            cmd.InstanceCount = 0;
            cmd.FirstIndex = lodMesh.FirstIndex;
            cmd.FirstInstance = 0;
            cmd.VertexOffset = lodMesh.VertexOffset;
            cmd.UseIndirect = true;
            cmd.IndirectCommandBuffer = argsBuffer;
            cmd.IndirectCountBuffer = countBuffer;
            cmd.IndirectMaxDrawCount = 1;
            cmd.IndirectStride = sizeof(GrassIndirectDrawGPU);
            cmd.IndirectCommandOffset = static_cast<size_t>(lod) * sizeof(GrassIndirectDrawGPU);
            cmd.IndirectCountOffset = 0;
            cmd.Bindings = drawBindings;

            // The head draws the same LOD record over the same instance pool and bindings, so it places every
            // blade where the colour draw does.
            Engine::Renderer::DrawCommand headCmd{};
            if (drawsHeads)
            {
                headCmd = cmd;
                headCmd.VertexFlags = head.VertexFlags;
                headCmd.InternedPipeline = head.Pipeline;
                headCmd.PipelineMeta = head.Meta;
                headCmd.PipelineSetCount = head.SetCount;
                headCmd.PassKeywords = head.Keywords;
            }
            m_RenderServices.EmitForwardCommand(ctx.ViewId, cmd, forwardDepth, drawsHeads ? &headCmd : nullptr);
            scratch.DrawsPrepassHeads |= drawsHeads;
        }

        {
            static bool logged = false;
            if (kDbg && once(logged))
                Logger::Log::Info("TerrainGrass.Forward EMIT view={} frame={} capacity={}",
                                  static_cast<uint32>(ctx.ViewId), ctx.FrameIndex, plan.Capacity);
        }
    }

private:
    // Backing storage for the emitted draws' DrawBindings spans. Rebuilt every emit, read at
    // world-pass record — the same one-frame lifetime the terrain params scratch has.
    struct ViewScratch
    {
        std::vector<Engine::Renderer::DrawBindings::BufferEntry> Shared;
        std::vector<Engine::Renderer::DrawBindings::BufferEntry> LodBuffers[kGrassLodCount];
        std::vector<Engine::Renderer::DrawBindings::TextureEntry> Textures;
        // This frame's draws carry their prepass heads (DrawsPrepassHeads).
        bool DrawsPrepassHeads = false;
    };

    TerrainGrassRenderFeature& m_Feature;
    TerrainECS::TerrainRenderFeature& m_TerrainFeature;
    Engine::Renderer::RenderServices& m_RenderServices;
    // One material + one interned pipeline per GrassDrawMode, resolved lazily the
    // first frame a view draws in that mode.
    Engine::Renderer::Material* m_Materials[kGrassDrawModeCount] = {};
    Rendering::GraphicsPipelineId m_PipelineIds[kGrassDrawModeCount] = {};
    uint32 m_PipelineMaterialVersions[kGrassDrawModeCount] = {};
    // Each mode's prepass head pipeline, with the depth variant and the colour pipeline it was derived from: a
    // republished variant or a re-interned colour pipeline has a new id, which re-derives the head.
    struct PinnedPrepassHead
    {
        Rendering::GraphicsPipelineId Source{};
        Rendering::GraphicsPipelineId ColourPipeline{};
        Rendering::GraphicsPipelineId Pipeline{};
    };
    PinnedPrepassHead m_PrepassHeadPipelines[kGrassDrawModeCount] = {};
    std::unordered_map<Rendering::ViewId, ViewScratch> m_Scratch;
};

void TerrainGrassForwardContributorDeleter::operator()(TerrainGrassForwardContributor* ptr) const
{
    delete ptr;
}

bool TerrainGrassRenderFeature::DrawsPrepassHeads(Rendering::ViewId viewId) const
{
    return m_ForwardContributor && m_ForwardContributor->DrawsPrepassHeads(viewId);
}

TerrainGrassRenderFeature::~TerrainGrassRenderFeature()
{
    m_ForwardProducer.Reset();

    m_RegisteredOn = nullptr;
    DestroyGpuResources();
}

bool TerrainGrassRenderFeature::Initialize(Rendering::IDevice* device)
{
    if (m_Initialized)
        return true;
    if (!device)
        return false;

    m_Device = device;
    CreateBladeMesh(5u);

    // Zeroed at creation, not merely allocated: the grass DRAW binds whichever slot the frame lands
    // on, and a frame whose placement never ran would otherwise hand the surface uninitialized
    // upload memory as atlas params — a garbage Enabled sends it down the atlas path with a garbage
    // bindless index. Zero reads as Enabled == 0, which is exactly "no atlas, use the unified path".
    const GrassAtlasParamsGPU zeroedAtlasParams{};
    for (uint32 i = 0; i < kMaxFrames; ++i)
    {
        m_AtlasParamsUBO[i] = CreateUniformBuffer(*m_Device, sizeof(GrassAtlasParamsGPU),
                                                  "TerrainGrass_AtlasParams");
        if (m_AtlasParamsUBO[i].IsValid())
            m_Device->UpdateBuffer(m_AtlasParamsUBO[i], 0, sizeof(zeroedAtlasParams),
                                   &zeroedAtlasParams);
    }

    // A 1-row NO_SLOT indirection buffer, bound at placement binding 5 whenever no atlas terrain is
    // present so the descriptor is always valid (Atlas.Enabled == 0 keeps the shader off this path).
    {
        Rendering::BufferDesc desc{};
        desc.size = kGrassAtlasRowBytes;
        desc.usage = static_cast<uint32>(Rendering::BufferUsage::Storage);
        desc.memoryUsage = Rendering::BufferMemoryUsage::Upload;
        desc.persistent = true;
        desc.debugName = "TerrainGrass_DefaultAtlasRows";
        m_DefaultAtlasRows = m_Device->CreateBuffer(desc);
        if (m_DefaultAtlasRows.IsValid())
        {
            if (void* mapped = m_Device->MapBuffer(m_DefaultAtlasRows))
            {
                std::memset(mapped, 0xFF, kGrassAtlasRowBytes); // Slot == kAtlasNoSlot
                m_Device->UnmapBuffer(m_DefaultAtlasRows);
            }
        }
    }

    m_Initialized = m_BladeVB.IsValid() && m_BladeIB.IsValid();
    return m_Initialized;
}

void TerrainGrassRenderFeature::EnsureForwardContributor(
    Engine::Renderer::RenderServices& rs,
    TerrainECS::TerrainRenderFeature& terrainFeature)
{
    if (m_ForwardContributor)
        return;

    m_ForwardContributor.reset(new TerrainGrassForwardContributor(*this, terrainFeature, rs));
    m_RegisteredOn = &rs;

    auto* contributor = m_ForwardContributor.get();
    m_ForwardProducer = rs.RegisterForwardEmit(
        [contributor](Engine::Renderer::ForwardEmitContext& ctx)
        {
            contributor->EmitForwardCommands(ctx);
        });
}

Rendering::BufferHandle TerrainGrassRenderFeature::GetInstanceBuffer(Rendering::ViewId viewId,
                                                                     uint32 frameIndex) const
{
    auto it = m_PlacementScratch.find(viewId);
    if (it == m_PlacementScratch.end())
        return {};
    return it->second.Frames[frameIndex].Instances;
}

Rendering::BufferHandle TerrainGrassRenderFeature::GetDrawParamsUBO(Rendering::ViewId viewId,
                                                                   uint32 frameIndex) const
{
    auto it = m_PlacementScratch.find(viewId);
    if (it == m_PlacementScratch.end())
        return {};
    return it->second.Frames[frameIndex].DrawParamsUBO;
}

Rendering::BufferHandle TerrainGrassRenderFeature::GetIndirectArgsBuffer(Rendering::ViewId viewId,
                                                                         uint32 frameIndex) const
{
    auto it = m_PlacementScratch.find(viewId);
    if (it == m_PlacementScratch.end())
        return {};
    return it->second.Frames[frameIndex].IndirectArgs;
}

Rendering::BufferHandle TerrainGrassRenderFeature::GetIndirectCountBuffer(Rendering::ViewId viewId,
                                                                          uint32 frameIndex) const
{
    auto it = m_PlacementScratch.find(viewId);
    if (it == m_PlacementScratch.end())
        return {};
    return it->second.Frames[frameIndex].IndirectCount;
}

uint32 TerrainGrassRenderFeature::GetInstanceCapacity(Rendering::ViewId viewId, uint32 frameIndex) const
{
    auto it = m_PlacementScratch.find(viewId);
    if (it == m_PlacementScratch.end())
        return 0;
    return it->second.Frames[frameIndex].Capacity;
}

namespace
{
struct GrassShaderLocation
{
    std::filesystem::path Dir;
    std::filesystem::path IncludeRoot;
    std::filesystem::path CbtDir; // holds cbt_atlas.glsl (the resident-window atlas resolve grass shares)
};

// The staged grass compute sources under the install assets root's Shaders directory; nullopt
// when the placement kernel is not staged there.
// `cookedPrograms` is a device that ingests ahead-of-time cooked WGSL (no shader compiler at
// runtime): the programs are staged beside the executable (Tools/Web/builtin_shader_cook.py), the
// same move CBTKernelSet makes for the terrain kernels, and only the program directory is needed.
std::optional<GrassShaderLocation> FindGrassShaderLocation(bool cookedPrograms)
{
    namespace fs = std::filesystem;
    const fs::path shaders = PathUtils::GetInstallAssetsRoot() / "Shaders";
    std::error_code ec;
    if (cookedPrograms)
    {
        const fs::path programDir = shaders / "TerrainGrass";
        if (!fs::exists(programDir / "terrain_grass_place.comp.wgsl", ec))
            return std::nullopt;
        return GrassShaderLocation{programDir, {}, {}};
    }
    GrassShaderLocation location{shaders / "TerrainGrass", shaders, shaders / "CBT"};
    if (!fs::exists(location.Dir / "terrain_grass_place.comp", ec))
        return std::nullopt;
    return location;
}

bool CompileGrassComputePipeline(Rendering::IDevice& device, const GrassShaderLocation& location,
                                 const char* shaderFile, const char* debugName, bool compat,
                                 Rendering::ComputePipelineId& outId,
                                 Rendering::PipelineHandle& outPipeline)
{
    namespace fs = std::filesystem;
    Vector<uint8> computeProgram;
    if (device.PreferredShaderSource() == Rendering::ShaderSourceKind::Wgsl)
    {
        // No shader compiler at runtime: load this stage's ahead-of-time cooked WGSL (staged
        // beside the executable, named "<stage>.wgsl") instead of compiling from source.
        const fs::path cookedProgram = location.Dir / (std::string(shaderFile) + ".wgsl");
        if (!ReadFileBytesShared(cookedProgram, computeProgram) || computeProgram.empty())
        {
            Logger::Log::Error("TerrainGrass: cooked program not found ({}.wgsl)", shaderFile);
            return false;
        }
    }
    else
    {
        Rendering::ShaderProgramCompileRequest req{};
        req.debugName = debugName;
        req.baseDirectory = location.Dir;
        req.cacheRoot = fs::path(".Cache") / "Shaders";
        req.includeDirs = {location.IncludeRoot, location.CbtDir, location.Dir};
        // The compat profile selects the named-binding arm of the source, the same arm the
        // web cook builds, so a desktop run under GE_FORCE_COMPAT exercises it too.
        std::vector<std::string> defines;
        if (compat)
            defines.push_back("GE_COMPAT_PROFILE");
        req.stages = {{"cs", shaderFile, "main", std::move(defines)}};
        Rendering::ShaderProgramCompileResult result{};
        std::string compileError;
        if (!Rendering::ShaderCompileService::CompileProgramToCache(
                req, device.PreferredShaderSource(), result, &compileError))
        {
            Logger::Log::Error("TerrainGrass: {} compile failed: {}", shaderFile, compileError);
            return false;
        }
        auto itCs = result.stageBytes.find("cs");
        if (itCs == result.stageBytes.end())
        {
            Logger::Log::Error("TerrainGrass: {} produced no compute SPIR-V", shaderFile);
            return false;
        }
        computeProgram = std::move(itCs->second);
    }

    Rendering::ComputePipelineDesc cd{};
    cd.ComputeShader = std::make_shared<const std::vector<uint8>>(std::move(computeProgram));
    // Both grass compute pipelines share one descriptor layout, so the plan and the placement
    // bind one set: the plan writes the cell ranges into it and the placement reads them back.
    cd.DescriptorSetLayouts.push_back(
        device.InternDescriptorSetLayout(MakeGrassPlaceDescriptorSetLayout(compat)));
    // Compat has no second set: it has no binding arrays, so the maps this shader reads are
    // named bindings inside set 0 above (see grassMapTap).
    if (!compat)
    {
        const uint32 bindlessTextureCount =
            std::max<uint32>(1u, device.GetCapabilities().maxBindlessTextures);
        cd.DescriptorSetLayouts.push_back(
            device.InternDescriptorSetLayout(
                Engine::Renderer::TextureService::GetBindlessTextureSetLayout(bindlessTextureCount)));
    }
    cd.DebugName = debugName;
    outId = device.InternComputePipeline(cd);
    outPipeline = device.GetOrCreateComputePipeline(outId);
    if (!outPipeline.IsValid())
    {
        Logger::Log::Error("TerrainGrass: {} pipeline creation failed", debugName);
        return false;
    }
    return true;
}
} // namespace

bool TerrainGrassRenderFeature::EnsureComputePipeline(Engine::Renderer::RenderServices& rs)
{
    if (m_ComputePipeline.IsValid() && m_PlanPipeline.IsValid() && m_ClassifyPipeline.IsValid())
        return true;
    if (m_ComputePipelineAttempted)
        return false;
    m_ComputePipelineAttempted = true;
    if (!m_Device)
        return false;
    const bool compat = rs.GetProfile().IsCompat();
    const std::optional<GrassShaderLocation> location = FindGrassShaderLocation(
        m_Device->PreferredShaderSource() == Rendering::ShaderSourceKind::Wgsl);
    if (!location)
    {
        Logger::Log::Warning("TerrainGrass: placement compute shader not found");
        return false;
    }
    if (!CompileGrassComputePipeline(*m_Device, *location, "terrain_grass_classify.comp",
                                     "TerrainGrassClassify_Pipeline", compat, m_ClassifyPipelineId,
                                     m_ClassifyPipeline)
        || !CompileGrassComputePipeline(*m_Device, *location, "terrain_grass_plan.comp",
                                        "TerrainGrassPlan_Pipeline", compat, m_PlanPipelineId,
                                        m_PlanPipeline)
        || !CompileGrassComputePipeline(*m_Device, *location, "terrain_grass_place.comp",
                                        "TerrainGrassPlace_Pipeline", compat, m_ComputePipelineId,
                                        m_ComputePipeline))
        return false;
    Logger::Log::Info("TerrainGrass: placement compute pipeline ready");
    return true;
}

const GrassPlacementPlan& TerrainGrassRenderFeature::GetPlan(Rendering::ViewId viewId,
                                                            uint32 frameIndex) const
{
    static const GrassPlacementPlan kNoPlan{};
    auto it = m_PlacementScratch.find(viewId);
    if (it == m_PlacementScratch.end())
        return kNoPlan;
    return it->second.Frames[frameIndex].Plan;
}

GrassPlacementPlan TerrainGrassRenderFeature::ResolvePlan(
    Engine::Renderer::RenderServices& rs,
    TerrainECS::TerrainRenderFeature& terrainFeature,
    Rendering::ViewId viewId, uint32 paramsSlot) const
{
    const auto summary = terrainFeature.GetTerrainGrassPlacementSummary(paramsSlot);

    GrassPlacementParams params;
    params.NearDensity = summary.MaxNearDensity;
    params.FarRadius = summary.MaxRange;
    // The flattest authored curve spends the most of the budget far from the camera, so planning
    // against it keeps the fit an upper bound when terrains disagree.
    params.Falloff = summary.MinFalloff;
    params.Seed = 0.0f; // per-terrain and read on the GPU; the fit does not depend on it

    GrassPlacementPlan plan = GrassFitPlacementToBudget(params, kMaxInstancesPerView);
    // Say it at INFO when the budget costs an author range, and again whenever that RESULT changes.
    // Without this the only signal that 500 became 369 is behind a debug env var, and the number the
    // component shows is not the number the field is drawn at.
    //
    // Keyed on the fit's inputs AND its output rather than a once-per-process flag: an author who
    // edits the density or the range gets a new answer, and a latch would have told them about the
    // first one only. Not per terrain -- the fit runs on the summary across every active grass
    // terrain, so the distinct fit RESULT is the honest granularity.
    if (plan.RangeReduced)
    {
        const RangeFitLogKey key{params.NearDensity, params.FarRadius, params.Falloff,
                                 plan.Params.FarRadius};
        if (!(m_LastRangeFitLogged == key))
        {
            m_LastRangeFitLogged = key;
            Logger::Log::Info(
                "TerrainGrass: {} per m2 over {} m exceeds the per-view instance budget ({}); "
                "placing to {} m instead. Near density is unchanged — the fit spends range.",
                params.NearDensity, params.FarRadius, kMaxInstancesPerView,
                plan.Params.FarRadius);
        }
    }
    (void)rs;
    (void)viewId;
    return plan;
}

TerrainGrassRenderFeature::PlacementBuffers* TerrainGrassRenderFeature::EnsurePlacementBuffers(
    Rendering::ViewId viewId, uint32 frameIndex)
{
    if (!m_Device)
        return nullptr;

    auto& buffers = m_PlacementScratch[viewId].Frames[frameIndex];
    if (buffers.Instances.IsValid() && buffers.IndirectArgs.IsValid() &&
        buffers.IndirectCount.IsValid() && buffers.PlaceParamsUBO.IsValid() &&
        buffers.DrawParamsUBO.IsValid())
    {
        return &buffers;
    }

    // Defer-destroy: the outgoing buffers may still be read by the GPU on a frame in flight.
    DeferBufferDestroy(buffers.Instances);
    DeferBufferDestroy(buffers.IndirectArgs);
    DeferBufferDestroy(buffers.IndirectCount);
    DeferBufferDestroy(buffers.PlaceParamsUBO);
    DeferBufferDestroy(buffers.DrawParamsUBO);
    DeferBufferDestroy(buffers.CellPlan);

    // Clears the slot's elision gate along with its handles, which is exactly right: whatever this
    // slot retained went with the buffers, so the gate must not compare the next frame's inputs
    // against a record describing content that no longer exists.
    buffers = {};
    buffers.Capacity = kMaxInstancesPerView;

    Rendering::BufferDesc instancesDesc{};
    instancesDesc.size = static_cast<size_t>(buffers.Capacity) * sizeof(GrassBladeInstanceGPU);
    // TransferSrc for the GE_TERRAIN_GRASS_DEBUG roots readback: WebGPU validates
    // buffer usage strictly, and a CopySrc-less source invalidates the WHOLE
    // command buffer — the frame's placement dispatch is then dropped with it,
    // and the instrument reads back the zero it caused itself.
    instancesDesc.usage = static_cast<uint32>(Rendering::BufferUsage::Storage)
        | static_cast<uint32>(Rendering::BufferUsage::TransferSrc);
    instancesDesc.memoryUsage = Rendering::BufferMemoryUsage::DeviceLocal;
    instancesDesc.debugName = "TerrainGrass_Instances";
    buffers.Instances = m_Device->CreateBuffer(instancesDesc);

    Rendering::BufferDesc argsDesc{};
    argsDesc.size = sizeof(GrassIndirectBlockGPU);
    argsDesc.usage = static_cast<uint32>(Rendering::BufferUsage::Storage)
        | static_cast<uint32>(Rendering::BufferUsage::Indirect)
        | static_cast<uint32>(Rendering::BufferUsage::TransferDst)
        | static_cast<uint32>(Rendering::BufferUsage::TransferSrc);
    argsDesc.memoryUsage = Rendering::BufferMemoryUsage::DeviceLocal;
    argsDesc.debugName = "TerrainGrass_IndirectArgs";
    buffers.IndirectArgs = m_Device->CreateBuffer(argsDesc);

    Rendering::BufferDesc countDesc{};
    countDesc.size = sizeof(uint32);
    countDesc.usage = static_cast<uint32>(Rendering::BufferUsage::Indirect)
        | static_cast<uint32>(Rendering::BufferUsage::TransferDst);
    countDesc.memoryUsage = Rendering::BufferMemoryUsage::DeviceLocal;
    countDesc.debugName = "TerrainGrass_IndirectCount";
    buffers.IndirectCount = m_Device->CreateBuffer(countDesc);

    Rendering::BufferDesc cellPlanDesc{};
    // Sized to the LARGEST window rather than to this frame's plan: the fit changes the cell span
    // whenever the author edits density or range, and a buffer that resized with it would retire
    // and reallocate ~1 MB per view on an edit. Device-local — nothing on the CPU reads it.
    cellPlanDesc.size = kGrassCellPlanBytes;
    // TransferDst: the ring histogram at the head of the buffer is cleared by the CPU each frame,
    // because the classify kernel accumulates into it and has no phase in which to zero it first.
    cellPlanDesc.usage = static_cast<uint32>(Rendering::BufferUsage::Storage)
        | static_cast<uint32>(Rendering::BufferUsage::TransferDst);
    cellPlanDesc.memoryUsage = Rendering::BufferMemoryUsage::DeviceLocal;
    cellPlanDesc.debugName = "TerrainGrass_CellPlan";
    buffers.CellPlan = m_Device->CreateBuffer(cellPlanDesc);

    buffers.PlaceParamsUBO = CreateUniformBuffer(*m_Device, sizeof(GrassPlaceParamsGPU),
                                                 "TerrainGrass_PlaceParams");
    buffers.DrawParamsUBO = CreateUniformBuffer(*m_Device, sizeof(GrassDrawParamsGPU),
                                                "TerrainGrass_DrawParams");
    if (buffers.DrawParamsUBO.IsValid())
    {
        // SEEDED AT CREATION, not left to the first dispatch. These are host-visible UPLOAD
        // allocations with no zero-fill, and the draw only tests that the handle is valid, not that
        // anything has written it. A frame whose placement dispatch bails before the fill still
        // draws (TerrainGrassPlacementStats::Dropout::StaleArgs is exactly that case), so on the
        // first use of a frame slot the vertex stage would scale the range by uninitialised memory
        // — and at 0 the whole field collapses to a 1 m radius. 1.0 is the authored-range identity.
        const GrassDrawParamsGPU seed{};
        m_Device->UpdateBuffer(buffers.DrawParamsUBO, 0, sizeof(seed), &seed);
    }

    if (!buffers.Instances.IsValid() || !buffers.IndirectArgs.IsValid() ||
        !buffers.IndirectCount.IsValid() || !buffers.PlaceParamsUBO.IsValid() ||
        !buffers.DrawParamsUBO.IsValid() || !buffers.CellPlan.IsValid())
    {
        for (auto handle : {buffers.Instances, buffers.IndirectArgs, buffers.IndirectCount,
                            buffers.PlaceParamsUBO, buffers.DrawParamsUBO, buffers.CellPlan})
        {
            if (handle.IsValid())
                m_Device->DestroyBuffer(handle);
        }
        buffers = {};
        Logger::Log::Error("TerrainGrass: failed to allocate placement buffers");
        return nullptr;
    }

    return &buffers;
}

void TerrainGrassRenderFeature::EnsureIndirectConstants(Rendering::CommandList& cmd,
                                                        PlacementBuffers& buffers) const
{
    const PlacementBuffers::IndirectConstants constants{true, m_BladeLods, buffers.Plan.Capacity};
    if (buffers.SeededConstants == constants)
        return;

    for (uint32 lod = 0; lod < kGrassLodCount; ++lod)
    {
        const auto& lodMesh = constants.Lods[lod];
        const uint64 base = static_cast<uint64>(lod) * sizeof(GrassIndirectDrawGPU);
        cmd.FillBuffer(buffers.IndirectArgs, base + offsetof(GrassIndirectDrawGPU, IndexCount),
                       sizeof(uint32), lodMesh.IndexCount);
        cmd.FillBuffer(buffers.IndirectArgs, base + offsetof(GrassIndirectDrawGPU, FirstIndex),
                       sizeof(uint32), lodMesh.FirstIndex);
        cmd.FillBuffer(buffers.IndirectArgs, base + offsetof(GrassIndirectDrawGPU, VertexOffset),
                       sizeof(uint32), static_cast<uint32>(lodMesh.VertexOffset));
        // The plan dispatch writes both first instances every frame; seeding them here is what
        // leaves the records DEFINED on a frame whose placement bails before dispatching at all.
        cmd.FillBuffer(buffers.IndirectArgs, base + offsetof(GrassIndirectDrawGPU, FirstInstance),
                       sizeof(uint32), 0u);
    }
    // The pool size rides in the block itself so both kernels and the readback measure against one
    // number.
    cmd.FillBuffer(buffers.IndirectArgs, offsetof(GrassIndirectBlockGPU, Capacity), sizeof(uint32),
                   constants.Capacity);
    // The readback copies the whole block, padding included, so nothing in it is left as whatever
    // the device-local allocation happened to contain.
    cmd.FillBuffer(buffers.IndirectArgs, offsetof(GrassIndirectBlockGPU, _Pad),
                   sizeof(GrassIndirectBlockGPU::_Pad), 0u);
    // Each LOD's draw reads its command count from here, and a record is exactly one command.
    cmd.FillBuffer(buffers.IndirectCount, 0, sizeof(uint32), 1u);

    buffers.SeededConstants = constants;
}

bool TerrainGrassRenderFeature::EnsurePlacementBuffersForViewRG(
    Engine::Renderer::RenderServices& rs, TerrainECS::TerrainRenderFeature& terrainFeature,
    Rendering::ViewId viewId, uint32 frameIndex, uint32 activeGrassCount)
{
    if (!m_Initialized || !m_Device || activeGrassCount == 0)
        return false;
    // Once per frame across views: retire buffers grown out of in-flight use BEFORE this frame
    // allocates (the retirement is monotonic-frame paced).
    FlushDeferredDestroys();
    auto* buffers = EnsurePlacementBuffers(viewId, frameIndex);
    if (!buffers)
        return false;
    // Resolve the plan HERE, not in the dispatch: the forward draw reads Capacity to size the
    // instance binding, and it is emitted before the placement pass executes. One resolve per
    // view per frame is what makes the two agree by construction.
    const uint32 paramsSlot = terrainFeature.GetLastTerrainParamsSlot();
    // Carried into the exec, which binds this slot's SSBO rather than re-reading the ring: the
    // budget bound below is computed over that array's summary, and a bound computed over one
    // array while the cells are classified against another is not a bound at all.
    buffers->ParamsSlot = paramsSlot;
    buffers->Plan = ResolvePlan(rs, terrainFeature, viewId, paramsSlot);
    // The GPU inputs go with it, for the same reason one step further on: the elision gate compares
    // them at declaration to decide whether the pass is added at all.
    if (!ResolvePlaceParams(rs, terrainFeature, viewId, paramsSlot,
                            terrainFeature.GetTerrainParamsCount(paramsSlot), buffers->Plan,
                            buffers->Place))
    {
        return false;
    }
    return buffers->Instances.IsValid() && buffers->IndirectArgs.IsValid() &&
           buffers->IndirectCount.IsValid() && buffers->PlaceParamsUBO.IsValid() &&
           buffers->DrawParamsUBO.IsValid() && buffers->CellPlan.IsValid();
}

bool TerrainGrassRenderFeature::ResolvePlaceParams(
    Engine::Renderer::RenderServices& rs, TerrainECS::TerrainRenderFeature& terrainFeature,
    Rendering::ViewId viewId, uint32 paramsSlot, uint32 terrainParamsCount,
    const GrassPlacementPlan& plan, GrassPlaceParamsGPU& out) const
{
    out = GrassPlaceParamsGPU{};
    const auto* view = rs.Views().FindViewDesc(viewId);
    const auto* camera = view ? rs.Views().FindCameraData(view->cameraId) : nullptr;
    if (!camera)
        return false;

    Rendering::Matrix4x4 viewProj;
    std::memcpy(viewProj.Data(), camera->viewProj, sizeof(float) * 16);
    Rendering::Vector4 planes[6]{};
    Rendering::ExtractFrustumPlanes(viewProj, planes);
    // A view-projection that is not a real projection (a degenerate or unset camera) extracts
    // zero-length normals, and normalizing those yields NaN. NaN plane tests are not reliably
    // false on the GPU, so the cell cull would become undefined rather than merely permissive.
    // Substituting planes that accept everything keeps the failure mode defined: grass places
    // without frustum culling instead of flickering or vanishing.
    bool planesUsable = true;
    for (const auto& plane : planes)
    {
        if (!std::isfinite(plane.x) || !std::isfinite(plane.y) ||
        !std::isfinite(plane.z) || !std::isfinite(plane.w))
        {
        planesUsable = false;
        break;
        }
    }
    for (uint32 i = 0; i < 6; ++i)
    {
        out.FrustumPlanes[i][0] = planesUsable ? planes[i].x : 0.0f;
        out.FrustumPlanes[i][1] = planesUsable ? planes[i].y : 0.0f;
        out.FrustumPlanes[i][2] = planesUsable ? planes[i].z : 0.0f;
        out.FrustumPlanes[i][3] = planesUsable ? planes[i].w : 1.0f;
    }

    out.CameraPos[0] = camera->cameraPos[0];
    out.CameraPos[1] = camera->cameraPos[1];
    out.CameraPos[2] = camera->cameraPos[2];

    // Column-major, so columns 0-2 are elements 0-2, 4-6 and 8-10; element 12-14 is the
    // translation and is deliberately left out (the compute works camera-relative). These are
    // the same matrices the vertex stage sees as Cam.uV / Cam.uP — camera_ubo_fields.glsl
    // names Rendering::CameraData as its mirror — so the screen width the compute computes is
    // the one the draw would have computed.
    for (uint32 i = 0; i < 3; ++i)
    {
        out.ViewRotCol0[i] = camera->view[i];
        out.ViewRotCol1[i] = camera->view[4 + i];
        out.ViewRotCol2[i] = camera->view[8 + i];
    }
    // proj[5] is row 1 of column 1 under the same convention.
    out.ViewRotCol2[3] = camera->proj[5];
    // The other half of the world-width-to-screen-width conversion, and per-view for the same
    // reason the matrix above is: a frame can place for several views at different sizes (a
    // Game View beside a Scene View, six reflection-probe cube faces), and a width sized for
    // the wrong viewport is wrong on screen. Snapshotted at the view's last world pass, so it
    // is one frame stale across a resize - the same staleness WorldColorSampleCount carries,
    // and harmless for the same reason: it shifts a sub-pixel width floor by the resize ratio
    // for one frame. 0 until the view's first world pass, which the compute reads as "size
    // unknown" and answers with no widening at all.
    //
    // ORTHOGRAPHIC VIEWS PUBLISH 0 DELIBERATELY. The compute's floor divides by clip.w to get
    // pixels per world unit, which is view z only under a perspective projection; an
    // orthographic one carries w = 1, so the same expression under-reads by a factor of z and
    // pins every blade past about a metre to the expansion ceiling. Camera::Perspective is
    // authorable, so this is reachable from the editor rather than hypothetical. 0 is the
    // "size unknown" value the compute already handles, which is exactly the pre-floor
    // behaviour: no widening, no ceiling, blades as authored.
    const bool orthographic = Engine::Renderer::IsOrthographicProjectionLH_ZO(camera->proj);
    out.ViewRotCol0[3] = orthographic
        ? 0.0f
        : static_cast<float>(rs.Views().GetViewWorldViewportHeight(viewId));
    const auto summary = terrainFeature.GetTerrainGrassPlacementSummary(paramsSlot);
    out.CameraPos[3] = summary.MaxRange > 0.0f
        ? plan.Params.FarRadius / summary.MaxRange
        : 0.0f;

    out.CellWindow[0] = static_cast<int32>(
        std::floor(camera->cameraPos[0] / kGrassCellSize));
    out.CellWindow[1] = static_cast<int32>(
        std::floor(camera->cameraPos[2] / kGrassCellSize));
    out.CellWindow[2] = static_cast<int32>(plan.CellSpan);
    out.CellWindow[3] = static_cast<int32>(terrainParamsCount);

    // A conservative vertical extent for the cell AABB: no blade can root outside the terrain's
    // own height range, and none can reach higher than the tallest authored blade.
    out.Bounds[0] = summary.WorldMinY;
    out.Bounds[1] = summary.WorldMaxY + summary.MaxBladeHeight;
    out.Bounds[2] = plan.Lod1StartDistance;
    // Cells are rejected on centre distance, so allow the half-diagonal before the cell's near
    // corner could still be inside the range.
    out.Bounds[3] = plan.Params.FarRadius + kGrassCellSize;
    // The budget fit the plan kernel re-runs on the GPU needs terms that dominate every
    // per-terrain value, or its per-ring total would stop being a bound on the per-cell sum.
    out.Summary[0] = summary.MaxNearDensity;
    out.Summary[1] = summary.MinFalloff;
    return true;
}

bool TerrainGrassRenderFeature::ShouldSkipPlacement(
    TerrainECS::TerrainRenderFeature& terrainFeature, Rendering::ViewId viewId, uint32 frameIndex)
{
    // Kill switches, read once (the house latch idiom the cull/scatter/cluster/SDSM families use):
    // GE_IDLE_ELISION=0 masters everything off, GE_IDLE_ELISION_GRASS=0 disables this family alone.
    // Both default ON — the gate is exact-input and fails toward recomputing.
    static const bool kAllowElision = []
    {
        const auto enabled = [](const char* name)
        {
            const char* v = std::getenv(name);
            return !v || std::strcmp(v, "0") != 0;
        };
        return enabled("GE_IDLE_ELISION") && enabled("GE_IDLE_ELISION_GRASS");
    }();

    auto it = m_PlacementScratch.find(viewId);
    if (it == m_PlacementScratch.end())
        return false;
    PlacementBuffers& buffers = it->second.Frames[frameIndex];

    GrassPlacementElisionInputs inputs;
    inputs.PlaceParams = std::span<const uint8>(reinterpret_cast<const uint8*>(&buffers.Place),
                                                sizeof(buffers.Place));
    inputs.Plan = buffers.Plan;
    terrainFeature.CopyLastTerrainParamsBytes(buffers.TerrainParamsScratch);
    inputs.TerrainParams = buffers.TerrainParamsScratch;
    inputs.TerrainContentEpoch = terrainFeature.GetGrassPlacementContentEpoch();

    TerrainECS::AtlasGrassSource atlasSource{};
    if (terrainFeature.TryGetAtlasGrassSource(atlasSource))
    {
        inputs.AtlasIdentity = atlasSource.Identity;
        inputs.AtlasTableVersion = atlasSource.TableVersion;
        inputs.AtlasRows = atlasSource.RowBytes;
        buffers.AtlasBindlessScratch = {atlasSource.HeightBindless, atlasSource.HeightCoarseBindless,
                                        atlasSource.NormalBindless, atlasSource.NormalCoarseBindless,
                                        atlasSource.SplatBindless, atlasSource.SplatCoarseBindless,
                                        atlasSource.GrassBindless, atlasSource.GrassCoarseBindless};
        inputs.AtlasBindlessIndices = buffers.AtlasBindlessScratch;
    }
    else
    {
        buffers.AtlasBindlessScratch = {};
        inputs.AtlasBindlessIndices = {};
    }

    // The indirect fields the CPU seeds INSIDE the placement pass; an elided frame does not re-seed
    // them, so a blade-mesh rebuild or a capacity change has to be visible here. Gathered word by
    // word in the order EnsureIndirectConstants writes them: the struct that carries them has
    // padding, and padding bytes are indeterminate.
    uint32 seededWord = 0;
    for (uint32 lod = 0; lod < kGrassLodCount; ++lod)
    {
        buffers.SeededIndirectScratch[seededWord++] = m_BladeLods[lod].IndexCount;
        buffers.SeededIndirectScratch[seededWord++] = m_BladeLods[lod].FirstIndex;
        buffers.SeededIndirectScratch[seededWord++] = static_cast<uint32>(m_BladeLods[lod].VertexOffset);
    }
    buffers.SeededIndirectScratch[seededWord] = buffers.Plan.Capacity;
    inputs.SeededIndirectWords = buffers.SeededIndirectScratch;

    Rendering::ElisionInputBlob blob;
    BuildGrassPlacementElisionBlob(inputs, blob);

    // settleFrames 1: the gate is per (view, FRAME SLOT), so one match already means this slot's
    // own buffers were written under these exact inputs the last time it was visited. The stamp is
    // that slot's visit counter for the same reason — a view that stops being declared for a while
    // has not had its private buffers rewritten by anyone else in the meantime.
    ++buffers.VisitStamp;
    const Rendering::RecomputeElisionGate::Decision decision =
        buffers.Gate.Evaluate(buffers.VisitStamp, std::move(blob), kAllowElision, 1u);

    if (buffers.Gate.ShouldReportWindow(600) && Rendering::IdleElisionLoggingEnabled())
    {
        const Rendering::RecomputeElisionGate::Stats& st = buffers.Gate.GetStats();
        Logger::Log::Info(
            "[IdleElision] Grass window view {} slot {}: eval {} skip {} | first {} forced {} "
            "gap {} changed {} unsettled {} | content epoch {}",
            static_cast<uint32>(viewId), frameIndex, st.Evaluated, st.Skipped,
            st.CauseCounts[static_cast<size_t>(Rendering::ElisionCause::FirstEvaluate)],
            st.CauseCounts[static_cast<size_t>(Rendering::ElisionCause::Forced)],
            st.CauseCounts[static_cast<size_t>(Rendering::ElisionCause::EvaluationGap)],
            st.CauseCounts[static_cast<size_t>(Rendering::ElisionCause::InputsChanged)],
            st.CauseCounts[static_cast<size_t>(Rendering::ElisionCause::NotSettled)],
            inputs.TerrainContentEpoch);
    }
    if (Rendering::IdleElisionLoggingEnabled() && decision.Skip != buffers.GateLogState)
    {
        if (decision.Skip)
            Logger::Log::Info("[IdleElision] Grass engaged: view {} slot {} elided (inputs settled "
                              "{} frames)",
                              static_cast<uint32>(viewId), frameIndex,
                              buffers.Gate.ConsecutiveMatches());
        else
            Logger::Log::Info("[IdleElision] Grass disengaged: view {} slot {}: {}",
                              static_cast<uint32>(viewId), frameIndex,
                              Rendering::ElisionDisengageReason(decision.Cause, decision.Skip));
        buffers.GateLogState = decision.Skip;
    }
    return decision.Skip;
}

void TerrainGrassRenderFeature::DispatchPlacementForView(
    Rendering::RenderGraph::RGContext& ctx,
    Engine::Renderer::RenderServices& rs,
    TerrainECS::TerrainRenderFeature& terrainFeature,
    Rendering::ViewId viewId,
    uint32 frameIndex)
{
    DispatchPlacementBody(ctx.Cmd, ctx.GetDevice(), rs, terrainFeature, viewId, frameIndex);
}

void TerrainGrassRenderFeature::DispatchPlacementBody(
    Rendering::CommandList* cmd,
    Rendering::IDevice* device,
    Engine::Renderer::RenderServices& rs,
    TerrainECS::TerrainRenderFeature& terrainFeature,
    Rendering::ViewId viewId,
    uint32 frameIndex)
{
    // GE_TERRAIN_GRASS_DEBUG: trace where the placement dispatch stops. Each logs once.
    static const bool kDbg = std::getenv("GE_TERRAIN_GRASS_DEBUG") != nullptr;
    auto once = [](bool& flag) { if (flag) return false; flag = true; return true; };

    // Every return below leaves this view's grass wrong for the frame, and each one's stop= trace
    // needs kDbg and fires once per process — so the counters are the only record a shipped editor
    // keeps of a dropped frame. Which side of the FillBuffer a return sits on decides what the
    // frame LOOKS like: before it, the args survive from a full ring ago (stale placement at a
    // healthy count); after it, they are zero (no grass at all).
    using Dropout = TerrainGrassPlacementStats::Dropout;

    if (!m_Initialized || !m_Device)
    {
        m_PlacementStats.RecordDropout(viewId, Dropout::StaleArgs);
        return;
    }
    if (!EnsureComputePipeline(rs))
    {
        m_PlacementStats.RecordDropout(viewId, Dropout::StaleArgs);
        static bool logged = false;
        if (kDbg && once(logged))
            Logger::Log::Info("TerrainGrass.Place stop=pipeline (compute pipeline not ready)");
        return;
    }

    if (!cmd || !device)
    {
        m_PlacementStats.RecordDropout(viewId, Dropout::StaleArgs);
        return;
    }

    auto* buffers = EnsurePlacementBuffers(viewId, frameIndex);
    if (!buffers)
    {
        m_PlacementStats.RecordDropout(viewId, Dropout::StaleArgs);
        static bool logged = false;
        if (kDbg && once(logged))
            Logger::Log::Info("TerrainGrass.Place stop=buffers (no placement buffers)");
        return;
    }

    // The slot the plan, the place params and the elision blob were all resolved against, carried
    // from declaration rather than re-read: the bound array has to be the one the budget bound was
    // computed over, or the plan hands out slots against one terrain table while the kernels read
    // another. Extraction runs before declare on the same thread today, so re-reading would give
    // the same answer — carrying it is what keeps that true if it ever stops being.
    const uint32 paramsSlot = buffers->ParamsSlot;
    const uint32 terrainParamsCount = terrainFeature.GetTerrainParamsCount(paramsSlot);
    const uint32 activeGrassCount = terrainFeature.GetTerrainGrassActiveCount(paramsSlot);
    if (terrainParamsCount == 0 || activeGrassCount == 0)
    {
        m_PlacementStats.RecordDropout(viewId, Dropout::StaleArgs);
        static bool logged = false;
        if (kDbg && once(logged))
            Logger::Log::Info("TerrainGrass.Place stop=counts slot={} params={} active={}",
                              paramsSlot, terrainParamsCount, activeGrassCount);
        return;
    }
    EnsureBladeSegments(terrainFeature.GetTerrainGrassMaxBladeSegments(paramsSlot));

    const auto paramsSSBO = terrainFeature.GetTerrainParamsSSBO(paramsSlot);
    if (!paramsSSBO.IsValid())
    {
        m_PlacementStats.RecordDropout(viewId, Dropout::StaleArgs);
        static bool logged = false;
        if (kDbg && once(logged))
            Logger::Log::Info("TerrainGrass.Place stop=buffers (params SSBO invalid, slot {})",
                              paramsSlot);
        return;
    }

    const GrassPlacementPlan& plan = buffers->Plan;
    if (plan.CellCount == 0)
    {
        m_PlacementStats.RecordDropout(viewId, Dropout::StaleArgs);
        static bool logged = false;
        if (kDbg && once(logged))
            Logger::Log::Info("TerrainGrass.Place stop=plan (no cells; density or range is zero)");
        return;
    }

    // The sub-mesh records and the pool size describe the blade mesh and the budget, not the frame,
    // so they are written once per slot and again only when one of them moves.
    EnsureIndirectConstants(*cmd, *buffers);
    // The ring histogram is the one thing the GPU accumulates into rather than writes, so it is the
    // one thing the frame has to clear. Its range is disjoint from the cell array the same dispatch
    // writes, so the two need no ordering against each other.
    cmd->FillBuffer(buffers->CellPlan, 0,
                    static_cast<uint64>(kGrassRingHistogramWords) * sizeof(uint32), 0u);
    // Nothing else needs resetting. Every instance count and every counter in the block is WRITTEN
    // by the plan dispatch rather than accumulated into, so a stale value cannot survive one; the
    // single accumulator left, AcceptedBlades, is zeroed by that same dispatch before the emit adds
    // to it. The barrier below publishes the constants above to the plan.
    cmd->Barrier(Rendering::ResourceBarrier::CreateMemoryBarrier(
        static_cast<uint64>(Rendering::PipelineStageMask::Transfer),
        static_cast<uint64>(Rendering::PipelineStageMask::ComputeShader),
        static_cast<uint64>(Rendering::ResourceAccessMask::TransferWrite),
        static_cast<uint64>(Rendering::ResourceAccessMask::ShaderRead)
            | static_cast<uint64>(Rendering::ResourceAccessMask::ShaderWrite)));

    // Per-view placement inputs. Written through the persistently-mapped UBO because a staging
    // copy on the transfer queue is not ordered against this frame's graphics-queue compute.
    // Resolved at declaration (EnsurePlacementBuffersForViewRG), because the elision gate
    // compares these exact bytes before deciding whether this pass runs at all.
    const GrassPlaceParamsGPU& place = buffers->Place;
    m_Device->UpdateBuffer(buffers->PlaceParamsUBO, 0, sizeof(place), &place);

    // The draw's copy of the same fit scale, written from the same plan in the same place, so the
    // two stages cannot be given different ideas of where the field ends.
    GrassDrawParamsGPU draw{};
    draw.RangeFitScale = place.CameraPos[3] > 0.0f ? place.CameraPos[3] : 1.0f;
    m_Device->UpdateBuffer(buffers->DrawParamsUBO, 0, sizeof(draw), &draw);

    const bool compat = rs.GetProfile().IsCompat();
    Rendering::DescriptorSetDesc dsDesc{};
    dsDesc.layout = MakeGrassPlaceDescriptorSetLayout(compat);
    dsDesc.transient = true;
    dsDesc.debugName = "TerrainGrassPlace_DS";
    auto ds = device->CreateDescriptorSet(dsDesc);
    if (!ds.IsValid())
    {
        m_PlacementStats.RecordDropout(viewId, Dropout::ZeroGrass);
        static bool logged = false;
        if (kDbg && once(logged))
            Logger::Log::Info("TerrainGrass.Place stop=descriptorset (CreateDescriptorSet failed)");
        return;
    }

    // Atlas resolve inputs (rebuilt once per frame from the single atlas terrain, if any).
    const AtlasBinding atlas = EnsureAtlasBinding(terrainFeature, frameIndex);

    device->UpdateStorageBufferBinding(ds, 0, paramsSSBO, 0,
        static_cast<size_t>(terrainParamsCount) * sizeof(Terrain::TerrainGPUParams));
    device->UpdateStorageBufferBinding(ds, 1, buffers->CellPlan, 0, kGrassCellPlanBytes);
    device->UpdateStorageBufferBinding(ds, 2, buffers->Instances, 0,
        static_cast<size_t>(buffers->Capacity) * sizeof(GrassBladeInstanceGPU));
    device->UpdateStorageBufferBinding(ds, 3, buffers->IndirectArgs, 0,
        sizeof(GrassIndirectBlockGPU));
    if (atlas.ParamsUBO.IsValid())
        device->UpdateBufferBinding(ds, 4, atlas.ParamsUBO, 0, sizeof(GrassAtlasParamsGPU));
    if (atlas.Rows.IsValid())
        device->UpdateStorageBufferBinding(ds, 5, atlas.Rows, 0, atlas.RowsRangeBytes);
    device->UpdateBufferBinding(ds, 6, buffers->PlaceParamsUBO, 0, sizeof(GrassPlaceParamsGPU));

    if (compat)
    {
    // Named map bindings (the compat profile has no binding arrays). The set is not
    // partially-bindable, so every one of the twelve is written every dispatch — white where a
    // map is absent, which is what the shader's own "index == 0" guards already treat as unbound.
    //
    const Rendering::TextureHandle white = rs.Textures().GetDefaultWhiteTexture();
    const auto bindMap = [&](uint32 index, Rendering::TextureHandle tex) {
        device->UpdateImageBinding(ds, kGrassCompatMapBinding0 + index, tex.IsValid() ? tex : white);
    };
    bindMap(0, atlas.HeightTexture);
    bindMap(1, atlas.HeightCoarseTexture);
    bindMap(2, atlas.NormalTexture);
    bindMap(3, atlas.NormalCoarseTexture);
    bindMap(4, atlas.SplatTexture);
    bindMap(5, atlas.SplatCoarseTexture);
    // Unified (non-atlas) maps: route the first non-atlas terrain's source textures to the named
    // slots the compat taps read. Existence rides the params kTerrainFlagHas* bits, so an invalid
    // handle here degrades to the white default AND the shader never taps it.
    Rendering::TextureHandle uniHeight{}, uniNormal{}, uniSplat{}, uniControl{};
    terrainFeature.TryGetUnifiedGrassMaps(uniHeight, uniNormal, uniSplat, &uniControl);
    bindMap(6, uniHeight);
    bindMap(7, uniNormal);
    bindMap(8, uniSplat);
    bindMap(9, atlas.GrassTexture);
    bindMap(10, atlas.GrassCoarseTexture);
    bindMap(11, uniControl);
    device->UpdateSamplerBinding(ds, kGrassCompatMapSamplerBinding,
                                 rs.Textures().GetSampler(Rendering::SamplerPreset::LinearClamp));
    }

    // Compat carries its maps in set 0 (named bindings), so there is no bindless set to bind.
    // Both pipelines share this one set: the plan reads the terrain params and writes the cell
    // ranges, the emit reads those ranges and writes the pool.
    const Rendering::DescriptorSetHandle bindlessSet =
        compat ? Rendering::DescriptorSetHandle{} : rs.Textures().BindlessTextureSet();

    {
        static bool logged = false;
        if (kDbg && once(logged))
            Logger::Log::Info("TerrainGrass.Place DISPATCH cells={} span={} range={} nearDensity={} "
                              "capacity={} planned={}",
                              plan.CellCount, plan.CellSpan, plan.Params.FarRadius,
                              plan.Params.NearDensity, plan.Capacity, plan.PlannedCandidates);
    }
    // One invocation per cell: the frustum test and the terrain footprint walk are per-cell work
    // with nothing to carry between cells, so they run as a grid.
    constexpr uint32 kClassifyWorkgroupSize = 64u;
    cmd->SetPipeline(m_ClassifyPipeline);
    cmd->BindDescriptorSet(0, ds, m_ClassifyPipeline);
    if (bindlessSet.IsValid())
        cmd->BindDescriptorSet(1, bindlessSet, m_ClassifyPipeline);
    cmd->Dispatch((plan.CellCount + kClassifyWorkgroupSize - 1u) / kClassifyWorkgroupSize, 1, 1);
    // The plan reads the histogram and the verdicts the classify just wrote.
    cmd->Barrier(Rendering::ResourceBarrier::CreateMemoryBarrier(
        static_cast<uint64>(Rendering::PipelineStageMask::ComputeShader),
        static_cast<uint64>(Rendering::PipelineStageMask::ComputeShader),
        static_cast<uint64>(Rendering::ResourceAccessMask::ShaderWrite),
        static_cast<uint64>(Rendering::ResourceAccessMask::ShaderRead)
            | static_cast<uint64>(Rendering::ResourceAccessMask::ShaderWrite)));

    // ONE workgroup: a prefix sum over the window has to be carried somewhere, and shared memory is
    // cheaper than another dispatch's worth of round trips through device memory.
    cmd->SetPipeline(m_PlanPipeline);
    cmd->BindDescriptorSet(0, ds, m_PlanPipeline);
    if (bindlessSet.IsValid())
        cmd->BindDescriptorSet(1, bindlessSet, m_PlanPipeline);
    cmd->Dispatch(1, 1, 1);
    // The emit reads every cell range the plan just wrote, and adds to the counter it just zeroed.
    cmd->Barrier(Rendering::ResourceBarrier::CreateMemoryBarrier(
        static_cast<uint64>(Rendering::PipelineStageMask::ComputeShader),
        static_cast<uint64>(Rendering::PipelineStageMask::ComputeShader),
        static_cast<uint64>(Rendering::ResourceAccessMask::ShaderWrite),
        static_cast<uint64>(Rendering::ResourceAccessMask::ShaderRead)
            | static_cast<uint64>(Rendering::ResourceAccessMask::ShaderWrite)));
    // One workgroup per cell of the window: a cell the plan did not admit reads its own plan entry
    // and returns, which is what a frustum-culled cell already cost before.
    cmd->SetPipeline(m_ComputePipeline);
    cmd->BindDescriptorSet(0, ds, m_ComputePipeline);
    if (bindlessSet.IsValid())
        cmd->BindDescriptorSet(1, bindlessSet, m_ComputePipeline);
    cmd->Dispatch(plan.CellCount, 1, 1);
    cmd->Barrier(MakeGrassPlaceReadyBarrier());
}

TerrainGrassRenderFeature::GrassAtlasParamsGPU
TerrainGrassRenderFeature::BuildAtlasParams(const TerrainECS::AtlasGrassSource& src, uint32 rowCapacity)
{
    GrassAtlasParamsGPU p{};
    if (!src.Valid)
        return p; // Enabled = 0 -> the compute skips the atlas path entirely
    p.Enabled = 1u;
    p.AtlasDim = src.AtlasDim;
    p.SlotStride = src.AtlasSlotStride;
    p.SlotsPerRow = src.AtlasSlotsPerRow;
    p.TileRes = src.AtlasTileRes;
    p.TilesPerAxisX = src.AtlasTilesPerAxisX;
    p.TilesPerAxisZ = src.AtlasTilesPerAxisZ;
    p.CoarseDim = src.AtlasCoarseDim;
    p.HeightBindless = src.HeightBindless;
    p.HeightCoarseBindless = src.HeightCoarseBindless;
    p.NormalBindless = src.NormalBindless;
    p.NormalCoarseBindless = src.NormalCoarseBindless;
    p.SplatBindless = src.SplatBindless;
    p.SplatCoarseBindless = src.SplatCoarseBindless;
    p.GrassEnabled = src.GrassRegionsActive ? 1u : 0u;
    p.GrassBindless = src.GrassBindless;
    p.GrassCoarseBindless = src.GrassCoarseBindless;
    // The shader guards its row read with tileIndex < RowCount, so clamp to what the ring slot holds.
    p.RowCount = rowCapacity == 0 ? 0u : std::min(src.RowCount, rowCapacity);
    return p;
}

bool TerrainGrassRenderFeature::AtlasSlotsNeedReset(bool hasAtlas, uint64 currentIdentity,
                                                    uint64 lastIdentity)
{
    return !hasAtlas || currentIdentity != lastIdentity;
}

TerrainGrassRenderFeature::AtlasBinding
TerrainGrassRenderFeature::EnsureAtlasBinding(TerrainECS::TerrainRenderFeature& terrainFeature,
                                              uint32 frameIndex)
{
    const uint32 slot = frameIndex;
    AtlasBinding out{};
    out.ParamsUBO = m_AtlasParamsUBO[slot];
    out.Rows = m_DefaultAtlasRows;
    out.RowsRangeBytes = kGrassAtlasRowBytes;

    TerrainECS::AtlasGrassSource src{};
    bool hasAtlas = terrainFeature.TryGetAtlasGrassSource(src) && src.RowCount > 0;

    // D1: invalidate the whole per-slot version cache when the atlas terrain's identity changes (a
    // destroy+recreate whose new table version could numerically collide with a retained slot
    // version) or when the atlas goes away — otherwise a stale slot would never re-upload and grass
    // would resolve through the dead terrain's rows (the #505/#506 stale-cache class at grass level).
    if (AtlasSlotsNeedReset(hasAtlas, src.Identity, m_AtlasIdentity))
    {
        for (auto& v : m_AtlasRowsSlotVersion)
            v = UINT64_MAX;
        m_AtlasIdentity = hasAtlas ? src.Identity : 0;
    }

    // Grow the per-slot rows buffers so each holds this terrain's rows. A grow defer-destroys the
    // old (still-in-flight) buffers and forces a re-upload into every slot on its next turn.
    if (hasAtlas && src.RowCount > m_AtlasRowsCapacity)
    {
        m_AtlasRowsCapacity = src.RowCount;
        for (uint32 i = 0; i < kMaxFrames; ++i)
        {
            DeferBufferDestroy(m_AtlasRows[i]);
            m_AtlasRows[i] = {};
            m_AtlasRowsSlotVersion[i] = UINT64_MAX;

            Rendering::BufferDesc desc{};
            desc.size = static_cast<size_t>(m_AtlasRowsCapacity) * kGrassAtlasRowBytes;
            desc.usage = static_cast<uint32>(Rendering::BufferUsage::Storage);
            desc.memoryUsage = Rendering::BufferMemoryUsage::Upload;
            desc.persistent = true;
            desc.flags = Rendering::BufferCreateFlags::FrameSlotted;
            desc.debugName = "TerrainGrass_AtlasRows";
            m_AtlasRows[i] = m_Device->CreateBuffer(desc);
            if (m_AtlasRows[i].IsValid())
            {
                if (void* mapped = m_Device->MapBuffer(m_AtlasRows[i]))
                {
                    std::memset(mapped, 0xFF, desc.size); // every row -> kAtlasNoSlot until uploaded
                    m_Device->UnmapBuffer(m_AtlasRows[i]);
                }
            }
        }
    }

    // D3: the atlas path is only viable when THIS frame's rows buffer actually exists — on a buffer
    // creation failure, disable it so the shader can't read a resident slot 0 out of a phantom rows
    // buffer (silent garbage placement, inverting the never-garbage invariant). Disabled params (all
    // zero) keep the compute off the atlas path; the default 1-row NO_SLOT buffer stays bound.
    hasAtlas = hasAtlas && m_AtlasRows[slot].IsValid() && m_AtlasRowsCapacity > 0;
    const GrassAtlasParamsGPU params =
        hasAtlas ? BuildAtlasParams(src, m_AtlasRowsCapacity) : GrassAtlasParamsGPU{};

    // Carried for the no-binding-array profile, which binds these by name. Gated on hasAtlas for
    // the same reason params is: a disabled atlas must not leave live maps bound.
    if (hasAtlas)
    {
        out.HeightTexture = src.HeightTexture;
        out.HeightCoarseTexture = src.HeightCoarseTexture;
        out.NormalTexture = src.NormalTexture;
        out.NormalCoarseTexture = src.NormalCoarseTexture;
        out.SplatTexture = src.SplatTexture;
        out.SplatCoarseTexture = src.SplatCoarseTexture;
        out.GrassTexture = src.GrassTexture;
        out.GrassCoarseTexture = src.GrassCoarseTexture;
    }

    // Binding 4 must always be a valid descriptor. Prefer this frame's slot UBO; if it failed to
    // create, fall back to any valid slot UBO so the binding is never left unwritten (validation).
    if (!out.ParamsUBO.IsValid())
        for (auto& ubo : m_AtlasParamsUBO)
            if (ubo.IsValid())
            {
                out.ParamsUBO = ubo;
                break;
            }
    if (out.ParamsUBO.IsValid())
        m_Device->UpdateBuffer(out.ParamsUBO, 0, sizeof(params), &params);

    if (hasAtlas && params.RowCount > 0)
    {
        out.Rows = m_AtlasRows[slot];
        out.RowsRangeBytes = static_cast<uint64>(params.RowCount) * kGrassAtlasRowBytes;

        // Version gate: a parked camera keeps the same table version, so this slot already holds the
        // current rows -> no re-map/upload. Separate per-slot buffers make the write GPU-safe.
        if (m_AtlasRowsSlotVersion[slot] != src.TableVersion)
        {
            if (void* mapped = m_Device->MapBuffer(m_AtlasRows[slot]))
            {
                const size_t copyBytes = std::min<size_t>(
                    src.RowBytes.size(),
                    static_cast<size_t>(params.RowCount) * kGrassAtlasRowBytes);
                std::memcpy(mapped, src.RowBytes.data(), copyBytes);
                m_Device->UnmapBuffer(m_AtlasRows[slot]);
                m_AtlasRowsSlotVersion[slot] = src.TableVersion;
            }
        }
    }

    // GE_TERRAIN_GRASS_DEBUG: one line of the GRASS FEATURE'S view of the atlas source — the values
    // the compute actually receives (vs the extraction's, which the same flag logs). If atlasActive
    // is 0, grassAtlasBacked() is false in the shader and the atlas terrain falls to the unified path
    // (HeightmapBindless == 0) -> roots at the base plane -> buried; residentRows shows whether the
    // resolve is resident (full precision) or falls to the coarse field.
    static const bool kGrassDebug = std::getenv("GE_TERRAIN_GRASS_DEBUG") != nullptr;
    if (kGrassDebug)
    {
        uint32 residentRows = 0;
        const size_t rows = src.RowBytes.size() / kGrassAtlasRowBytes;
        for (size_t i = 0; i < rows; ++i)
        {
            uint32 rowSlot = 0;
            std::memcpy(&rowSlot, src.RowBytes.data() + i * kGrassAtlasRowBytes, sizeof(rowSlot));
            residentRows += (rowSlot != 0xFFFFFFFFu);
        }
        static uint32 s_LastEnabled = 0xFFFFFFFFu, s_LastResident = 0xFFFFFFFFu;
        const uint32 enabled = params.Enabled;
        if (enabled != s_LastEnabled || residentRows != s_LastResident)
        {
            s_LastEnabled = enabled;
            s_LastResident = residentRows;
            Logger::Log::Info("TerrainGrass.AtlasBinding atlasActive={} srcValid={} rowCount={} "
                              "residentRows={} rowsBufValid={} heightBindless={} splatBindless={} "
                              "coarseDim={}",
                              enabled, src.Valid ? 1u : 0u, src.RowCount, residentRows,
                              m_AtlasRows[slot].IsValid() ? 1u : 0u, src.HeightBindless,
                              src.SplatBindless, src.AtlasCoarseDim);
        }
    }
    m_AtlasBindingBySlot[slot] = out;
    return out;
}

TerrainGrassRenderFeature::AtlasBinding
TerrainGrassRenderFeature::GetAtlasBindingForDraw(uint32 frameIndex) const
{
    const uint32 slot = frameIndex;
    AtlasBinding out = m_AtlasBindingBySlot[slot];
    // Never-ensured slot (a draw in a frame whose placement did not run): the zeroed params UBO and
    // the NO_SLOT default row keep both descriptors valid and the surface on the unified path.
    if (!out.ParamsUBO.IsValid())
        out.ParamsUBO = m_AtlasParamsUBO[slot];
    if (!out.Rows.IsValid())
    {
        out.Rows = m_DefaultAtlasRows;
        out.RowsRangeBytes = kGrassAtlasRowBytes;
    }
    return out;
}

void TerrainGrassRenderFeature::EnsureBladeSegments(uint32 bladeSegments)
{
    bladeSegments = std::clamp(bladeSegments,
        Components::kMinTerrainGrassBladeSegments,
        Components::kMaxTerrainGrassBladeSegments);
    if (bladeSegments == m_BladeSegments && m_BladeVB.IsValid() && m_BladeIB.IsValid())
        return;
    CreateBladeMesh(bladeSegments);
}

void TerrainGrassRenderFeature::CreateBladeMesh(uint32 bladeSegments)
{
    if (!m_Device)
        return;

    bladeSegments = std::clamp(bladeSegments,
        Components::kMinTerrainGrassBladeSegments,
        Components::kMaxTerrainGrassBladeSegments);

    if (m_BladeVB.IsValid())
        m_Device->DestroyBuffer(m_BladeVB);
    if (m_BladeIB.IsValid())
        m_Device->DestroyBuffer(m_BladeIB);
    m_BladeVB = {};
    m_BladeIB = {};
    m_BladeLods = {};
    m_BladeSegments = 0;

    // LOD 1 is a fixed two-segment blade: six vertices against LOD 0's (BladeSegments + 1) * 2,
    // which is half at the shipped BladeSegments of 5. It keeps the silhouette and the wind lean
    // readable on that reduced geometry; the vertex modifier animates it from the same wind field
    // as LOD 0, so the far field gusts with the near one.
    constexpr uint32 kLod1Segments = 2u;
    // Row parameter t = u^(2/3) for u evenly spaced over [0,1] — see the row loop below.
    constexpr float kBladeRowDistributionExponent = 2.0f / 3.0f;
    const uint32 lodSegments[kGrassLodCount] = {
        bladeSegments,
        std::min(bladeSegments, kLod1Segments),
    };

    std::vector<BladeVertex> vertices;
    std::vector<uint32> indices;
    for (uint32 lod = 0; lod < kGrassLodCount; ++lod)
    {
        const uint32 segments = lodSegments[lod];
        BladeLodMesh& mesh = m_BladeLods[lod];
        mesh.VertexOffset = static_cast<int32>(vertices.size());
        mesh.FirstIndex = static_cast<uint32>(indices.size());
        mesh.IndexCount = segments * 6u;

        // Rows are spaced to equalise how far the straight segment between them strays from the
        // spine the vertex stage actually evaluates. That spine puts row t at radius t*height and
        // angle bend*t^2 (terrain_grass_vertex_modifier.glsl), which is the cubic y = bend*x^3 to
        // leading order, so its second derivative grows LINEARLY in t. Chord error goes like
        // t * dt^2, and holding that constant gives t = u^(2/3). Evenly spaced rows spend their
        // budget where the blade is straight and kink at the tip; equal-lean-angle rows (t = u^0.5)
        // overcorrect and move a bigger kink to the root. Mesh-build only — no runtime cost.
        for (uint32 row = 0; row <= segments; ++row)
        {
            const float u = static_cast<float>(row) / static_cast<float>(segments);
            const float t = std::pow(u, kBladeRowDistributionExponent);
            vertices.push_back(BladeVertex{-1.0f, t});
            vertices.push_back(BladeVertex{ 1.0f, t});
        }
        // Indices are LOD-local; the draw record's vertexOffset rebases them into the shared VB.
        for (uint32 row = 0; row < segments; ++row)
        {
            const uint32 l0 = row * 2 + 0;
            const uint32 r0 = row * 2 + 1;
            const uint32 l1 = (row + 1) * 2 + 0;
            const uint32 r1 = (row + 1) * 2 + 1;
            indices.push_back(l0);
            indices.push_back(r0);
            indices.push_back(l1);
            indices.push_back(r0);
            indices.push_back(r1);
            indices.push_back(l1);
        }
    }

    m_BladeVB = Rendering::CreateVertexBuffer(m_Device, vertices.data(),
        vertices.size() * sizeof(BladeVertex), "TerrainGrass_BladeVB");
    m_BladeIB = Rendering::CreateIndexBuffer(m_Device, indices.data(),
        indices.size() * sizeof(uint32), "TerrainGrass_BladeIB");
    m_BladeSegments = bladeSegments;
}

void TerrainGrassRenderFeature::DeferBufferDestroy(Rendering::BufferHandle buf)
{
    if (!buf.IsValid())
        return;
    m_DeferredDestroys.push_back({buf, m_MonotonicFrame});
}

void TerrainGrassRenderFeature::FlushDeferredDestroys()
{
    if (!m_Device)
        return;
    // Advance the retirement clock once per REAL frame: this is called from
    // both arms' per-view entry points, and multi-view frames must not
    // shrink the in-flight window below FramesInFlight. The device frame
    // index cycles, so detect change and count on our own monotonic clock.
    const uint32 deviceFrame = m_Device->GetFrameIndex();
    if (deviceFrame == m_LastFlushDeviceFrame)
        return; // same frame: this flush already ran
    m_LastFlushDeviceFrame = deviceFrame;
    // RETIRE CONDITION: a deferred buffer's last possible GPU read is the
    // frame that deferred it (growth swaps the slot the SAME frame, so no
    // later frame ever binds the old handle). It is safe to call
    // DestroyBuffer once that frame's submission completes — i.e. after
    // FramesInFlight real frames; one extra frame absorbs the declaration-
    // vs-submission gap. The device-frame-keyed clock above guarantees
    // these are REAL frames. DestroyBuffer itself is additionally
    // timeline-deferred by the device, so this ring is belt-and-braces,
    // not the sole safety mechanism.
    constexpr uint32 kRetireAfterFrames = Rendering::IDevice::kMaxSupportedFramesInFlight + 1;
    ++m_MonotonicFrame;
    auto it = m_DeferredDestroys.begin();
    while (it != m_DeferredDestroys.end())
    {
        if (m_MonotonicFrame - it->FrameRetired >= kRetireAfterFrames)
        {
            if (it->Buffer.IsValid())
                m_Device->DestroyBuffer(it->Buffer);
            it = m_DeferredDestroys.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void TerrainGrassRenderFeature::DestroyGpuResources()
{
    if (!m_Device)
        return;

    for (auto& d : m_DeferredDestroys)
    {
        if (d.Buffer.IsValid())
            m_Device->DestroyBuffer(d.Buffer);
    }
    m_DeferredDestroys.clear();

    if (m_BladeVB.IsValid())
        m_Device->DestroyBuffer(m_BladeVB);
    if (m_BladeIB.IsValid())
        m_Device->DestroyBuffer(m_BladeIB);

    m_BladeVB = {};
    m_BladeIB = {};
    m_BladeLods = {};

    for (auto& ubo : m_AtlasParamsUBO)
    {
        if (ubo.IsValid())
            m_Device->DestroyBuffer(ubo);
        ubo = {};
    }
    for (auto& rows : m_AtlasRows)
    {
        if (rows.IsValid())
            m_Device->DestroyBuffer(rows);
        rows = {};
    }
    m_AtlasRowsCapacity = 0;
    if (m_DefaultAtlasRows.IsValid())
        m_Device->DestroyBuffer(m_DefaultAtlasRows);
    m_DefaultAtlasRows = {};

    m_PlacementStats.Destroy(m_Device);
}

void TerrainGrassRenderFeature::OnFrameSubmittedRG(Rendering::RenderGraph::RGFrame& frame,
                                                   const Rendering::IDevice::GpuSyncToken& token)
{
    m_PlacementStats.OnFrameSubmitted(frame, token);
}

void TerrainGrassRenderFeature::OnFrameStreamRetiredRG(Rendering::RenderGraph::RGFrame& frame)
{
    m_PlacementStats.OnFrameStreamRetired(frame);
}

// SCOPED to the placement-stats ring, whose slots are persistently MAPPED: a
// rebuild frees them and the cached pointers dangle, so reading one afterwards is
// a use-after-free that generational handles do not catch.
//
// This is NOT a device-recovery fix for grass. The blade VB/IB, the atlas-params
// and placement-params UBOs and the per-view placement buffers are still dead after a
// rebuild and have no re-create path here — the gap RenderServices records at its
// feature fan-out remains open for this feature. Whoever closes it for the placement
// buffers must go through EnsurePlacementBuffers, whose `buffers = {}` clears
// PlacementBuffers::SeededConstants and the slot's elision gate along with the handles:
// a slot that kept either over a fresh buffer would seed nothing and then elide onto
// content that no longer exists.
void TerrainGrassRenderFeature::OnDeviceRebuilt(Rendering::IDevice* device)
{
    if (!device)
        return;
    m_PlacementStats.OnDeviceRebuilt(device);
}

} // namespace GameEngine::TerrainGrass
