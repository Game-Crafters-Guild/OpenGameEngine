#include "Engine/Rendering/DDGIProbeFeature.h"
#include "Engine/Rendering/DDGIProbeWindow.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>

#include "Engine/Rendering/DDGIGlossyAtlasLayout.h"
#include "Engine/Rendering/DDGIEmissive.h"
#include "Engine/Rendering/DDGIMaterialMapAtlas.h"
#include "Engine/Rendering/DDGISceneService.h"
#include "Engine/Rendering/DDGISkinnedGeometry.h"
#include "Engine/Rendering/IEnvironmentSource.h"  // complete type for GetFeature<ImageBasedLightingFeature>
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/Pipeline/Nodes/PipelineNodeUtils.h"  // Pipeline::Nodes::Detail::BindStorageImageByName
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

namespace GameEngine::Engine::Renderer
{

namespace
{
// GLSL mirror: Includes/ddgi_common.glsl's GE_DDGI_OCT_RES/BORDER/TILE.
constexpr int32_t kOctRes = 6;
constexpr int32_t kBorder = 1;
constexpr int32_t kTile = kOctRes + 2 * kBorder;  // 8
// GLSL mirror: GE_DDGI_DEPTH_OCT_RES_FINE / GE_DDGI_DEPTH_TILE_FINE.
constexpr int32_t kDepthOctResFine = 14;
constexpr int32_t kDepthTileFine = kDepthOctResFine + 2 * kBorder;  // 16

// The interior resolution the kernels address a depth atlas by, from the
// tile edge a cascade was allocated with.
int32_t DepthOctResForTile(int32_t depthTile)
{
    return depthTile - 2 * kBorder;
}

uint64_t DepthStateBytes(int32_t probeTotal, int32_t depthTile)
{
    return static_cast<uint64_t>(std::max(probeTotal, 0)) * static_cast<uint64_t>(depthTile * depthTile) *
           sizeof(float) * 4;
}

// One float per irradiance texel — see CascadeGrid::TemporalState.
uint64_t TemporalStateBytes(int32_t probeTotal)
{
    return static_cast<uint64_t>(std::max(probeTotal, 0)) * static_cast<uint64_t>(kTile * kTile) * sizeof(float);
}

constexpr int32_t kMinProbesPerAxis = 2;
constexpr int32_t kMaxProbesPerAxis = 32;
constexpr float kMaxSaneWorldUnits = 1.0e6f;

// Smallest maximum 2D texture dimension the targets this engine ships to all
// guarantee. Only the glossy atlas can realistically approach it (18x18 tiles
// — see DDGIGlossyAtlasLayout.h), but it is the bound to check against.
constexpr int32_t kMaxAtlasDimension = 8192;

constexpr uint64_t kMaxGlossyHistoryBytes = 256ull * 1024ull * 1024ull;

uint64_t GlossyHistoryBytes(int32_t probeTotal)
{
    const uint64_t texels =
        static_cast<uint64_t>(kDDGIGlossyTile) * static_cast<uint64_t>(kDDGIGlossyTile);
    const uint64_t n = static_cast<uint64_t>(std::max(probeTotal, 0));
    return n * texels * (sizeof(float) * 4 + sizeof(float));
}

// std430-friendly packed light record, laid out to match
// Includes/light_packed_fields.glsl EXACTLY — the same field list
// Includes/ddgi_hit_shade.glsl's GE_DDGILightPacked struct is generated
// from, and the same layout Pipeline::Nodes::LightUploadNode's
// (file-local, non-exported) GPULightPacked already uses for the forward
// pass's own LightBuffer. DDGI cannot bind THAT buffer directly — it is
// republished per-view by a perView=true node, invisible to this frame-scope
// feature's Declare — so it packs an independent copy from the same CPU
// source (RenderServices::GetWorldLights). Two packers, one field list; if
// light_packed_fields.glsl ever changes, both must change together (a
// shared packer function is a reasonable follow-up once a third consumer
// makes the duplication actually cost something).
struct DDGIPackedLight
{
    uint32_t type = 0;
    uint32_t castsShadows = 0;
    uint32_t areaShape = 0;
    uint32_t castsLight = 1;
    float positionWS[3] = {};
    float range = 0.0f;
    float directionWS[3] = {};
    float intensity = 0.0f;
    float color[3] = {};
    float areaWidth = 1.0f;
    float areaHeight = 1.0f;
    float areaRadius = 0.5f;
    float decay = 2.0f;
    float spotCosInner = 1.0f;
    float spotCosOuter = 1.0f;
    float fogContribution = 1.0f;
    float fogDensityBoost = 0.0f;
    float fogOriginFade = 0.2f;
    float areaRightWS[3] = {1.0f, 0.0f, 0.0f};
    float fogAnisotropy = 0.25f;
    float areaUpWS[3] = {0.0f, 1.0f, 0.0f};
    float falloffMode = 0.0f;
    int32_t shadowSlot = -1;  // unshadowed for DDGI NEE purposes (probes ignore point-shadow atlas slots)
    int32_t shadowSlotReserved[3] = {-1, -1, -1};
};
static_assert(sizeof(DDGIPackedLight) == 144, "must match light_packed_fields.glsl's stride");

constexpr uint32_t kDDGIMaxLights = 256;  // NEE loop cost bound — see ddgi_hit_shade.glsl's cost note

// A published emitter proxy is a LOCAL light: without a finite range every
// probe in the volume shades against it whatever the distance. Scaled off the
// source radius rather than fixed, so a large emitter reaches proportionally
// further, with a floor so a tiny one still lights its immediate surroundings.
constexpr float GE_PI_F = 3.14159265358979323846f;
constexpr float kDDGIEmitterRangeScale = 32.0f;
constexpr float kDDGIEmitterMinRange = 8.0f;
}  // namespace

DDGIProbeFeature::DDGIProbeFeature() = default;
DDGIProbeFeature::~DDGIProbeFeature()
{
    ReleaseCascadeGpuResources(m_C0);
    ReleaseCascadeGpuResources(m_C1);
    ReleaseReflectionResources();
    m_VariabilityRing.Destroy(m_Device);
    if (m_ProbeStateFallback.IsValid() && m_Device)
        m_Device->DestroyBuffer(m_ProbeStateFallback);
    if (m_SceneAS && m_TlasSlot.IsValid())
        m_SceneAS->ReleaseTlasChannel(m_TlasSlot);
    for (const RetiredBuffer& r : m_RetiredInstanceBuffers)
        if (r.Buffer.IsValid() && m_Device)
            m_Device->DestroyBuffer(r.Buffer);
    if (m_TlasInstanceStaging.IsValid() && m_Device)
        m_Device->DestroyBuffer(m_TlasInstanceStaging);
    m_SceneService.reset();  // destroys its own GPU buffers before m_Device dangles
    m_MapAtlas.reset();   // ...and so does the atlas (which the service points at)
}

bool DDGIProbeFeature::Initialize(Rendering::IDevice* device, SceneAccelerationStructureService* sceneAS,
                                  Rendering::GPUScene* gpuScene, Rendering::MeshGPURegistry* meshRegistry,
                                  MaterialSystem* materials)
{
    // sceneAS may be null (non-ray-query device) — DDGINode still calls this
    // so the software lane can initialize; gpuScene/meshRegistry/materials
    // are required regardless of lane, since DDGISceneService (software) and
    // the hardware TLAS channel are both provisioned here, unconditionally,
    // and DeclareProbePasses picks the lane per tick.
    if (!device || !gpuScene || !meshRegistry || !materials)
        return false;
    m_Device = device;
    m_SceneAS = sceneAS;
    m_GpuScene = gpuScene;
    m_MeshRegistry = meshRegistry;
    m_Materials = materials;
    if (!m_SkinnedGeometry)
        m_SkinnedGeometry = std::make_unique<DDGISkinnedGeometry>(device, meshRegistry, sceneAS);
    if (!m_MapAtlas)
        m_MapAtlas = std::make_unique<DDGIMaterialMapAtlas>(m_Device, m_Materials);
    if (!m_SceneService)
        m_SceneService = std::make_unique<DDGISceneService>(m_Device, m_MeshRegistry, m_GpuScene,
                                                            m_Materials, m_MapAtlas.get());
    return true;
}

void DDGIProbeFeature::OnDeviceRebuilt(Rendering::IDevice* device)
{
    // Every cached GPU handle below is dead after a device rebuild (see
    // IRenderFeature::OnDeviceRebuilt's doc — IsValid() alone cannot tell).
    // Forget them all; the next Declare re-creates and re-clears from
    // scratch, exactly like a first enable.
    m_Device = device;
    for (CascadeGrid* grid : {&m_C0, &m_C1})
    {
        grid->RayBuffer = {};
        grid->IrradianceState = {};
        grid->IrradianceAtlas = {};
        grid->DepthState = {};
        grid->DepthAtlas = {};
        grid->TemporalState = {};
        grid->ProbeStateBuffer = {};
        grid->ProbeCellBuffer = {};
        for (Rendering::BufferHandle& b : grid->VolumeDataBuffer)
            b = {};
        grid->GpuGridValid = false;
        grid->AtlasEverUploaded = false;
        grid->NeedsClear = true;
        // Force EnsureCascadeGpuResources to reallocate immediately on the
        // next Declare rather than waiting out the idle gate — every handle
        // above is already dead, there is nothing to debounce against.
        grid->ProbeCount[0] = grid->ProbeCount[1] = grid->ProbeCount[2] = 0;
        grid->AllocatedRaysPerProbe = 0;
        grid->DepthTile = 0;
        grid->LastRequestedProbeCount[0] = grid->LastRequestedProbeCount[1] =
            grid->LastRequestedProbeCount[2] = -1;
        grid->LastRequestedRaysPerProbe = -1;
        grid->LastRequestedDepthTile = -1;
        grid->StructuralIdleTimerMs = kStructuralIdleGateMs;
    }
    m_AtlasSampler = {};
    m_GlossyResolveDepthSampler = {};
    m_ProbeStateFallback = {};
    m_RetiredInstanceBuffers.clear();
    m_TlasInstanceStaging = {};
    m_StagedInstanceHash = 0;
    m_TlasContentValid = false;
    m_BuiltInstanceHash = 0;
    m_TlasBuildExecuted.reset();
    m_PendingTlasHash = 0;
    m_HaveContentEpoch = false;
    m_LastContentEpoch = 0;
    m_C0Refl = {};
    m_C1Refl = {};
    m_Classify = {};
    m_ClassifySw = {};
    m_TraceHw = {};
    m_TraceSw = {};
    m_Blend = {};
    m_DepthBlend = {};
    m_Upload = {};
    m_DepthUpload = {};
    m_Variability = {};
    m_Clear = {};
    // Persistently mapped Readback slots the rebuild freed — drop the ring so
    // the next Throttle tick re-creates it (ExposureReadbackFeature's rule).
    m_VariabilityRing.Destroy(device);
    m_SolveBudget.SetConvergedThrottle(false);
    m_RoughBlend = {};
    m_RoughUpload = {};
    m_GlossyBlend = {};
    m_GlossyUpload = {};
    m_MapBlit = {};
    m_SwRefit = {};

    // The skinned cache is device-scoped end to end — posed buffers,
    // per-instance BLAS handles, its own compute pipeline and its
    // load-attempted latch (which would otherwise pin a dead pipeline
    // forever). Abandon rather than destroy: destroying a stale-generation
    // BLAS handle against the rebuilt backend could hit a recycled slot.
    if (m_SkinnedGeometry)
        m_SkinnedGeometry->AbandonDeviceObjects(device);

    // DDGISceneService owns its own GPU buffers against the OLD device —
    // reconstruct it fresh against the new one rather than trying to salvage
    // any state, the same "forget everything, rebuild from scratch on the
    // next tick" discipline every cascade's GPU state above just applied.
    m_SceneService.reset();
    m_MapAtlas.reset();  // destroyed before the service that holds a pointer to it is rebuilt
    if (m_MeshRegistry && m_Materials)
    {
        m_MapAtlas = std::make_unique<DDGIMaterialMapAtlas>(m_Device, m_Materials);
        m_SceneService = std::make_unique<DDGISceneService>(m_Device, m_MeshRegistry, m_GpuScene,
                                                            m_Materials, m_MapAtlas.get());
    }
}

void DDGIProbeFeature::SetActiveVolume(const DDGIVolumeDesc& desc)
{
    DDGIVolumeDesc clamped = desc;
    clamped.ProbesLongAxis = std::clamp(clamped.ProbesLongAxis, kMinProbesPerAxis, kMaxProbesPerAxis);
    clamped.RaysPerProbe = std::clamp(clamped.RaysPerProbe, 32, 256);
    clamped.Hysteresis = std::clamp(clamped.Hysteresis, 0.0f, 0.99f);
    clamped.FineCascadeExtentFraction = std::clamp(clamped.FineCascadeExtentFraction, 0.05f, 0.9f);
    // The shaders read these straight out of the UBO with no further guard, so
    // the clamps that keep their math well-defined belong here: a negative
    // clamp/band would flip the sign of a rolloff, and a Chebyshev strength
    // outside [0,1] would extrapolate the visibility mix past both endpoints.
    clamped.BounceIntensity = std::max(clamped.BounceIntensity, 0.0f);
    clamped.RadianceClamp = std::max(clamped.RadianceClamp, 0.0f);
    clamped.FireflyClamp = std::clamp(clamped.FireflyClamp, 1.0f, 20.0f);
    clamped.ChangeThreshold = std::clamp(clamped.ChangeThreshold, 0.5f, 8.0f);
    clamped.SnapAmount = std::clamp(clamped.SnapAmount, 0.0f, 0.9f);
    clamped.NormalBiasScale = std::clamp(clamped.NormalBiasScale, 0.0f, 8.0f);
    clamped.ChebyshevStrength = std::clamp(clamped.ChebyshevStrength, 0.0f, 1.0f);
    clamped.ClassifyStrength = std::clamp(clamped.ClassifyStrength, 0.0f, 1.0f);
    clamped.SkyIntensity = std::max(clamped.SkyIntensity, 0.0f);
    // Floored above 0 on purpose: the blend kernel raises each ray's cosine
    // weight to this power, and pow(0, 0) is 1 — a zero exponent would give
    // every backfacing ray full authority over the depth moments.
    clamped.DepthSharpness = std::clamp(clamped.DepthSharpness, 0.01f, 200.0f);
    clamped.FilterStrength = std::clamp(clamped.FilterStrength, 0.0f, 1.0f);
    clamped.FilterSmoothness = std::clamp(clamped.FilterSmoothness, 0.0f, 1.0f);
    // Serialized as a raw int32; an out-of-table value degrades to the
    // nearest valid scale rather than an undefined resolve extent.
    clamped.GlossyResolveScale =
        std::clamp(clamped.GlossyResolveScale, Components::DDGIGlossyResolveScale::Quarter,
                   Components::DDGIGlossyResolveScale::Full);
    clamped.DepthResolution = std::clamp(clamped.DepthResolution, Components::DDGIDepthResolution::Shared,
                                         Components::DDGIDepthResolution::Fine);
    clamped.ConvergedSolve = std::clamp(clamped.ConvergedSolve, Components::DDGIConvergedSolve::Continuous,
                                        Components::DDGIConvergedSolve::Throttle);

    // A placement-mode flip re-clears both grids. Leaving Adaptive would
    // otherwise strand the relocation offsets and inactive flags its classify
    // ticks wrote in the probe-state buffer, and Grid mode never rewrites
    // them — so "Grid" would silently keep rendering an adapted field. The
    // clear also gives a fresh Adaptive run a defined starting point instead
    // of an arbitrarily stale one.
    const bool placementChanged = clamped.ProbePlacement != m_Volume.ProbePlacement;
    const bool shapeChanged =
        !m_Volume.Enabled != !clamped.Enabled || clamped.ProbesLongAxis != m_Volume.ProbesLongAxis ||
        std::memcmp(clamped.GridSizeWS, m_Volume.GridSizeWS, sizeof(clamped.GridSizeWS)) != 0;
    const bool movedAtAll =
        std::memcmp(clamped.GridMinWS, m_Volume.GridMinWS, sizeof(clamped.GridMinWS)) != 0;

    // A pure TRANSLATION does not invalidate the field. Probe storage slots are
    // keyed on absolute world cell modulo resolution (Includes/ddgi_common.glsl),
    // so a grid that slides by whole cells leaves every probe still inside it in
    // its own slot, history intact — only the newly entered slab inherits a
    // stale slot, and the blend kernel's own change detection (ChangeThreshold /
    // SnapAmount) pulls those texels to the fresh estimate within a few solves.
    //
    // Sliding FARTHER than the grid is wide is the exception: nothing overlaps,
    // every slot is stale, and healing them individually would be slower and
    // uglier than starting clean.
    bool outranTheGrid = false;
    if (movedAtAll && !shapeChanged)
    {
        int32_t probeCount[3];
        ComputeProbeGridLayout(clamped.GridSizeWS, clamped.ProbesLongAxis, probeCount);
        for (int axis = 0; axis < 3 && !outranTheGrid; ++axis)
        {
            const float spans = static_cast<float>(std::max(probeCount[axis] - 1, 1));
            const float spacing = std::max(clamped.GridSizeWS[axis] / spans, 1.0e-4f);
            const float movedCells =
                std::abs(clamped.GridMinWS[axis] - m_Volume.GridMinWS[axis]) / spacing;
            outranTheGrid = movedCells >= static_cast<float>(probeCount[axis]);
        }
    }
    const bool geometryChanged = shapeChanged || (movedAtAll && outranTheGrid);
    const bool fineCascadeGeometryChanged =
        !m_Volume.EnableFineCascade != !clamped.EnableFineCascade ||
        clamped.FineCascadeExtentFraction != m_Volume.FineCascadeExtentFraction;
    m_Volume = clamped;
    if (geometryChanged || placementChanged)
    {
        // C0's grid moved/resized -> C1's derived sub-region moved/resized too
        // (ComputeFineCascadeBounds always derives from m_Volume fresh).
        m_C0.NeedsClear = true;
        m_C1.NeedsClear = true;
    }
    else if (fineCascadeGeometryChanged)
    {
        m_C1.NeedsClear = true;
    }
}

bool DDGIProbeFeature::LoadKernel(const char* shaderPkgPath, KernelPipeline& out, const char* debugName)
{
    if (out.LoadAttempted)
        return out.PipelineId.IsValid();
    out.LoadAttempted = true;

    Rendering::ShaderPackage pkg{};
    std::string loadErr;
    if (!Rendering::LoadShaderPkg(shaderPkgPath, m_Device->PreferredShaderSource(), pkg, &loadErr))
    {
        Logger::Log::Warning("DDGIProbeFeature: failed to load {}: {}", shaderPkgPath, loadErr);
        return false;
    }
    auto itCs = pkg.stageBytes.find("cs");
    if (itCs == pkg.stageBytes.end() || itCs->second.empty())
    {
        Logger::Log::Warning("DDGIProbeFeature: {} missing cs stage", shaderPkgPath);
        return false;
    }

    out.Meta = std::make_unique<Rendering::ShaderMeta>(std::move(pkg.meta));

    Rendering::ComputePipelineDesc cd{};
    cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(itCs->second));
    cd.DebugName = debugName;

    out.Set0Layout = Rendering::DescriptorSetLayoutDesc{};
    auto patchLayout = [&](uint32_t setIndex, Rendering::DescriptorSetLayoutDesc& dsl)
    {
        if (setIndex == 0)
            out.Set0Layout = dsl;
    };
    std::string err;
    Rendering::MaterialHelper::ApplyShaderMetaToComputeDesc(
        *m_Device, *out.Meta, cd, Rendering::MaterialBuilder::MergeMode::Auto, {true, 128}, patchLayout,
        &err);

    out.PipelineId = m_Device->InternComputePipeline(std::move(cd));
    return out.PipelineId.IsValid();
}

void DDGIProbeFeature::LoadKernelsIfNeeded()
{
    LoadKernel("Shaders/ddgi_classify.shaderpkg", m_Classify, "DDGI.Classify");
    LoadKernel("Shaders/ddgi_classify_sw.shaderpkg", m_ClassifySw, "DDGI.ClassifySW");
    LoadKernel("Shaders/ddgi_trace_hw.shaderpkg", m_TraceHw, "DDGI.TraceHW");
    LoadKernel("Shaders/ddgi_trace_sw.shaderpkg", m_TraceSw, "DDGI.TraceSW");
    LoadKernel("Shaders/ddgi_bvh_refit.shaderpkg", m_SwRefit, "DDGI.SwRefit");
    LoadKernel("Shaders/ddgi_blend.shaderpkg", m_Blend, "DDGI.Blend");
    LoadKernel("Shaders/ddgi_upload.shaderpkg", m_Upload, "DDGI.Upload");
    LoadKernel("Shaders/ddgi_depth_blend.shaderpkg", m_DepthBlend, "DDGI.DepthBlend");
    LoadKernel("Shaders/ddgi_depth_upload.shaderpkg", m_DepthUpload, "DDGI.DepthUpload");
    LoadKernel("Shaders/ddgi_variability.shaderpkg", m_Variability, "DDGI.Variability");
    LoadKernel("Shaders/ddgi_clear.shaderpkg", m_Clear, "DDGI.Clear");
    LoadKernel("Shaders/ddgi_rough_blend.shaderpkg", m_RoughBlend, "DDGI.RoughBlend");
    LoadKernel("Shaders/ddgi_rough_upload.shaderpkg", m_RoughUpload, "DDGI.RoughUpload");
    LoadKernel("Shaders/ddgi_glossy_blend.shaderpkg", m_GlossyBlend, "DDGI.GlossyBlend");
    LoadKernel("Shaders/ddgi_glossy_upload.shaderpkg", m_GlossyUpload, "DDGI.GlossyUpload");
    LoadKernel("Shaders/ddgi_map_atlas_blit.shaderpkg", m_MapBlit, "DDGI.MapBlit");
}

void DDGIProbeFeature::ComputeVolumeHalfExtents(const float worldMatrix[16], float outHalfExtents[3])
{
    // Column-major: columns 0/1/2 are the transform's basis vectors, whose
    // lengths are the per-axis scale (rotation is a unit-length rotation of
    // each column, so taking the length drops it — which is what v1's
    // world-axis-aligned grid wants). Unit cube => half-extent 0.5 * scale.
    for (int axis = 0; axis < 3; ++axis)
    {
        const float x = worldMatrix[axis * 4 + 0];
        const float y = worldMatrix[axis * 4 + 1];
        const float z = worldMatrix[axis * 4 + 2];
        const float scale = std::sqrt(x * x + y * y + z * z);
        // A collapsed or mirrored transform would otherwise give an empty or
        // inverted grid; clamp to something small but positive so the volume
        // stays well-formed and visibly tiny rather than silently absent.
        outHalfExtents[axis] = std::max(scale * 0.5f, 1.0e-3f);
    }
}

Mathematics::Vector3 DDGIProbeFeature::SnapCentreToProbeGrid(const Mathematics::Vector3& cameraWS,
                                                             const float gridSizeWS[3],
                                                             int32_t probesLongAxis)
{
    int32_t probeCount[3];
    ComputeProbeGridLayout(gridSizeWS, probesLongAxis, probeCount);
    Mathematics::Vector3 snapped;
    for (int axis = 0; axis < 3; ++axis)
    {
        // Cell spacing is over the SPANS between probes, not the probes
        // themselves: a row of N probes across a box of size S has N-1 gaps.
        // Using N here would drift the lattice off the box by one gap.
        const float spans = static_cast<float>(std::max(probeCount[axis] - 1, 1));
        const float spacing = std::max(gridSizeWS[axis] / spans, 1.0e-4f);
        snapped[axis] = std::round(cameraWS[axis] / spacing) * spacing;
    }
    return snapped;
}

void DDGIProbeFeature::ComputeProbeGridLayout(const float gridSizeWS[3], int32_t probesLongAxis,
                                              int32_t outCounts[3])
{
    // Longest-axis-gets-probesLongAxis, others scaled by aspect (matches the
    // ported library's TARGET_PROBES_LONG_AXIS convention).
    const float sx = std::max(gridSizeWS[0], 1e-3f);
    const float sy = std::max(gridSizeWS[1], 1e-3f);
    const float sz = std::max(gridSizeWS[2], 1e-3f);
    const float longest = std::max({sx, sy, sz});
    auto axisCount = [&](float size)
    {
        return std::clamp(static_cast<int32_t>(std::round(probesLongAxis * (size / longest))),
                          kMinProbesPerAxis, kMaxProbesPerAxis);
    };
    outCounts[0] = axisCount(sx);
    outCounts[1] = axisCount(sy);
    outCounts[2] = axisCount(sz);
}

namespace
{
// M5: C1's world-space bounds — a smaller box of `fraction` x C0's extents,
// sharing C0's center. Pure function of the volume desc; called from both
// DeclareProbePasses (to size/place C1's dispatch) and UploadVolumeDataFine
// (to fill the consumer's DDGIVolumeData UBO) so the two never drift apart.
void ComputeFineCascadeBounds(const DDGIVolumeDesc& volume, float outMinWS[3], float outSizeWS[3])
{
    for (int i = 0; i < 3; ++i)
    {
        const float center = volume.GridMinWS[i] + volume.GridSizeWS[i] * 0.5f;
        const float size = std::max(volume.GridSizeWS[i] * volume.FineCascadeExtentFraction, 1e-3f);
        outSizeWS[i] = size;
        outMinWS[i] = center - size * 0.5f;
    }
}

}  // namespace

bool DDGIProbeFeature::EnsureCascadeGpuResources(CascadeGrid& grid, const float gridSizeWS[3],
                                                 int32_t probesLongAxis, int32_t rawRaysPerProbe,
                                                 int32_t depthTile, float deltaTimeSeconds,
                                                 const CascadeDebugNames& names)
{
    int32_t newCount[3];
    ComputeProbeGridLayout(gridSizeWS, probesLongAxis, newCount);
    // The trace kernels stride their 64-thread workgroup over the ray set, so
    // the component's whole 32..256 range is traced and the buffers size to it.
    const int32_t effectiveRays = rawRaysPerProbe;

    // Idle gate: a request that differs from the last OBSERVED request
    // resets the timer; a steady request accumulates toward the gate.
    const bool requestChanged = newCount[0] != grid.LastRequestedProbeCount[0] ||
                                newCount[1] != grid.LastRequestedProbeCount[1] ||
                                newCount[2] != grid.LastRequestedProbeCount[2] ||
                                effectiveRays != grid.LastRequestedRaysPerProbe ||
                                depthTile != grid.LastRequestedDepthTile;
    grid.LastRequestedProbeCount[0] = newCount[0];
    grid.LastRequestedProbeCount[1] = newCount[1];
    grid.LastRequestedProbeCount[2] = newCount[2];
    grid.LastRequestedRaysPerProbe = effectiveRays;
    grid.LastRequestedDepthTile = depthTile;
    grid.StructuralIdleTimerMs =
        requestChanged ? 0.0f : grid.StructuralIdleTimerMs + std::max(deltaTimeSeconds, 0.0f) * 1000.0f;

    const bool haveResources = grid.RayBuffer.IsValid() && grid.IrradianceState.IsValid() &&
                               grid.IrradianceAtlas.IsValid() && grid.DepthState.IsValid() &&
                               grid.DepthAtlas.IsValid() && grid.TemporalState.IsValid() &&
                               grid.ProbeStateBuffer.IsValid() && grid.ProbeCellBuffer.IsValid();
    const bool differsFromAllocated = newCount[0] != grid.ProbeCount[0] || newCount[1] != grid.ProbeCount[1] ||
                                      newCount[2] != grid.ProbeCount[2] ||
                                      effectiveRays != grid.AllocatedRaysPerProbe ||
                                      depthTile != grid.DepthTile;
    const bool needsRealloc =
        !haveResources || (differsFromAllocated && grid.StructuralIdleTimerMs >= kStructuralIdleGateMs);

    // Bias (etc.) reads MinCellWS every frame regardless of whether a
    // reallocation is due — recomputed from the CURRENT extents against the
    // ALLOCATED probe count, so normal-bias scaling stays live while the
    // component's extents are actively being dragged, not frozen until the
    // idle gate fires.
    {
        const float sx = std::max(gridSizeWS[0], 1e-3f);
        const float sy = std::max(gridSizeWS[1], 1e-3f);
        const float sz = std::max(gridSizeWS[2], 1e-3f);
        const int32_t (&allocCount)[3] = grid.ProbeCount;
        grid.MinCellWS = std::min({sx / std::max(allocCount[0] - 1, 1), sy / std::max(allocCount[1] - 1, 1),
                                   sz / std::max(allocCount[2] - 1, 1)});
    }

    if (!needsRealloc)
        return false;

    ReleaseCascadeGpuResources(grid);

    grid.ProbeCount[0] = newCount[0];
    grid.ProbeCount[1] = newCount[1];
    grid.ProbeCount[2] = newCount[2];
    grid.ProbeTotal = newCount[0] * newCount[1] * newCount[2];
    grid.AllocatedRaysPerProbe = effectiveRays;
    grid.DepthTile = depthTile;
    grid.AtlasWidth = newCount[0] * kTile;
    grid.AtlasHeight = newCount[1] * newCount[2] * kTile;
    {
        const float sx = std::max(gridSizeWS[0], 1e-3f);
        const float sy = std::max(gridSizeWS[1], 1e-3f);
        const float sz = std::max(gridSizeWS[2], 1e-3f);
        grid.MinCellWS = std::min({sx / std::max(grid.ProbeCount[0] - 1, 1),
                                   sy / std::max(grid.ProbeCount[1] - 1, 1),
                                   sz / std::max(grid.ProbeCount[2] - 1, 1)});
    }

    Rendering::BufferDesc rayDesc{};
    rayDesc.size = static_cast<uint64_t>(grid.ProbeTotal) * static_cast<uint64_t>(grid.AllocatedRaysPerProbe) *
                  sizeof(float) * 4;
    rayDesc.usage = static_cast<uint32_t>(Rendering::BufferUsage::Storage);
    rayDesc.memoryUsage = Rendering::BufferMemoryUsage::DeviceLocal;
    rayDesc.debugName = names.RayBuffer;
    grid.RayBuffer = m_Device->CreateBuffer(rayDesc);

    Rendering::BufferDesc stateDesc{};
    stateDesc.size =
        static_cast<uint64_t>(grid.ProbeTotal) * static_cast<uint64_t>(kTile * kTile) * sizeof(float) * 4;
    stateDesc.usage = static_cast<uint32_t>(Rendering::BufferUsage::Storage);
    stateDesc.memoryUsage = Rendering::BufferMemoryUsage::DeviceLocal;
    stateDesc.debugName = names.IrradianceState;
    grid.IrradianceState = m_Device->CreateBuffer(stateDesc);

    Rendering::BufferDesc depthStateDesc = stateDesc;
    depthStateDesc.size = DepthStateBytes(grid.ProbeTotal, grid.DepthTile);
    depthStateDesc.debugName = names.DepthState;
    grid.DepthState = m_Device->CreateBuffer(depthStateDesc);

    Rendering::BufferDesc temporalStateDesc = stateDesc;
    temporalStateDesc.size = TemporalStateBytes(grid.ProbeTotal);
    temporalStateDesc.debugName = names.TemporalState;
    grid.TemporalState = m_Device->CreateBuffer(temporalStateDesc);

    Rendering::BufferDesc probeStateDesc{};
    probeStateDesc.size = static_cast<uint64_t>(grid.ProbeTotal) * sizeof(float) * 4;
    probeStateDesc.usage = static_cast<uint32_t>(Rendering::BufferUsage::Storage);
    probeStateDesc.memoryUsage = Rendering::BufferMemoryUsage::DeviceLocal;
    probeStateDesc.debugName = names.ProbeState;
    grid.ProbeStateBuffer = m_Device->CreateBuffer(probeStateDesc);

    Rendering::BufferDesc probeCellDesc = probeStateDesc;
    probeCellDesc.debugName = names.ProbeCell;
    grid.ProbeCellBuffer = m_Device->CreateBuffer(probeCellDesc);

    Rendering::TextureDesc atlasDesc{};
    atlasDesc.width = static_cast<uint32_t>(grid.AtlasWidth);
    atlasDesc.height = static_cast<uint32_t>(grid.AtlasHeight);
    atlasDesc.depth = 1;
    atlasDesc.mipLevels = 1;
    atlasDesc.arrayLayers = 1;
    atlasDesc.sampleCount = 1;
    atlasDesc.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
    atlasDesc.usage = static_cast<uint32_t>(Rendering::TextureUsage::UnorderedAccess |
                                            Rendering::TextureUsage::ShaderResource);
    // Upload storage-writes these atlases and the trace SAMPLES them, so their
    // sampled descriptors must claim GENERAL rather than ping-ponging the layout
    // every frame — ShadowMinMaxPyramid.cpp states the same invariant for the
    // same shape. Without it the graph re-asserts ShaderResource on import while
    // the previous frame's write left the image in General, which on Vulkan
    // trips VUID-vkCmdDispatch-None-09600 and emits a barrier with a false
    // oldLayout. initialState makes it true from creation, ahead of the first
    // per-subresource transition. (Metal has no image layouts, which is why an
    // all-Metal evidence set cannot see this.)
    atlasDesc.sampledInGeneralLayout = true;
    atlasDesc.initialState = Rendering::ResourceState::UnorderedAccess;
    atlasDesc.debugName = names.IrradianceAtlas;
    grid.IrradianceAtlas = m_Device->CreateTexture(atlasDesc);

    Rendering::TextureDesc depthAtlasDesc = atlasDesc;
    depthAtlasDesc.debugName = names.DepthAtlas;
    if (grid.DepthTile != kTile)
    {
        // Fine: its own tile grid, same RGBA16F format as Shared since both
        // moment pairs are sampled (ddgi_depth_upload.comp).
        depthAtlasDesc.width = static_cast<uint32_t>(newCount[0] * grid.DepthTile);
        depthAtlasDesc.height = static_cast<uint32_t>(newCount[1] * newCount[2] * grid.DepthTile);
    }
    grid.DepthAtlas = m_Device->CreateTexture(depthAtlasDesc);

    if (!m_AtlasSampler.IsValid())
        m_AtlasSampler =
            m_Device->CreateSampler(Rendering::SamplerDesc::MaterialLinearClamp("DDGI.AtlasSampler"));

    grid.NeedsClear = true;
    grid.AtlasEverUploaded = false;  // fresh/reallocated atlas texture — nothing written into it yet
    grid.ProbeCursor = 0;  // fresh grid: restart the round-robin budget from probe 0
    grid.GpuGridValid = grid.RayBuffer.IsValid() && grid.IrradianceState.IsValid() &&
                        grid.IrradianceAtlas.IsValid() && grid.DepthState.IsValid() &&
                        grid.DepthAtlas.IsValid() && grid.TemporalState.IsValid() &&
                        grid.ProbeStateBuffer.IsValid() && grid.ProbeCellBuffer.IsValid();
    if (!grid.GpuGridValid)
        Logger::Log::Warning("DDGIProbeFeature: {} GPU resource allocation failed at {}x{}x{} probes",
                             names.Prefix, grid.ProbeCount[0], grid.ProbeCount[1], grid.ProbeCount[2]);
    return true;
}

void DDGIProbeFeature::ReleaseCascadeGpuResources(CascadeGrid& grid)
{
    if (grid.RayBuffer.IsValid() && m_Device)
        m_Device->DestroyBuffer(grid.RayBuffer);
    if (grid.IrradianceState.IsValid() && m_Device)
        m_Device->DestroyBuffer(grid.IrradianceState);
    if (grid.IrradianceAtlas.IsValid() && m_Device)
        m_Device->DestroyTexture(grid.IrradianceAtlas);
    if (grid.DepthState.IsValid() && m_Device)
        m_Device->DestroyBuffer(grid.DepthState);
    if (grid.DepthAtlas.IsValid() && m_Device)
        m_Device->DestroyTexture(grid.DepthAtlas);
    if (grid.TemporalState.IsValid() && m_Device)
        m_Device->DestroyBuffer(grid.TemporalState);
    if (grid.ProbeCellBuffer.IsValid() && m_Device)
        m_Device->DestroyBuffer(grid.ProbeCellBuffer);
    if (grid.ProbeStateBuffer.IsValid() && m_Device)
        m_Device->DestroyBuffer(grid.ProbeStateBuffer);
    if (m_Device)
        for (Rendering::BufferHandle& b : grid.VolumeDataBuffer)
            if (b.IsValid())
                m_Device->DestroyBuffer(b);
    grid.RayBuffer = {};
    grid.IrradianceState = {};
    grid.IrradianceAtlas = {};
    grid.DepthState = {};
    grid.DepthAtlas = {};
    grid.TemporalState = {};
    grid.ProbeStateBuffer = {};
    grid.ProbeCellBuffer = {};
    for (Rendering::BufferHandle& b : grid.VolumeDataBuffer)
        b = {};
}

void DDGIProbeFeature::DispatchClear(Rendering::RenderGraph::RGFrame& frame, Rendering::BufferHandle target,
                                     uint32_t elementCount, const float fillValue[4], const char* passName)
{
    namespace RG = Rendering::RenderGraph;
    // A clear is a WRITER of the buffer its consumers read this frame. Phase is
    // a sort key among ready clusters, not a partition, so "clear runs before
    // trace" is a heuristic until an edge says so. Dedup is by physical handle
    // and never reads sizeBytes (ImportExternalBuffer returns early on the
    // dedup path), so importing here cannot conflict with the trace's import.
    const RG::RGBuffer targetRG = frame.ImportExternalBuffer(passName, target, 0);

    struct ClearParamsUBO
    {
        uint32_t ElementCount[4];
        float FillValue[4];
    };
    static_assert(sizeof(ClearParamsUBO) == 32, "must match ddgi_clear.comp's DDGIClearParams");

    auto clearUb = frame.AllocUpload<ClearParamsUBO>();
    if (!clearUb.Valid())
        return;
    clearUb.Ptr->ElementCount[0] = elementCount;
    clearUb.Ptr->ElementCount[1] = 0;
    clearUb.Ptr->ElementCount[2] = 0;
    clearUb.Ptr->ElementCount[3] = 0;
    clearUb.Ptr->FillValue[0] = fillValue[0];
    clearUb.Ptr->FillValue[1] = fillValue[1];
    clearUb.Ptr->FillValue[2] = fillValue[2];
    clearUb.Ptr->FillValue[3] = fillValue[3];

    const uint64_t targetBytes = static_cast<uint64_t>(elementCount) * sizeof(float) * 4;
    const Rendering::ComputePipelineId clearPipe = m_Clear.PipelineId;
    const Rendering::DescriptorSetLayoutDesc clearLayout = m_Clear.Set0Layout;
    const Rendering::ShaderMeta* clearMeta = m_Clear.Meta.get();
    // Copy passName into a std::string BEFORE the lambda captures it: M5's
    // per-cascade call sites pass a temporary (std::string(prefix)+"...").c_str()
    // whose backing storage dies at the end of THIS call's full expression —
    // capturing the raw pointer into a lambda that runs later (at command
    // recording, not declare time) would dangle. Capturing an owned
    // std::string by value is safe regardless of the caller's storage.
    const std::string debugNameOwned = passName;
    frame.AddComputePass(
        passName, Rendering::PassPhase::kEarlySetup,
        [targetRG](RG::RGPassBuilder& p)
        {
            p.PreventCulling();
            p.Write(targetRG, RG::RGBufferWrite::Storage);
        },
        [clearUb, target, targetBytes, clearPipe, clearLayout, clearMeta, elementCount,
         debugNameOwned](RG::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl || !clearMeta)
                return;
            Rendering::DescriptorSetDesc dsDesc{};
            dsDesc.layout = clearLayout;
            dsDesc.transient = true;
            dsDesc.debugName = debugNameOwned.c_str();
            auto ds = dev->CreateDescriptorSet(dsDesc);
            Rendering::NamedDescriptorWriter wd(dev, ds, *clearMeta, 0);
            // Reflected UBO bindings resolve by the block's INSTANCE name,
            // not its type name — every DDGI kernel names its instance
            // "DDGIParams" regardless of the block's own type name (here
            // "DDGIClearParams"). NamedDescriptorWriter::AddUniformBuffer
            // silently no-ops on a name-lookup miss (no error, no assert),
            // so this was a real bug across every DDGI compute UBO, not just
            // this one — see the sibling fixes at every other AddUniformBuffer
            // call in this file for the same correction.
            wd.AddUniformBuffer("DDGIParams", clearUb.Buffer, clearUb.Offset, sizeof(ClearParamsUBO));
            wd.AddStorageBuffer("DDGIClearTarget", target, 0, targetBytes);
            wd.Flush();
            Rendering::PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(clearPipe);
            if (!pipe.IsValid())
                return;
            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            cl->Dispatch((elementCount + 255) / 256, 1, 1);
        });
}

namespace
{
struct VolumeDataUBO
{
    float GridMinWS[4];
    float GridSizeWS[4];
    int32_t ProbeCount[4];  // xyz = res, w = total
    float Params0[4];       // x=minCellWS, y=normalBiasScale, z=chebyshevStrength, w=classifyStrength
    float Params1[4];       // x=enabled, y=intensity, z=glossyEnabled, w=reflectionIntensity
    int32_t Params2[4];     // x=glossyTilesX, yz=glossy atlas size (texels)
    int32_t Params3[4];     // x=DDGIDebugView (diagnostic overlay; 0 = off)
};
static_assert(sizeof(VolumeDataUBO) == 112, "must match Includes/ddgi_probes.glsl's DDGIVolumeData");
}  // namespace

size_t DDGIProbeFeature::GetVolumeDataSize()
{
    return sizeof(VolumeDataUBO);
}

Rendering::BufferHandle DDGIProbeFeature::UploadCascadeVolumeData(CascadeGrid& grid, const float gridMinWS[3],
                                                                   const float gridSizeWS[3],
                                                                   float normalBiasScale, float intensity,
                                                                   bool convergedEnabled, bool glossyEnabled,
                                                                   float reflectionIntensity,
                                                                   const DDGIGlossyAtlasLayout& glossyLayout,
                                                                   const char* debugName)
{
    Rendering::IDevice* device = m_Device;
    grid.VolumeDataCursor = (grid.VolumeDataCursor + 1) % CascadeGrid::kVolumeDataFrameSlots;
    Rendering::BufferHandle& slot = grid.VolumeDataBuffer[grid.VolumeDataCursor];
    if (!slot.IsValid())
    {
        Rendering::BufferDesc desc{};
        desc.size = sizeof(VolumeDataUBO);
        desc.usage = static_cast<uint32_t>(Rendering::BufferUsage::Uniform);
        desc.memoryUsage = Rendering::BufferMemoryUsage::Upload;
        desc.flags = Rendering::BufferCreateFlags::PersistentlyMapped;
        desc.debugName = debugName;
        slot = device->CreateBuffer(desc);
    }
    void* mapped = slot.IsValid() ? device->MapBuffer(slot) : nullptr;
    if (!mapped)
        return slot;

    VolumeDataUBO ubo{};
    ubo.GridMinWS[0] = gridMinWS[0];
    ubo.GridMinWS[1] = gridMinWS[1];
    ubo.GridMinWS[2] = gridMinWS[2];
    ubo.GridSizeWS[0] = gridSizeWS[0];
    ubo.GridSizeWS[1] = gridSizeWS[1];
    ubo.GridSizeWS[2] = gridSizeWS[2];
    ubo.ProbeCount[0] = grid.ProbeCount[0];
    ubo.ProbeCount[1] = grid.ProbeCount[1];
    ubo.ProbeCount[2] = grid.ProbeCount[2];
    ubo.ProbeCount[3] = grid.ProbeTotal;
    ubo.Params0[0] = grid.MinCellWS;
    ubo.Params0[1] = normalBiasScale;
    // Read straight off the volume rather than threaded through this
    // function's parameter list: unlike the per-cascade geometry above it is
    // one value shared by both cascades AND by the trace kernels' bounce
    // gather (DeclareCascadeGridPasses writes the same field into
    // TraceParamsUBO::Params1[1]), and the two gathers must agree.
    ubo.Params0[2] = m_Volume.ChebyshevStrength;
    // Same reasoning as ChebyshevStrength above: one value shared by both
    // cascades and by the trace kernels (TraceParamsUBO::Params1[3]), so it
    // is read off the volume rather than threaded through this signature.
    ubo.Params0[3] = m_Volume.ClassifyStrength;
    ubo.Params1[0] = convergedEnabled ? 1.0f : 0.0f;
    ubo.Params1[1] = intensity;
    ubo.Params1[2] = glossyEnabled ? 1.0f : 0.0f;
    ubo.Params1[3] = reflectionIntensity;
    // Glossy atlas packing is near-square and independent of this grid's
    // probe counts, so the consumer cannot derive it and must be told.
    // TilesX == 0 is the gather's "no sharp lobe" gate (history cap refused
    // this cascade's glossy allocation).
    ubo.Params3[0] = static_cast<int32_t>(m_Volume.DebugView);
    // The depth atlas's interior tile resolution — the ONE value every reader
    // addresses it by (GE_DDGIDepthTexelUV); the trace kernels get the same
    // number in their own UBO (TraceParamsUBO::GridMinWS[3]).
    ubo.Params3[1] = DepthOctResForTile(std::max(grid.DepthTile, kTile));
    ubo.Params3[2] = 0;
    ubo.Params3[3] = 0;
    ubo.Params2[0] = glossyLayout.TilesX;
    ubo.Params2[1] = glossyLayout.WidthTexels;
    ubo.Params2[2] = glossyLayout.HeightTexels;
    // Read off feature state like ChebyshevStrength above, and deliberately
    // NOT per call site: GlossyResolveConsumeActive() is frame-coherent, so
    // every ring slot written this frame is identical and slot reuse across
    // the frame's several uploads cannot bind a pass to a different flag.
    // Written for both cascades; the consumer reads only C0's uParams2.w
    // (Includes/ddgi_probes.glsl).
    ubo.Params2[3] = GlossyResolveConsumeActive() ? 1 : 0;
    std::memcpy(mapped, &ubo, sizeof(ubo));
    return slot;
}

Rendering::BufferHandle DDGIProbeFeature::UploadVolumeData(Rendering::IDevice* /*device*/)
{
    return UploadCascadeVolumeData(m_C0, m_Volume.GridMinWS, m_Volume.GridSizeWS, m_Volume.NormalBiasScale,
                                   m_Volume.Intensity, HasConvergedVolume(),
                                   m_Volume.EnableGlossy && m_C0Refl.GpuValid, m_Volume.ReflectionIntensity,
                                   m_C0Refl.GlossyLayout, "DDGI.C0.VolumeData");
}

DDGIProbeFeature::ProbeStateBinding DDGIProbeFeature::ResolveProbeStateBinding(const CascadeGrid& grid)
{
    if (grid.ProbeStateBuffer.IsValid())
        return {grid.ProbeStateBuffer, CascadeProbeStateBytes(grid)};

    if (!m_ProbeStateFallback.IsValid() && m_Device)
    {
        Rendering::BufferDesc desc{};
        desc.size = kFallbackProbeStateBytes;
        desc.usage = static_cast<uint32_t>(Rendering::BufferUsage::Storage);
        // Host-visible so the zero fill below is a memset rather than a compute
        // clear that would have to be declared into someone's render graph.
        desc.memoryUsage = Rendering::BufferMemoryUsage::Upload;
        desc.flags = Rendering::BufferCreateFlags::PersistentlyMapped;
        desc.debugName = "DDGI.ProbeState.Fallback";
        m_ProbeStateFallback = m_Device->CreateBuffer(desc);
        if (void* mapped = m_ProbeStateFallback.IsValid() ? m_Device->MapBuffer(m_ProbeStateFallback)
                                                          : nullptr)
            std::memset(mapped, 0, static_cast<size_t>(kFallbackProbeStateBytes));
    }
    return {m_ProbeStateFallback, kFallbackProbeStateBytes};
}

DDGIProbeFeature::ProbeStateBinding DDGIProbeFeature::GetProbeStateBinding()
{
    return ResolveProbeStateBinding(m_C0);
}

DDGIProbeFeature::ProbeStateBinding DDGIProbeFeature::GetProbeStateBindingFine()
{
    return ResolveProbeStateBinding(m_C1);
}

DDGIProbeFeature::GatherReadsRG DDGIProbeFeature::ImportGatherReads(Rendering::RenderGraph::RGFrame& frame) const
{
    const std::pair<const char*, Rendering::TextureHandle> atlases[] = {
        {kC0DebugNames.IrradianceAtlas, m_C0.IrradianceAtlas}, {kC0DebugNames.DepthAtlas, m_C0.DepthAtlas},
        {kC1DebugNames.IrradianceAtlas, m_C1.IrradianceAtlas}, {kC1DebugNames.DepthAtlas, m_C1.DepthAtlas},
        {kC0DebugNames.RoughAtlas, m_C0Refl.RoughAtlas},       {kC0DebugNames.GlossyAtlas, m_C0Refl.GlossyAtlas},
        {kC1DebugNames.RoughAtlas, m_C1Refl.RoughAtlas},       {kC1DebugNames.GlossyAtlas, m_C1Refl.GlossyAtlas}};
    GatherReadsRG reads;
    static_assert(std::size(atlases) == std::size(reads.Atlases));
    for (size_t i = 0; i < std::size(atlases); ++i)
        if (atlases[i].second.IsValid())
            reads.Atlases[i] = frame.ImportExternalTexture(atlases[i].first, atlases[i].second,
                                                           Rendering::ResourceState::UnorderedAccess);
    const std::pair<const char*, const CascadeGrid*> grids[] = {{kC0DebugNames.ProbeState, &m_C0},
                                                                {kC1DebugNames.ProbeState, &m_C1}};
    static_assert(std::size(grids) == std::size(reads.ProbeStates));
    for (size_t i = 0; i < std::size(grids); ++i)
        if (grids[i].second->ProbeStateBuffer.IsValid())
            reads.ProbeStates[i] = frame.ImportExternalBuffer(grids[i].first, grids[i].second->ProbeStateBuffer,
                                                              CascadeProbeStateBytes(*grids[i].second));
    return reads;
}

Rendering::BufferHandle DDGIProbeFeature::UploadVolumeDataFine(Rendering::IDevice* /*device*/)
{
    float fineMinWS[3];
    float fineSizeWS[3];
    ComputeFineCascadeBounds(m_Volume, fineMinWS, fineSizeWS);
    return UploadCascadeVolumeData(m_C1, fineMinWS, fineSizeWS, m_Volume.NormalBiasScale, m_Volume.Intensity,
                                   HasConvergedFineCascade(), m_Volume.EnableGlossy && m_C1Refl.GpuValid,
                                   m_Volume.ReflectionIntensity, m_C1Refl.GlossyLayout, "DDGI.C1.VolumeData");
}

void DDGIProbeFeature::DeclareCascadeGridPasses(Rendering::RenderGraph::RGFrame& frame, CascadeGrid& grid,
                                                const float gridMinWS[3], const float gridSizeWS[3],
                                                const SharedTickInputs& shared, const CascadeDebugNames& names,
                                                uint32_t* outProbeBaseThisTick, uint32_t* outProbesThisTick)
{
    namespace RG = Rendering::RenderGraph;
    if (!grid.GpuGridValid)
        return;

    const bool fullUpload = grid.NeedsClear || !grid.AtlasEverUploaded ||
        grid.AtlasUploadState.NeedsFullUpload(gridMinWS, gridSizeWS,
                                             m_Volume.FilterStrength, m_Volume.FilterSmoothness);

    // Track relocation writes through to both tracing lanes. Classification
    // also invalidates the cell history when its sampling position changes.
    const RG::RGBuffer probeStateRG =
        frame.ImportExternalBuffer(names.ProbeState, grid.ProbeStateBuffer, CascadeProbeStateBytes(grid));
    // Fine depth reads the invalidated record before diffuse blend restamps it.
    const RG::RGBuffer probeCellRG =
        frame.ImportExternalBuffer(names.ProbeCell, grid.ProbeCellBuffer, CascadeProbeStateBytes(grid));
    const Rendering::BufferHandle probeCellBuf = grid.ProbeCellBuffer;
    const uint64_t probeCellBytes = CascadeProbeStateBytes(grid);
    // The two per-texel state buffers likewise: the atlases they feed are not
    // readable through capture_resource (no linear texel size), so this is
    // the only way to see what the blend actually wrote for a probe.
    const uint64_t texelStateBytes =
        static_cast<uint64_t>(grid.ProbeTotal) * uint64_t(kTile * kTile) * sizeof(float) * 4;
    const uint64_t depthStateBytes = DepthStateBytes(grid.ProbeTotal, grid.DepthTile);
    const uint64_t temporalStateBytes = TemporalStateBytes(grid.ProbeTotal);
    const RG::RGBuffer irradianceStateRG =
        frame.ImportExternalBuffer(names.IrradianceState, grid.IrradianceState, texelStateBytes);
    const RG::RGBuffer depthStateRG =
        frame.ImportExternalBuffer(names.DepthState, grid.DepthState, depthStateBytes);
    // Blend writes temporal history before the convergence reduction reads it.
    const RG::RGBuffer temporalStateRG =
        frame.ImportExternalBuffer(names.TemporalState, grid.TemporalState, temporalStateBytes);
    const int32_t depthOctRes = DepthOctResForTile(grid.DepthTile);
    const bool fineDepth = grid.DepthTile != kTile;

    // Classification, trace and blend share one contiguous solve window.
    // Persistent atlas tiles outside it retain their last upload. A placement
    // or filter change refreshes the whole atlas, including invalid scroll cells.
    const uint32_t probesPerTick = std::max<uint32_t>(
        1u, shared.RaysPerTickBudget / static_cast<uint32_t>(std::max(grid.AllocatedRaysPerProbe, 1)));
    const DDGIProbeWindow window =
        ComputeDDGIProbeWindow(grid.ProbeCursor, probesPerTick, static_cast<uint32_t>(grid.ProbeTotal));
    const uint32_t probeBaseThisTick = window.Base;
    const uint32_t probesThisTick = window.Count;
    grid.ProbeCursor = window.NextCursor;
    if (outProbeBaseThisTick)
        *outProbeBaseThisTick = probeBaseThisTick;
    if (outProbesThisTick)
        *outProbesThisTick = probesThisTick;

    // Clear (grid just (re)allocated). Runs in the SAME frame as the first
    // trace. kEarlySetup sorts ahead of kDefault (Trace/Blend/Upload), but
    // phase is a sort key among ready clusters rather than a partition, so the
    // ordering that actually binds is the declared edge: DispatchClear writes
    // each target through an imported RGBuffer and the trace/blend read the
    // same physical handle. Four targets: irradiance/depth state default to
    // zero; probe state defaults to zero offset + ACTIVE (w=1), so a freshly
    // allocated probe is usable immediately rather than spuriously excluded
    // until its first classify tick lands.
    if (grid.NeedsClear)
    {
        const uint32_t stateElementCount = static_cast<uint32_t>(grid.ProbeTotal) * uint32_t(kTile * kTile);
        const float zeroFill[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        DispatchClear(frame, grid.IrradianceState, stateElementCount, zeroFill, names.ClearIrradiance);
        DispatchClear(frame, grid.DepthState,
                      static_cast<uint32_t>(grid.ProbeTotal) * uint32_t(grid.DepthTile * grid.DepthTile),
                      zeroFill, names.ClearDepth);
        // One float per irradiance texel, cleared as vec4 elements.
        DispatchClear(frame, grid.TemporalState, stateElementCount / 4u, zeroFill, names.ClearTemporal);
        const float probeStateFill[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        DispatchClear(frame, grid.ProbeStateBuffer, static_cast<uint32_t>(grid.ProbeTotal), probeStateFill,
                     names.ClearProbeState);
        // Unstamped: every slot reads as "changed cell" on its first blend,
        // which is the same seeding path a fresh allocation already takes
        // (GE_DDGI_PROBE_CELL_UNSTAMPED, Includes/ddgi_common.glsl).
        const float probeCellFill[4] = {-1.0e30f, -1.0e30f, -1.0e30f, 0.0f};
        DispatchClear(frame, grid.ProbeCellBuffer, static_cast<uint32_t>(grid.ProbeTotal), probeCellFill,
                     names.ClearProbeCell);
        // Initialize every ray as a black miss so a declared trace pass that
        // fails to execute cannot expose uninitialized allocation contents.
        // Windowed uploads refresh only the probes visited this tick; the
        // allocation-wide clear is therefore required before the first window.
        // Blend kernels separately reject non-finite traced radiance and
        // discard poisoned history before arithmetic. A bad later trace can
        // darken one visit but cannot permanently contaminate the state.
        // The -1 hit distance uses the same miss encoding as traced rays.
        const float rayMissFill[4] = {0.0f, 0.0f, 0.0f, -1.0f};
        const uint32_t rayElementCount =
            static_cast<uint32_t>(grid.ProbeTotal) * static_cast<uint32_t>(grid.AllocatedRaysPerProbe);
        DispatchClear(frame, grid.RayBuffer, rayElementCount, rayMissFill, names.ClearRayBuffer);
        grid.NeedsClear = false;
    }

    // Classify: reads/refines each probe's own relocation offset + active
    // flag (ddgi_classify.comp's doc) from short local rays against the
    // scene. Runs after Clear (a freshly allocated probe starts at a
    // well-defined zero-offset/active state) but before Trace, so Trace reads
    // THIS tick's offset, not last tick's.
    //
    // This whole block IS adaptive probe placement (the default). In
    // DDGIProbePlacement::Grid no classify pass is declared on either lane and
    // every probe keeps its clear-time state (zero offset, active) — the bare
    // lattice, and the A/B control for what Adaptive changed. Switching
    // placement modes re-clears the grid (SetActiveVolume), so a Grid-mode
    // field never inherits offsets an earlier Adaptive tick wrote.
    //
    // Both lanes classify: ddgi_classify.comp walks the hardware TLAS via
    // GL_EXT_ray_query, ddgi_classify_sw.comp walks the same pooled software
    // BVH ddgi_trace_sw.comp does. Same ray set, same burial test, same
    // relocation math, same ge_ddgiProbeState output — only the traversal and
    // therefore the bindings differ.
    struct ClassifyParamsUBO
    {
        float GridMinWS[4];
        float GridSizeWS[4];
        int32_t ProbeCount[4];
        float Params0[4];
        uint32_t Dispatch0[4];
    };
    static_assert(sizeof(ClassifyParamsUBO) == 80, "must match ddgi_classify.comp's DDGIClassifyParams");
    // The software kernel's DDGIClassifyParams is that SAME layout plus one
    // trailing uvec4 (uSwScene) — the identical relationship TraceSwParamsUBO
    // has to TraceParamsUBO below. Embedding rather than duplicating keeps
    // every std140 offset shared and the field fill single-sourced; a separate
    // struct with its own assert rather than loosening the 80-byte one.
    struct ClassifySwParamsUBO
    {
        ClassifyParamsUBO Base;
        uint32_t SwScene[4];  // x=tlasNodeCount, y=instanceBase, z=tlasBase, w=unused
    };
    static_assert(sizeof(ClassifySwParamsUBO) == 96,
                  "must match ddgi_classify_sw.comp's DDGIClassifyParams");

    // A classify kernel that failed to load degrades placement to Grid for
    // this tick rather than taking the whole field down with it: the probes
    // simply keep their clear-time state, which is a correct (if unadapted)
    // field, and Trace/Blend/Upload still run.
    const bool classifyKernelReady = shared.UseSoftwareLane ? m_ClassifySw.PipelineId.IsValid()
                                                            : m_Classify.PipelineId.IsValid();
    // ClassifyStrength 0 zeroes both of this pass's outputs everywhere they
    // are applied, so dispatching it would burn a ray set per tick to write
    // state nothing reads. Skipping it is what makes 0 the free A/B control
    // against Adaptive rather than merely a visual one, and matches the
    // reference's own `if (classifyStrength > 0)` prep gate (gi_probes.js:2690).
    if (m_Volume.ProbePlacement == Components::DDGIProbePlacement::Adaptive && classifyKernelReady &&
        m_Volume.ClassifyStrength > 0.0f)
    {
        ClassifyParamsUBO cp{};
        cp.GridMinWS[0] = gridMinWS[0];
        cp.GridMinWS[1] = gridMinWS[1];
        cp.GridMinWS[2] = gridMinWS[2];
        cp.GridSizeWS[0] = gridSizeWS[0];
        cp.GridSizeWS[1] = gridSizeWS[1];
        cp.GridSizeWS[2] = gridSizeWS[2];
        cp.ProbeCount[0] = grid.ProbeCount[0];
        cp.ProbeCount[1] = grid.ProbeCount[1];
        cp.ProbeCount[2] = grid.ProbeCount[2];
        cp.ProbeCount[3] = grid.ProbeTotal;
        cp.Params0[0] = grid.MinCellWS;
        cp.Dispatch0[0] = probeBaseThisTick;
        cp.Dispatch0[1] = probesThisTick;
        cp.Dispatch0[2] = static_cast<uint32_t>(shared.RayEpoch);  // ray-set rotation (M4)

        const Rendering::BufferHandle probeStateBuf = grid.ProbeStateBuffer;
        const uint64_t probeStateBytes = CascadeProbeStateBytes(grid);

        if (shared.UseSoftwareLane)
        {
            auto classifyUb = frame.AllocUpload<ClassifySwParamsUBO>();
            if (classifyUb.Valid())
            {
                ClassifySwParamsUBO swp{};
                swp.Base = cp;
                swp.SwScene[0] = shared.SwTlasNodeCount;
                swp.SwScene[1] = shared.SwInstanceBase;
                swp.SwScene[2] = shared.SwTlasBase;
                *classifyUb.Ptr = swp;

                const Rendering::ComputePipelineId classifyPipe = m_ClassifySw.PipelineId;
                const Rendering::DescriptorSetLayoutDesc classifyLayout = m_ClassifySw.Set0Layout;
                const Rendering::ShaderMeta* classifyMeta = m_ClassifySw.Meta.get();
                frame.AddComputePass(
                    names.ClassifySW, Rendering::PassPhase::kEarlySetup,
                    [swNodesRG = shared.SwNodesRG, swVertexDataRG = shared.SwVertexDataRG,
                     swPackedSceneRG = shared.SwPackedSceneRG, probeCellRG, probeStateRG](RG::RGPassBuilder& p)
                    {
                        p.PreventCulling();
                        p.Read(swPackedSceneRG, RG::RGBufferRead::Storage);
                        // Order after the skinned BVH refit's writes — the
                        // classify walk traverses the same pooled tree.
                        if (swNodesRG.IsValid())
                            p.Read(swNodesRG, RG::RGBufferRead::Storage);
                        if (swVertexDataRG.IsValid())
                            p.Read(swVertexDataRG, RG::RGBufferRead::Storage);
                        p.Write(probeCellRG, RG::RGBufferWrite::Storage);
                        p.Write(probeStateRG, RG::RGBufferWrite::Storage);
                    },
                    [classifyUb, probeStateBuf, probeStateBytes, probeCellBuf, probeCellBytes, classifyPipe,
                     classifyLayout, classifyMeta,
                     probesThisTick, swPackedScene = shared.SwPackedScene,
                     swPackedSceneBytes = shared.SwPackedSceneBytes, swNodes = shared.SwNodes,
                     swNodesBytes = shared.SwNodesBytes, swTriangleIndices = shared.SwTriangleIndices,
                     swTriangleIndicesBytes = shared.SwTriangleIndicesBytes,
                     swVertexData = shared.SwVertexData,
                     swVertexDataBytes = shared.SwVertexDataBytes](RG::RGContext& ctx)
                    {
                        auto* dev = ctx.GetDevice();
                        auto* cl = ctx.Cmd;
                        if (!dev || !cl || !classifyMeta)
                            return;
                        Rendering::DescriptorSetDesc dsDesc{};
                        dsDesc.layout = classifyLayout;
                        dsDesc.transient = true;
                        dsDesc.debugName = "DDGI.ClassifySW.Set0";
                        auto ds = dev->CreateDescriptorSet(dsDesc);
                        Rendering::NamedDescriptorWriter wd(dev, ds, *classifyMeta, 0);
                        // Instance name, not block name — see DDGIClearParams's fix.
                        wd.AddUniformBuffer("DDGIParams", classifyUb.Buffer, classifyUb.Offset,
                                            sizeof(ClassifySwParamsUBO));
                        wd.AddStorageBuffer("DDGIProbeState", probeStateBuf, 0, probeStateBytes);
                        wd.AddStorageBuffer("DDGIProbeCellRW", probeCellBuf, 0, probeCellBytes);
                        wd.AddStorageBuffer("DDGISwPackedScene", swPackedScene, 0, swPackedSceneBytes);
                        wd.AddStorageBuffer("DDGISwNodes", swNodes, 0, swNodesBytes);
                        wd.AddStorageBuffer("DDGISwTriangleIndices", swTriangleIndices, 0,
                                            swTriangleIndicesBytes);
                        wd.AddStorageBuffer("DDGISwVertexData", swVertexData, 0, swVertexDataBytes);
                        wd.Flush();
                        Rendering::PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(classifyPipe);
                        if (!pipe.IsValid())
                            return;
                        cl->SetPipeline(pipe);
                        cl->BindDescriptorSet(0, ds, pipe);
                        cl->Dispatch(probesThisTick, 1, 1);
                    });
            }
        }
        else
        {
            auto classifyUb = frame.AllocUpload<ClassifyParamsUBO>();
            if (classifyUb.Valid())
            {
                *classifyUb.Ptr = cp;

                const Rendering::ComputePipelineId classifyPipe = m_Classify.PipelineId;
                const Rendering::DescriptorSetLayoutDesc classifyLayout = m_Classify.Set0Layout;
                const Rendering::ShaderMeta* classifyMeta = m_Classify.Meta.get();
                const Rendering::TlasSlotHandle tlasSlot = m_TlasSlot;
                const RG::RGAccelerationStructure tlasRG = frame.ImportAccelerationStructure("DDGI.TLAS", tlasSlot);
                frame.AddComputePass(
                    names.Classify, Rendering::PassPhase::kEarlySetup,
                    [probeCellRG, probeStateRG, tlasRG](RG::RGPassBuilder& p)
                    {
                        p.PreventCulling();
                        p.Read(tlasRG);
                        p.Write(probeCellRG, RG::RGBufferWrite::Storage);
                        p.Write(probeStateRG, RG::RGBufferWrite::Storage);
                    },
                    [this, classifyUb, probeStateBuf, probeStateBytes, probeCellBuf, probeCellBytes, classifyPipe,
                     classifyLayout, classifyMeta, probesThisTick, tlasSlot](RG::RGContext& ctx)
                    {
                        auto* dev = ctx.GetDevice();
                        auto* cl = ctx.Cmd;
                        if (!dev || !cl || !classifyMeta)
                            return;
                        Rendering::DescriptorSetDesc dsDesc{};
                        dsDesc.layout = classifyLayout;
                        dsDesc.transient = true;
                        dsDesc.debugName = "DDGI.Classify.Set0";
                        auto ds = dev->CreateDescriptorSet(dsDesc);
                        Rendering::NamedDescriptorWriter wd(dev, ds, *classifyMeta, 0);
                        // Instance name, not block name — see DDGIClearParams's fix.
                        wd.AddUniformBuffer("DDGIParams", classifyUb.Buffer, classifyUb.Offset,
                                            sizeof(ClassifyParamsUBO));
                        wd.AddStorageBuffer("DDGIProbeState", probeStateBuf, 0, probeStateBytes);
                        wd.AddStorageBuffer("DDGIProbeCellRW", probeCellBuf, 0, probeCellBytes);
                        if (!wd.TryAddAccelerationStructure("uTlas", tlasSlot))
                        {
                            m_HardwareLaneFailed.store(true, std::memory_order_release);
                            Logger::Log::Error(
                                "DDGI: ddgi_classify.comp reflects no 'uTlas' acceleration-structure "
                                "binding — falling back to the software lane");
                            return;
                        }
                        wd.Flush();
                        Rendering::PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(classifyPipe);
                        if (!pipe.IsValid())
                        {
                            // Hardware pipeline variant creation failed (e.g. this
                            // backend can't translate the ray-query GLSL — see
                            // m_HardwareLaneFailed's doc). Sticky: don't retry
                            // every tick, fall back to the software lane instead.
                            m_HardwareLaneFailed.store(true, std::memory_order_release);
                            return;
                        }
                        cl->SetPipeline(pipe);
                        cl->BindDescriptorSet(0, ds, pipe);
                        cl->Dispatch(probesThisTick, 1, 1);
                    });
            }
        }
    }

    // Trace: one workgroup per probe in [probeBase, probeBase+probesThisTick),
    // GE_DDGI_TRACE_THREADS threads striding over the ray set (ddgi_trace_hw.comp).
    // Infinite bounce reads THIS cascade's own previous-frame
    // atlas (self-referential, same as the ported library's trick) — a
    // documented M5 simplification: a physically tighter design would have a
    // fine cascade bounce off the coarse cascade for hit points outside its
    // own small volume, but that needs a second grid-info block threaded
    // through the trace kernel's UBO, deferred until a concrete look-dev pass
    // asks for it.
    struct TraceParamsUBO
    {
        float GridMinWS[4];
        float GridSizeWS[4];
        int32_t ProbeCount[4];
        float Params0[4];
        uint32_t Dispatch0[4];
        float Params1[4];  // x=radianceClamp, y=chebyshevStrength, z=skyScale, w=classifyStrength
    };
    static_assert(sizeof(TraceParamsUBO) == 96,
                  "must match ddgi_trace_hw.comp's DDGIVolumeParams");
    // M7: the software kernel's DDGIVolumeParams is this SAME layout plus one
    // trailing uvec4 (uSwScene) — see ddgi_trace_sw.comp's doc. Embedding
    // TraceParamsUBO as the first member keeps every existing offset
    // identical (both kernels' std140 layouts agree up to Dispatch0) without
    // duplicating the field-fill logic below into two structs.
    struct TraceSwParamsUBO
    {
        TraceParamsUBO Base;
        uint32_t SwScene[4];  // x=tlasNodeCount, y=instanceBase, z=tlasBase, w=unused
    };
    static_assert(sizeof(TraceSwParamsUBO) == 112,
                  "must match ddgi_trace_sw.comp's DDGIVolumeParams");

    TraceParamsUBO tp{};
    tp.GridMinWS[0] = gridMinWS[0];
    tp.GridMinWS[1] = gridMinWS[1];
    tp.GridMinWS[2] = gridMinWS[2];
    // The previous-frame depth atlas's interior tile resolution, for the
    // recursive bounce's Chebyshev gather (GE_DDGIDepthTexelUV) — rides the
    // .w lane the kernels' grid math ignores, like BounceIntensity below.
    tp.GridMinWS[3] = static_cast<float>(depthOctRes);
    tp.GridSizeWS[0] = gridSizeWS[0];
    tp.GridSizeWS[1] = gridSizeWS[1];
    tp.GridSizeWS[2] = gridSizeWS[2];
    // Bounce strength rides in uGridSizeWS.w — a lane the kernels already
    // ignore (they read .xyz), so the knob costs no UBO growth and cannot
    // disturb the std140 offsets the software kernel's trailing uvec4 depends
    // on. Negative/garbage is not possible: the component's range is clamped.
    tp.GridSizeWS[3] = std::max(m_Volume.BounceIntensity, 0.0f);
    tp.ProbeCount[0] = grid.ProbeCount[0];
    tp.ProbeCount[1] = grid.ProbeCount[1];
    tp.ProbeCount[2] = grid.ProbeCount[2];
    tp.ProbeCount[3] = grid.ProbeTotal;
    tp.Params0[0] = grid.MinCellWS;
    tp.Params0[1] = m_Volume.NormalBiasScale;
    tp.Params0[2] = m_Volume.Hysteresis;
    tp.Params0[3] = m_Volume.Intensity;
    tp.Dispatch0[0] = probeBaseThisTick;
    tp.Dispatch0[1] = static_cast<uint32_t>(grid.AllocatedRaysPerProbe);
    tp.Dispatch0[2] = static_cast<uint32_t>(shared.RayEpoch);  // ray-set rotation (M4)
    tp.Dispatch0[3] = shared.LightCount;
    // The recursive bounce's luminance ceiling and the visibility strength its
    // 8-probe gather weights with. The gather MUST use the same strength the
    // forward consumer uses (UploadCascadeVolumeData feeds it the same field),
    // or the bounce and the shading disagree on where light stops.
    tp.Params1[0] = m_Volume.RadianceClamp;
    tp.Params1[1] = m_Volume.ChebyshevStrength;
    // Sky scale rides the TRACE lane, not the consumer's: the sky term is
    // baked into the ray buffer on a miss, which every downstream consumer of
    // that buffer (diffuse blend and both reflection lobes) then inherits for
    // free. Classify strength must likewise reach trace, because the ray
    // origin applies the same relocation offset the gather does.
    tp.Params1[2] = shared.SkyIntensityScale;
    tp.Params1[3] = m_Volume.ClassifyStrength;

    const Rendering::BufferHandle rayBuf = grid.RayBuffer;
    const Rendering::TextureHandle atlasTex = grid.IrradianceAtlas;
    // The recursive bounce fetch (Includes/ddgi_hit_shade.glsl's shared
    // GE_DDGISampleBounce) weights its 8-probe gather with the same Chebyshev
    // visibility test the forward consumer uses, so both trace lanes need the
    // previous frame's depth-moment atlas alongside its irradiance atlas. Same
    // sampler for both, matching how RenderServicesWorldPass binds the pair.
    const Rendering::TextureHandle depthAtlasTrace = grid.DepthAtlas;
    const Rendering::SamplerHandle atlasSampler = m_AtlasSampler;

    // The ray buffer and both probe atlases are persistent device resources the
    // graph never saw, yet they carry this feature's whole producer/consumer
    // chain within one frame: Trace writes the rays Blend consumes, and Trace
    // SAMPLES the irradiance/depth atlases that Upload storage-writes in the
    // same phase. Undeclared, nothing orders those, and a fast trace can read
    // an atlas mid-upload — which is why the hardware lane never settled while
    // the slower software lane happened to stay ordered.
    const RG::RGTexture irradianceRG = frame.ImportExternalTexture(
        names.IrradianceAtlas, atlasTex, Rendering::ResourceState::UnorderedAccess);
    const RG::RGTexture depthAtlasRG = frame.ImportExternalTexture(
        names.DepthAtlas, depthAtlasTrace, Rendering::ResourceState::UnorderedAccess);
    // Same physical handle the blit imported, so RG dedups to one resource and
    // orders these kernels after DDGI.MapBlit with the sampled-read barrier.
    // Invalid until the atlas has layers; the reads below are declared only then.
    const bool mapAtlasTracked = shared.MapAtlas.IsValid() && m_MapAtlas;
    const RG::RGTexture mapAtlasRG =
        mapAtlasTracked
            ? frame.ImportExternalTexture("DDGI.MaterialMapAtlas", shared.MapAtlas,
                                          Rendering::ResourceState::ShaderResource,
                                          Rendering::TextureFormat::RGBA8_UNORM, 1,
                                          std::max(m_MapAtlas->GetLayerCount(), 1u))
            : RG::RGTexture{};
    // The miss path samples the IBL irradiance cube, which the sky bake writes in
    // the frames after a scene opens. Importing it under the bake's name and
    // handle lands on the same graph resource, so the traces are ordered after
    // the bake's writes (with the semaphore when they run on another queue) and
    // read it in its sampled layout.
    const RG::RGTexture skyIrradianceRG =
        shared.SkyIrradiance.IsValid()
            ? frame.ImportExternalTexture("IBL_Irradiance", shared.SkyIrradiance,
                                          Rendering::ResourceState::ShaderResource,
                                          Rendering::TextureFormat::R16G16B16A16_FLOAT, 1,
                                          ImageBasedLightingFeature::kNumCaptureFaces)
            : RG::RGTexture{};
    const Rendering::BufferHandle probeStateBufTrace = grid.ProbeStateBuffer;
    const uint64_t probeStateBytesTrace = CascadeProbeStateBytes(grid);
    const uint64_t traceRayBytes = static_cast<uint64_t>(grid.ProbeTotal) *
                                   static_cast<uint64_t>(grid.AllocatedRaysPerProbe) * sizeof(float) * 4;
    const RG::RGBuffer rayRG = frame.ImportExternalBuffer(names.RayBuffer, rayBuf, traceRayBytes);

    // Trace's pipeline-variant compile is only provable at EXECUTION time
    // (RGContext::GetOrCreatePipelineVariant, lazy — see m_HardwareLaneFailed's
    // doc). Declaring trace successfully says nothing about whether it will
    // actually run: on the very first tick a backend's translation fails, the
    // sticky flag only gets set from INSIDE trace's own lambda, one dispatch
    // too late to stop blend (declared in this SAME DeclareCascadeGridPasses
    // call) from reading that tick's ray buffer as if trace had written it.
    // Threading this flag from trace's lambda to blend's lets blend skip its
    // OWN dispatch — not just read safe data, skip entirely, preserving
    // whatever irradiance/depth state already existed — rather than trust a
    // same-tick trace that never actually executed.
    auto traceExecuted = std::make_shared<bool>(false);

    if (!shared.UseSoftwareLane)
    {
        auto traceUb = frame.AllocUpload<TraceParamsUBO>();
        if (!traceUb.Valid())
            return;
        *traceUb.Ptr = tp;

        const Rendering::ComputePipelineId tracePipe = m_TraceHw.PipelineId;
        const Rendering::DescriptorSetLayoutDesc traceLayout = m_TraceHw.Set0Layout;
        const Rendering::ShaderMeta* traceMeta = m_TraceHw.Meta.get();
        const Rendering::TlasSlotHandle tlasSlot = m_TlasSlot;
        const RG::RGAccelerationStructure tlasRG = frame.ImportAccelerationStructure("DDGI.TLAS", tlasSlot);

        frame.AddComputePass(
            names.TraceHW, Rendering::PassPhase::kDefault,
            [mapAtlasRG, mapAtlasTracked, skyIrradianceRG, rayRG, irradianceRG, depthAtlasRG,
             probeStateRG, tlasRG](RG::RGPassBuilder& p)
            {
                p.PreventCulling();
                p.Read(tlasRG);
                if (mapAtlasTracked)
                    p.Read(mapAtlasRG, RG::RGTextureRead::Sampled);
                if (skyIrradianceRG.IsValid())
                    p.Read(skyIrradianceRG, RG::RGTextureRead::Sampled);
                p.Write(rayRG, RG::RGBufferWrite::Storage);
                p.Read(probeStateRG, RG::RGBufferRead::Storage);
                p.Read(irradianceRG, RG::RGTextureRead::Sampled);
                p.Read(depthAtlasRG, RG::RGTextureRead::Sampled);
            },
            [this, traceUb, rayBuf, traceRayBytes, atlasTex, depthAtlasTrace, atlasSampler, tracePipe,
             traceLayout, traceMeta,
             probesThisTick, tlasSlot, gpuInstanceBuffer = shared.GpuInstanceBuffer,
             gpuInstanceBytes = shared.GpuInstanceBytes, meshGeomBuffer = shared.MeshGeomBuffer,
             meshGeomBytes = shared.MeshGeomBytes, materialParamsBuffer = shared.MaterialParamsBuffer,
             materialParamsOffset = shared.MaterialParamsOffset, materialParamsSize = shared.MaterialParamsSize,
             skyIrradiance = shared.SkyIrradiance, skySampler = shared.SkySampler,
             lightBuffer = shared.LightBuffer, lightOffset = shared.LightOffset,
             lightBytesCopy = shared.LightBytes, probeStateBufTrace,
             probeStateBytesTrace, mapAtlas = shared.MapAtlas,
             mapSampler = shared.MapSampler, mapTable = shared.MapTable,
             mapTableBytes = shared.MapTableBytes,
             skinnedRowMap = shared.SkinnedRowMapBuffer, skinnedRowMapOffset = shared.SkinnedRowMapOffset,
             skinnedRowMapBytes = shared.SkinnedRowMapBytes, skinnedGeomRows = shared.SkinnedGeomRowsBuffer,
             skinnedGeomRowsOffset = shared.SkinnedGeomRowsOffset,
             skinnedGeomRowsBytes = shared.SkinnedGeomRowsBytes, traceExecuted](RG::RGContext& ctx)
            {
                auto* dev = ctx.GetDevice();
                auto* cl = ctx.Cmd;
                if (!dev || !cl || !traceMeta)
                    return;
                Rendering::DescriptorSetDesc dsDesc{};
                dsDesc.layout = traceLayout;
                dsDesc.transient = true;
                dsDesc.debugName = "DDGI.TraceHW.Set0";
                auto ds = dev->CreateDescriptorSet(dsDesc);
                Rendering::NamedDescriptorWriter wd(dev, ds, *traceMeta, 0);
                wd.AddStorageBuffer("DDGIInstanceBuffer", gpuInstanceBuffer, 0, gpuInstanceBytes);
                wd.AddStorageBuffer("DDGIMaterialBuffer", materialParamsBuffer, materialParamsOffset,
                                    materialParamsSize);
                wd.AddStorageBuffer("DDGILightBuffer", lightBuffer, lightOffset, lightBytesCopy);
                if (skyIrradiance.IsValid() && skySampler.IsValid())
                    wd.AddCombinedImageSampler("ge_ddgiSkyIrradiance", skyIrradiance, skySampler);
                wd.AddStorageBuffer("DDGIMeshGeometryBuffer", meshGeomBuffer, 0, meshGeomBytes);
                wd.AddStorageBuffer("DDGISkinnedRowMap", skinnedRowMap, skinnedRowMapOffset,
                                    skinnedRowMapBytes);
                wd.AddStorageBuffer("DDGISkinnedGeomRows", skinnedGeomRows, skinnedGeomRowsOffset,
                                    skinnedGeomRowsBytes);
                // Instance name, not block name — see DDGIClearParams's fix.
                wd.AddUniformBuffer("DDGIParams", traceUb.Buffer, traceUb.Offset, sizeof(TraceParamsUBO));
                if (atlasSampler.IsValid())
                {
                    wd.AddCombinedImageSampler("uPrevIrradianceAtlas", atlasTex, atlasSampler);
                    wd.AddCombinedImageSampler("uPrevDepthAtlas", depthAtlasTrace, atlasSampler);
                }
                wd.AddStorageBuffer("DDGIRayBuffer", rayBuf, 0, traceRayBytes);
                wd.AddStorageBuffer("DDGIProbeStateRO", probeStateBufTrace, 0, probeStateBytesTrace);
                // Textured base colour and emission. Both are skipped together
                // when the scene has no maps at all: with no layer ever
                // assigned every material record reads -1, so the kernel never
                // samples the atlas and never indexes the table.
                if (mapAtlas.IsValid() && mapSampler.IsValid())
                    wd.AddCombinedImageSampler("ge_ddgiMapAtlas", mapAtlas, mapSampler);
                if (mapTable.IsValid())
                    wd.AddStorageBuffer("DDGIMapTable", mapTable, 0, mapTableBytes);
                if (!wd.TryAddAccelerationStructure("uTlas", tlasSlot))
                {
                    m_HardwareLaneFailed.store(true, std::memory_order_release);
                    Logger::Log::Error(
                        "DDGI: ddgi_trace_hw.comp reflects no 'uTlas' acceleration-structure binding "
                        "— falling back to the software lane");
                    return;
                }
                wd.Flush();
                Rendering::PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(tracePipe);
                if (!pipe.IsValid())
                {
                    // See classify's identical handling above and
                    // m_HardwareLaneFailed's doc — sticky fallback to the
                    // software lane instead of retrying every tick.
                    m_HardwareLaneFailed.store(true, std::memory_order_release);
                    return;
                }
                cl->SetPipeline(pipe);
                cl->BindDescriptorSet(0, ds, pipe);
                cl->Dispatch(probesThisTick, 1, 1);
                *traceExecuted = true;
            });
    }
    else
    {
        auto traceUb = frame.AllocUpload<TraceSwParamsUBO>();
        if (!traceUb.Valid())
            return;
        TraceSwParamsUBO swp{};
        swp.Base = tp;
        swp.SwScene[0] = shared.SwTlasNodeCount;
        swp.SwScene[1] = shared.SwInstanceBase;
        swp.SwScene[2] = shared.SwTlasBase;
        *traceUb.Ptr = swp;

        const Rendering::ComputePipelineId tracePipe = m_TraceSw.PipelineId;
        const Rendering::DescriptorSetLayoutDesc traceLayout = m_TraceSw.Set0Layout;
        const Rendering::ShaderMeta* traceMeta = m_TraceSw.Meta.get();

        frame.AddComputePass(
            names.TraceSW, Rendering::PassPhase::kDefault,
            [mapAtlasRG, mapAtlasTracked, rayRG, irradianceRG, depthAtlasRG, probeStateRG,
             swNodesRG = shared.SwNodesRG, swVertexDataRG = shared.SwVertexDataRG,
             swPackedSceneRG = shared.SwPackedSceneRG, skyIrradianceRG](RG::RGPassBuilder& p)
            {
                p.PreventCulling();
                if (mapAtlasTracked)
                    p.Read(mapAtlasRG, RG::RGTextureRead::Sampled);
                if (skyIrradianceRG.IsValid())
                    p.Read(skyIrradianceRG, RG::RGTextureRead::Sampled);
                p.Write(rayRG, RG::RGBufferWrite::Storage);
                p.Read(probeStateRG, RG::RGBufferRead::Storage);
                p.Read(swPackedSceneRG, RG::RGBufferRead::Storage);
                p.Read(irradianceRG, RG::RGTextureRead::Sampled);
                p.Read(depthAtlasRG, RG::RGTextureRead::Sampled);
                // Order after the skinned BVH refit's writes.
                if (swNodesRG.IsValid())
                    p.Read(swNodesRG, RG::RGBufferRead::Storage);
                if (swVertexDataRG.IsValid())
                    p.Read(swVertexDataRG, RG::RGBufferRead::Storage);
            },
            [traceUb, rayBuf, traceRayBytes, atlasTex, depthAtlasTrace, atlasSampler, tracePipe, traceLayout,
             traceMeta,
             probesThisTick, skyIrradiance = shared.SkyIrradiance, skySampler = shared.SkySampler,
             lightBuffer = shared.LightBuffer, lightOffset = shared.LightOffset,
             lightBytesCopy = shared.LightBytes, probeStateBufTrace, probeStateBytesTrace,
             swPackedScene = shared.SwPackedScene, swPackedSceneBytes = shared.SwPackedSceneBytes,
             swNodes = shared.SwNodes, swNodesBytes = shared.SwNodesBytes,
             swTriangleIndices = shared.SwTriangleIndices,
             swTriangleIndicesBytes = shared.SwTriangleIndicesBytes,
             swTriangleMaterials = shared.SwTriangleMaterials,
             swTriangleMaterialsBytes = shared.SwTriangleMaterialsBytes, swVertexData = shared.SwVertexData,
             swVertexDataBytes = shared.SwVertexDataBytes, mapAtlas = shared.MapAtlas,
             mapSampler = shared.MapSampler, traceExecuted](RG::RGContext& ctx)
            {
                auto* dev = ctx.GetDevice();
                auto* cl = ctx.Cmd;
                if (!dev || !cl || !traceMeta)
                    return;
                Rendering::DescriptorSetDesc dsDesc{};
                dsDesc.layout = traceLayout;
                dsDesc.transient = true;
                dsDesc.debugName = "DDGI.TraceSW.Set0";
                auto ds = dev->CreateDescriptorSet(dsDesc);
                Rendering::NamedDescriptorWriter wd(dev, ds, *traceMeta, 0);
                wd.AddStorageBuffer("DDGILightBuffer", lightBuffer, lightOffset, lightBytesCopy);
                if (skyIrradiance.IsValid() && skySampler.IsValid())
                    wd.AddCombinedImageSampler("ge_ddgiSkyIrradiance", skyIrradiance, skySampler);
                // Instance name, not block name — see DDGIClearParams's fix.
                wd.AddUniformBuffer("DDGIParams", traceUb.Buffer, traceUb.Offset, sizeof(TraceSwParamsUBO));
                if (atlasSampler.IsValid())
                {
                    wd.AddCombinedImageSampler("uPrevIrradianceAtlas", atlasTex, atlasSampler);
                    wd.AddCombinedImageSampler("uPrevDepthAtlas", depthAtlasTrace, atlasSampler);
                }
                wd.AddStorageBuffer("DDGIRayBuffer", rayBuf, 0, traceRayBytes);
                wd.AddStorageBuffer("DDGIProbeStateRO", probeStateBufTrace, 0, probeStateBytesTrace);
                wd.AddStorageBuffer("DDGISwPackedScene", swPackedScene, 0, swPackedSceneBytes);
                wd.AddStorageBuffer("DDGISwNodes", swNodes, 0, swNodesBytes);
                wd.AddStorageBuffer("DDGISwTriangleIndices", swTriangleIndices, 0, swTriangleIndicesBytes);
                wd.AddStorageBuffer("DDGISwTriangleMaterials", swTriangleMaterials, 0,
                                    swTriangleMaterialsBytes);
                wd.AddStorageBuffer("DDGISwVertexData", swVertexData, 0, swVertexDataBytes);
                // Textured base colour and emission — the layer indices and uv
                // transform already ride in the packed uber-material records,
                // so this lane binds only the atlas itself.
                if (mapAtlas.IsValid() && mapSampler.IsValid())
                    wd.AddCombinedImageSampler("ge_ddgiMapAtlas", mapAtlas, mapSampler);
                wd.Flush();
                Rendering::PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(tracePipe);
                if (!pipe.IsValid())
                    return;
                cl->SetPipeline(pipe);
                cl->BindDescriptorSet(0, ds, pipe);
                cl->Dispatch(probesThisTick, 1, 1);
                *traceExecuted = true;
            });
    }

    // Blend: cosine-gather rayBuffer -> irradianceState.
    struct BlendParamsUBO
    {
        float GridMinWS[4];
        float GridSizeWS[4];
        int32_t ProbeCount[4];
        float Params0[4];
        uint32_t Dispatch0[4];
        float Timing[4];
        float Params1[4];  // x=depthSharpness, yzw=reserved
    };
    static_assert(sizeof(BlendParamsUBO) == 112, "must match ddgi_blend.comp's DDGIBlendParams");
    auto blendUb = frame.AllocUpload<BlendParamsUBO>();
    if (!blendUb.Valid())
        return;
    BlendParamsUBO bp{};
    bp.GridMinWS[0] = tp.GridMinWS[0];
    bp.GridMinWS[1] = tp.GridMinWS[1];
    bp.GridMinWS[2] = tp.GridMinWS[2];
    bp.GridSizeWS[0] = tp.GridSizeWS[0];
    bp.GridSizeWS[1] = tp.GridSizeWS[1];
    bp.GridSizeWS[2] = tp.GridSizeWS[2];
    bp.ProbeCount[0] = tp.ProbeCount[0];
    bp.ProbeCount[1] = tp.ProbeCount[1];
    bp.ProbeCount[2] = tp.ProbeCount[2];
    bp.ProbeCount[3] = tp.ProbeCount[3];
    bp.Params0[0] = tp.Params0[0];
    bp.Params0[1] = tp.Params0[1];
    bp.Params0[2] = tp.Params0[2];
    bp.Params0[3] = tp.Params0[3];
    bp.Dispatch0[0] = probeBaseThisTick;
    bp.Dispatch0[1] = tp.Dispatch0[1];
    bp.Dispatch0[2] = probesThisTick;
    bp.Dispatch0[3] = tp.Dispatch0[2];  // frameIndex: MUST match trace's ray-set rotation exactly
    bp.Timing[0] = shared.TickTimeMs;
    // The variance-aware temporal policy's three authored knobs — see
    // ddgi_blend.comp, which documents what each does and why a scalar
    // hysteresis alone cannot both absorb Monte-Carlo noise and react to a
    // real lighting change.
    bp.Timing[1] = m_Volume.FireflyClamp;
    bp.Timing[2] = m_Volume.ChangeThreshold;
    bp.Timing[3] = m_Volume.SnapAmount;
    // Depth-moment cosine power. Irradiance keeps the plain cosine weight, so
    // this shapes only the distance accumulation the Chebyshev test consumes.
    bp.Params1[0] = m_Volume.DepthSharpness;
    // Tells the fused kernel whether the depth moments are its job this tick
    // (Shared) or ddgi_depth_blend.comp's (Fine).
    bp.Params1[1] = static_cast<float>(depthOctRes);
    *blendUb.Ptr = bp;

    const Rendering::BufferHandle stateBuf2 = grid.IrradianceState;
    const uint64_t stateBytes2 =
        static_cast<uint64_t>(grid.ProbeTotal) * uint64_t(kTile * kTile) * sizeof(float) * 4;
    const Rendering::BufferHandle depthStateBuf = grid.DepthState;
    const Rendering::BufferHandle temporalStateBuf = grid.TemporalState;
    const uint64_t rayBytes = static_cast<uint64_t>(grid.ProbeTotal) *
                              static_cast<uint64_t>(grid.AllocatedRaysPerProbe) * sizeof(float) * 4;
    // Fine depth: the moments' own blend, over the same window and the same
    // ray buffer, at the finer tile. Same UBO as Blend (identical block).
    // Declared AHEAD of Blend: it reads the cell record Blend re-stamps.
    if (fineDepth)
    {
        const Rendering::ComputePipelineId depthBlendPipe = m_DepthBlend.PipelineId;
        const Rendering::DescriptorSetLayoutDesc depthBlendLayout = m_DepthBlend.Set0Layout;
        const Rendering::ShaderMeta* depthBlendMeta = m_DepthBlend.Meta.get();
        frame.AddComputePass(
            names.DepthBlend, Rendering::PassPhase::kDefault,
            [rayRG, depthStateRG, probeCellRG](RG::RGPassBuilder& p)
            {
                p.PreventCulling();
                p.Read(rayRG, RG::RGBufferRead::Storage);
                p.Read(probeCellRG, RG::RGBufferRead::Storage);
                p.Write(depthStateRG, RG::RGBufferWrite::Storage);
            },
            [blendUb, rayBuf, rayBytes, depthStateBuf, depthStateBytes, probeCellBuf, probeCellBytes,
             depthBlendPipe, depthBlendLayout, depthBlendMeta, probesThisTick,
             traceExecuted](RG::RGContext& ctx)
            {
                auto* dev = ctx.GetDevice();
                auto* cl = ctx.Cmd;
                if (!dev || !cl || !depthBlendMeta || !*traceExecuted)
                    return;
                Rendering::DescriptorSetDesc dsDesc{};
                dsDesc.layout = depthBlendLayout;
                dsDesc.transient = true;
                dsDesc.debugName = "DDGI.DepthBlend.Set0";
                auto ds = dev->CreateDescriptorSet(dsDesc);
                Rendering::NamedDescriptorWriter wd(dev, ds, *depthBlendMeta, 0);
                wd.AddUniformBuffer("DDGIParams", blendUb.Buffer, blendUb.Offset, sizeof(BlendParamsUBO));
                wd.AddStorageBuffer("DDGIRayBufferRO", rayBuf, 0, rayBytes);
                wd.AddStorageBuffer("DDGIDepthState", depthStateBuf, 0, depthStateBytes);
                wd.AddStorageBuffer("DDGIProbeCellRO", probeCellBuf, 0, probeCellBytes);
                wd.Flush();
                Rendering::PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(depthBlendPipe);
                if (!pipe.IsValid())
                    return;
                cl->SetPipeline(pipe);
                cl->BindDescriptorSet(0, ds, pipe);
                cl->Dispatch(probesThisTick, 1, 1);
            });
    }

    const Rendering::ComputePipelineId blendPipe = m_Blend.PipelineId;
    const Rendering::DescriptorSetLayoutDesc blendLayout = m_Blend.Set0Layout;
    const Rendering::ShaderMeta* blendMeta = m_Blend.Meta.get();
    frame.AddComputePass(
        names.Blend, Rendering::PassPhase::kDefault,
        [rayRG, temporalStateRG, probeCellRG, irradianceStateRG, depthStateRG, fineDepth](RG::RGPassBuilder& p)
        {
            p.PreventCulling();
            p.Read(rayRG, RG::RGBufferRead::Storage);
            p.Write(irradianceStateRG, RG::RGBufferWrite::Storage);
            if (!fineDepth)
                p.Write(depthStateRG, RG::RGBufferWrite::Storage);
            p.Write(temporalStateRG, RG::RGBufferWrite::Storage);
            p.Write(probeCellRG, RG::RGBufferWrite::Storage);
        },
        [blendUb, rayBuf, rayBytes, stateBuf2, stateBytes2, depthStateBuf, depthStateBytes, temporalStateBuf,
         temporalStateBytes, probeCellBuf, probeCellBytes, blendPipe, blendLayout, blendMeta, probesThisTick,
         traceExecuted](RG::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl || !blendMeta)
                return;
            // See traceExecuted's doc above DeclareCascadeGridPasses's trace
            // dispatch: trace's own pipeline-variant compile is only provable
            // at its OWN execution time, which runs before this lambda in
            // command-recording order (same PassPhase::kDefault, declared
            // first) — so this flag is reliably set by the time Blend reads
            // it. A trace that never actually dispatched means the ray
            // buffer wasn't refreshed this tick; blending it now would bake
            // stale-or-placeholder data into the atlas. Skip entirely rather
            // than run against data trace never wrote.
            if (!*traceExecuted)
                return;
            Rendering::DescriptorSetDesc dsDesc{};
            dsDesc.layout = blendLayout;
            dsDesc.transient = true;
            dsDesc.debugName = "DDGI.Blend.Set0";
            auto ds = dev->CreateDescriptorSet(dsDesc);
            Rendering::NamedDescriptorWriter wd(dev, ds, *blendMeta, 0);
            // Instance name, not block name — see DDGIClearParams's fix.
            wd.AddUniformBuffer("DDGIParams", blendUb.Buffer, blendUb.Offset, sizeof(BlendParamsUBO));
            wd.AddStorageBuffer("DDGIRayBufferRO", rayBuf, 0, rayBytes);
            wd.AddStorageBuffer("DDGIIrradianceState", stateBuf2, 0, stateBytes2);
            wd.AddStorageBuffer("DDGIDepthState", depthStateBuf, 0, depthStateBytes);
            wd.AddStorageBuffer("DDGITemporalState", temporalStateBuf, 0, temporalStateBytes);
            wd.AddStorageBuffer("DDGIProbeCell", probeCellBuf, 0, probeCellBytes);
            wd.Flush();
            Rendering::PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(blendPipe);
            if (!pipe.IsValid())
                return;
            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            cl->Dispatch(probesThisTick, 1, 1);
        });

    // Each updated workgroup writes the entire tile, including its gutter.
    const uint32_t uploadBase = fullUpload ? 0u : probeBaseThisTick;
    const uint32_t uploadCount = fullUpload ? static_cast<uint32_t>(grid.ProbeTotal) : probesThisTick;
    struct UploadParamsUBO
    {
        float GridMinWS[4];
        float GridSizeWS[4];
        int32_t ProbeCount[4];
        uint32_t Dispatch0[4];
        float Params0[4];
    };
    static_assert(sizeof(UploadParamsUBO) == 80, "must match ddgi_upload.comp's DDGIUploadParams");
    auto uploadUb = frame.AllocUpload<UploadParamsUBO>();
    if (!uploadUb.Valid())
        return;
    UploadParamsUBO up{};
    // Placement for the cell record compare (a scrolled, unvisited slot
    // uploads as black — see ddgi_upload.comp).
    std::memcpy(up.GridMinWS, tp.GridMinWS, sizeof(up.GridMinWS));
    std::memcpy(up.GridSizeWS, tp.GridSizeWS, sizeof(up.GridSizeWS));
    up.ProbeCount[0] = tp.ProbeCount[0];
    up.ProbeCount[1] = tp.ProbeCount[1];
    up.ProbeCount[2] = tp.ProbeCount[2];
    up.ProbeCount[3] = tp.ProbeCount[3];
    up.Dispatch0[0] = uploadBase;
    up.Dispatch0[1] = uploadCount;
    // Volume-wide rather than per-cascade: both cascades run this one kernel,
    // and the intra-tile denoise is copy-time (ddgi_upload.comp's header), so
    // it never touches either cascade's state.
    up.Params0[0] = m_Volume.FilterStrength;
    up.Params0[1] = m_Volume.FilterSmoothness;
    // The fused kernel copies the depth moments only while they share its
    // tile; a Fine atlas is ddgi_depth_upload.comp's below.
    up.Params0[2] = static_cast<float>(depthOctRes);
    up.Params0[3] = 0.0f;
    *uploadUb.Ptr = up;

    const Rendering::TextureHandle depthAtlasTex = grid.DepthAtlas;
    const Rendering::ComputePipelineId uploadPipe = m_Upload.PipelineId;
    const Rendering::DescriptorSetLayoutDesc uploadLayout = m_Upload.Set0Layout;
    const Rendering::ShaderMeta* uploadMeta = m_Upload.Meta.get();
    const uint32_t probeTotalForUpload = uploadCount;
    frame.AddComputePass(
        names.Upload, Rendering::PassPhase::kDefault,
        [irradianceRG, depthAtlasRG, probeCellRG, irradianceStateRG, depthStateRG, fineDepth](RG::RGPassBuilder& p)
        {
            p.PreventCulling();
            p.Read(probeCellRG, RG::RGBufferRead::Storage);
            p.Read(irradianceStateRG, RG::RGBufferRead::Storage);
            if (!fineDepth)
                p.Read(depthStateRG, RG::RGBufferRead::Storage);
            p.Write(irradianceRG, RG::RGTextureWrite::Storage);
            p.Write(depthAtlasRG, RG::RGTextureWrite::Storage);
        },
        [uploadUb, stateBuf2, stateBytes2, atlasTex, depthStateBuf, depthStateBytes, depthAtlasTex, probeCellBuf,
         probeCellBytes, uploadPipe, uploadLayout, uploadMeta, probeTotalForUpload](RG::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl || !uploadMeta)
                return;
            Rendering::DescriptorSetDesc dsDesc{};
            dsDesc.layout = uploadLayout;
            dsDesc.transient = true;
            dsDesc.debugName = "DDGI.Upload.Set0";
            auto ds = dev->CreateDescriptorSet(dsDesc);
            Rendering::NamedDescriptorWriter wd(dev, ds, *uploadMeta, 0);
            // Instance name, not block name — see DDGIClearParams's fix.
            wd.AddUniformBuffer("DDGIParams", uploadUb.Buffer, uploadUb.Offset, sizeof(UploadParamsUBO));
            wd.AddStorageBuffer("DDGIIrradianceStateRO", stateBuf2, 0, stateBytes2);
            wd.AddStorageBuffer("DDGIDepthStateRO", depthStateBuf, 0, depthStateBytes);
            wd.AddStorageBuffer("DDGIProbeCellRO", probeCellBuf, 0, probeCellBytes);
            wd.Flush();
            Pipeline::Nodes::Detail::BindStorageImageByName(dev, ds, *uploadMeta, "uIrradianceAtlas",
                                                             atlasTex);
            Pipeline::Nodes::Detail::BindStorageImageByName(dev, ds, *uploadMeta, "uDepthAtlas",
                                                             depthAtlasTex);
            Rendering::PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(uploadPipe);
            if (!pipe.IsValid())
                return;
            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            cl->Dispatch(static_cast<uint32_t>(probeTotalForUpload), 1, 1);
        });

    if (fineDepth)
    {
        struct DepthUploadParamsUBO
        {
            float GridMinWS[4];
            float GridSizeWS[4];
            int32_t ProbeCount[4];
            uint32_t Dispatch0[4];
        };
        static_assert(sizeof(DepthUploadParamsUBO) == 64,
                      "must match ddgi_depth_upload.comp's DDGIDepthUploadParams");
        auto depthUploadUb = frame.AllocUpload<DepthUploadParamsUBO>();
        if (!depthUploadUb.Valid())
            return;
        DepthUploadParamsUBO dup{};
        std::memcpy(dup.GridMinWS, tp.GridMinWS, sizeof(dup.GridMinWS));
        std::memcpy(dup.GridSizeWS, tp.GridSizeWS, sizeof(dup.GridSizeWS));
        dup.ProbeCount[0] = tp.ProbeCount[0];
        dup.ProbeCount[1] = tp.ProbeCount[1];
        dup.ProbeCount[2] = tp.ProbeCount[2];
        dup.ProbeCount[3] = tp.ProbeCount[3];
        dup.Dispatch0[0] = uploadBase;
        dup.Dispatch0[1] = uploadCount;
        *depthUploadUb.Ptr = dup;

        const Rendering::ComputePipelineId depthUploadPipe = m_DepthUpload.PipelineId;
        const Rendering::DescriptorSetLayoutDesc depthUploadLayout = m_DepthUpload.Set0Layout;
        const Rendering::ShaderMeta* depthUploadMeta = m_DepthUpload.Meta.get();
        frame.AddComputePass(
            names.DepthUpload, Rendering::PassPhase::kDefault,
            [depthStateRG, depthAtlasRG, probeCellRG](RG::RGPassBuilder& p)
            {
                p.PreventCulling();
                p.Read(depthStateRG, RG::RGBufferRead::Storage);
                p.Read(probeCellRG, RG::RGBufferRead::Storage);
                p.Write(depthAtlasRG, RG::RGTextureWrite::Storage);
            },
            [depthUploadUb, depthStateBuf, depthStateBytes, depthAtlasTex, probeCellBuf, probeCellBytes,
             depthUploadPipe, depthUploadLayout, depthUploadMeta, probeTotalForUpload](RG::RGContext& ctx)
            {
                auto* dev = ctx.GetDevice();
                auto* cl = ctx.Cmd;
                if (!dev || !cl || !depthUploadMeta)
                    return;
                Rendering::DescriptorSetDesc dsDesc{};
                dsDesc.layout = depthUploadLayout;
                dsDesc.transient = true;
                dsDesc.debugName = "DDGI.DepthUpload.Set0";
                auto ds = dev->CreateDescriptorSet(dsDesc);
                Rendering::NamedDescriptorWriter wd(dev, ds, *depthUploadMeta, 0);
                wd.AddUniformBuffer("DDGIParams", depthUploadUb.Buffer, depthUploadUb.Offset,
                                    sizeof(DepthUploadParamsUBO));
                wd.AddStorageBuffer("DDGIDepthStateRO", depthStateBuf, 0, depthStateBytes);
                wd.AddStorageBuffer("DDGIProbeCellRO", probeCellBuf, 0, probeCellBytes);
                wd.Flush();
                Pipeline::Nodes::Detail::BindStorageImageByName(dev, ds, *depthUploadMeta, "uDepthAtlas",
                                                                 depthAtlasTex);
                Rendering::PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(depthUploadPipe);
                if (!pipe.IsValid())
                    return;
                cl->SetPipeline(pipe);
                cl->BindDescriptorSet(0, ds, pipe);
                cl->Dispatch(static_cast<uint32_t>(probeTotalForUpload), 1, 1);
            });
    }
    // Upload has now been DECLARED for this grid — matches this file's
    // existing convention of trusting declaration over GPU-side confirmation
    // (grid.NeedsClear = false follows the same pattern above). See
    // AtlasEverUploaded's doc: this is what HasConvergedVolume() actually
    // needs, not just GpuGridValid.
    grid.AtlasEverUploaded = true;
    grid.AtlasUploadState.Commit(gridMinWS, gridSizeWS, m_Volume.FilterStrength, m_Volume.FilterSmoothness);
}

bool DDGIProbeFeature::EnsureReflectionResources(float /*deltaTimeSeconds*/)
{
    // Each cascade's idle gate already debounces how often its size changes,
    // so the lobes just follow. No separate reflection idle timer.
    if (!m_Volume.EnableGlossy || !m_C0.GpuGridValid)
    {
        ReleaseReflectionResources();
        return false;
    }

    const DDGIGlossyAtlasLayout c0Layout = ComputeDDGIGlossyAtlasLayout(m_C0.ProbeTotal);
    const uint64_t c0GlossyBytes = GlossyHistoryBytes(m_C0.ProbeTotal);
    const bool c0GlossyOk = c0GlossyBytes <= kMaxGlossyHistoryBytes &&
                            c0Layout.WidthTexels <= kMaxAtlasDimension &&
                            c0Layout.HeightTexels <= kMaxAtlasDimension;
    if (!c0GlossyOk)
    {
        Logger::Log::Warning(
            "DDGIProbeFeature: glossy reflection lobe disabled at {} probes — history would need {} MB "
            "({}x{} atlas). Reduce ProbesLongAxis or turn EnableGlossy off.",
            m_C0.ProbeTotal, c0GlossyBytes / (1024ull * 1024ull), c0Layout.WidthTexels,
            c0Layout.HeightTexels);
        ReleaseReflectionResources();
        return false;
    }

    AllocateReflectionCascade(m_C0, m_C0Refl, "DDGI.C0", true);
    if (!m_C0Refl.GpuValid)
    {
        ReleaseReflectionResources();
        return false;
    }

    if (!m_Volume.EnableFineCascade || !m_C1.GpuGridValid)
    {
        ReleaseReflectionCascade(m_C1Refl);
        return true;
    }

    const DDGIGlossyAtlasLayout c1Layout = ComputeDDGIGlossyAtlasLayout(m_C1.ProbeTotal);
    const uint64_t c1GlossyBytes = GlossyHistoryBytes(m_C1.ProbeTotal);
    const bool c1GlossyOk = (c0GlossyBytes + c1GlossyBytes) <= kMaxGlossyHistoryBytes &&
                            c1Layout.WidthTexels <= kMaxAtlasDimension &&
                            c1Layout.HeightTexels <= kMaxAtlasDimension;
    if (!c1GlossyOk)
    {
        Logger::Log::Warning(
            "DDGIProbeFeature: C1 sharp reflection lobe disabled at {} probes — combined history would "
            "need {} MB. C1 rough lobe still runs.",
            m_C1.ProbeTotal, (c0GlossyBytes + c1GlossyBytes) / (1024ull * 1024ull));
    }
    AllocateReflectionCascade(m_C1, m_C1Refl, "DDGI.C1", c1GlossyOk);
    return true;
}

void DDGIProbeFeature::ReleaseReflectionResources()
{
    ReleaseReflectionCascade(m_C0Refl);
    ReleaseReflectionCascade(m_C1Refl);
}

void DDGIProbeFeature::ReleaseReflectionCascade(ReflectionCascade& refl)
{
    if (m_Device)
    {
        if (refl.RoughState.IsValid())
            m_Device->DestroyBuffer(refl.RoughState);
        if (refl.GlossyNumerator.IsValid())
            m_Device->DestroyBuffer(refl.GlossyNumerator);
        if (refl.GlossyWeight.IsValid())
            m_Device->DestroyBuffer(refl.GlossyWeight);
        if (refl.RoughAtlas.IsValid())
            m_Device->DestroyTexture(refl.RoughAtlas);
        if (refl.GlossyAtlas.IsValid())
            m_Device->DestroyTexture(refl.GlossyAtlas);
    }
    refl = {};
}

bool DDGIProbeFeature::AllocateReflectionCascade(const CascadeGrid& grid, ReflectionCascade& refl,
                                                 const char* debugPrefix, bool allocateGlossy)
{
    const bool sizeMatches = refl.AllocatedProbeTotal == grid.ProbeTotal &&
                             refl.AllocatedAtlasWidth == grid.AtlasWidth &&
                             refl.AllocatedAtlasHeight == grid.AtlasHeight &&
                             refl.GlossyAllocated == allocateGlossy;
    if (refl.GpuValid && sizeMatches)
        return false;

    ReleaseReflectionCascade(refl);

    const std::string roughStateName = std::string(debugPrefix) + ".RoughState";
    const std::string roughAtlasName = std::string(debugPrefix) + ".RoughAtlas";
    const std::string glossyNumName = std::string(debugPrefix) + ".GlossyNumerator";
    const std::string glossyWeightName = std::string(debugPrefix) + ".GlossyWeight";
    const std::string glossyAtlasName = std::string(debugPrefix) + ".GlossyAtlas";

    Rendering::BufferDesc stateDesc{};
    stateDesc.size =
        static_cast<uint64_t>(grid.ProbeTotal) * static_cast<uint64_t>(kTile * kTile) * sizeof(float) * 4;
    stateDesc.usage = static_cast<uint32_t>(Rendering::BufferUsage::Storage);
    stateDesc.memoryUsage = Rendering::BufferMemoryUsage::DeviceLocal;
    stateDesc.debugName = roughStateName.c_str();
    refl.RoughState = m_Device->CreateBuffer(stateDesc);

    Rendering::TextureDesc atlasDesc{};
    atlasDesc.depth = 1;
    atlasDesc.mipLevels = 1;
    atlasDesc.arrayLayers = 1;
    atlasDesc.sampleCount = 1;
    atlasDesc.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
    atlasDesc.usage = static_cast<uint32_t>(Rendering::TextureUsage::UnorderedAccess |
                                            Rendering::TextureUsage::ShaderResource);
    atlasDesc.width = static_cast<uint32_t>(grid.AtlasWidth);
    atlasDesc.height = static_cast<uint32_t>(grid.AtlasHeight);
    // Storage-written by the reflection blend/upload and SAMPLED by the forward
    // consumer, so its descriptors claim GENERAL. Every render-graph import
    // states it is already GENERAL, and a consumer can bind it before the
    // first upload writes it, so without initialState the image stays
    // UNDEFINED from creation and the first draw that binds it trips
    // VUID-vkCmdDraw-None-09600 ("expects GENERAL -- current layout is
    // UNDEFINED"), observed live under MoltenVK + validation. Same invariant
    // ShadowMinMaxPyramid.cpp states for the same shape.
    atlasDesc.sampledInGeneralLayout = true;
    atlasDesc.initialState = Rendering::ResourceState::UnorderedAccess;
    atlasDesc.debugName = roughAtlasName.c_str();
    refl.RoughAtlas = m_Device->CreateTexture(atlasDesc);

    bool glossyOk = true;
    if (allocateGlossy)
    {
        const DDGIGlossyAtlasLayout glossyLayout = ComputeDDGIGlossyAtlasLayout(grid.ProbeTotal);
        const uint64_t glossyTexelsPerProbe =
            static_cast<uint64_t>(kDDGIGlossyTile) * static_cast<uint64_t>(kDDGIGlossyTile);
        const uint64_t glossyNumeratorBytes =
            static_cast<uint64_t>(grid.ProbeTotal) * glossyTexelsPerProbe * sizeof(float) * 4;
        const uint64_t glossyWeightBytes =
            static_cast<uint64_t>(grid.ProbeTotal) * glossyTexelsPerProbe * sizeof(float);

        Rendering::BufferDesc numeratorDesc{};
        numeratorDesc.size = glossyNumeratorBytes;
        numeratorDesc.usage = static_cast<uint32_t>(Rendering::BufferUsage::Storage);
        numeratorDesc.memoryUsage = Rendering::BufferMemoryUsage::DeviceLocal;
        numeratorDesc.debugName = glossyNumName.c_str();
        refl.GlossyNumerator = m_Device->CreateBuffer(numeratorDesc);

        Rendering::BufferDesc weightDesc{};
        weightDesc.size = glossyWeightBytes;
        weightDesc.usage = static_cast<uint32_t>(Rendering::BufferUsage::Storage);
        weightDesc.memoryUsage = Rendering::BufferMemoryUsage::DeviceLocal;
        weightDesc.debugName = glossyWeightName.c_str();
        refl.GlossyWeight = m_Device->CreateBuffer(weightDesc);

        atlasDesc.width = static_cast<uint32_t>(glossyLayout.WidthTexels);
        atlasDesc.height = static_cast<uint32_t>(glossyLayout.HeightTexels);
        atlasDesc.debugName = glossyAtlasName.c_str();
        refl.GlossyAtlas = m_Device->CreateTexture(atlasDesc);
        refl.GlossyLayout = glossyLayout;
        glossyOk = refl.GlossyNumerator.IsValid() && refl.GlossyWeight.IsValid() &&
                   refl.GlossyAtlas.IsValid();
    }

    refl.AllocatedProbeTotal = grid.ProbeTotal;
    refl.AllocatedAtlasWidth = grid.AtlasWidth;
    refl.AllocatedAtlasHeight = grid.AtlasHeight;
    refl.NeedsClear = true;
    refl.RoughAtlasUploaded = false;
    refl.GlossyAtlasUploaded = false;
    refl.GlossyAllocated = allocateGlossy && glossyOk;
    refl.GpuValid = refl.RoughState.IsValid() && refl.RoughAtlas.IsValid();
    if (!refl.GpuValid)
    {
        Logger::Log::Warning("DDGIProbeFeature: reflection GPU resource allocation failed at {} probes ({})",
                             grid.ProbeTotal, debugPrefix);
        ReleaseReflectionCascade(refl);
        return false;
    }
    if (allocateGlossy && !refl.GlossyAllocated)
    {
        Logger::Log::Warning(
            "DDGIProbeFeature: sharp reflection lobe allocation failed at {} probes ({}); rough lobe still runs",
            grid.ProbeTotal, debugPrefix);
        if (m_Device)
        {
            if (refl.GlossyNumerator.IsValid())
                m_Device->DestroyBuffer(refl.GlossyNumerator);
            if (refl.GlossyWeight.IsValid())
                m_Device->DestroyBuffer(refl.GlossyWeight);
            if (refl.GlossyAtlas.IsValid())
                m_Device->DestroyTexture(refl.GlossyAtlas);
        }
        refl.GlossyNumerator = {};
        refl.GlossyWeight = {};
        refl.GlossyAtlas = {};
        refl.GlossyLayout = {};
    }
    return true;
}

void DDGIProbeFeature::DeclareReflectionPasses(Rendering::RenderGraph::RGFrame& frame, CascadeGrid& grid,
                                               ReflectionCascade& refl, const float gridMinWS[3],
                                               const float gridSizeWS[3], uint32_t probeBaseThisTick,
                                               uint32_t probesThisTick, const SharedTickInputs& shared,
                                               const CascadeDebugNames& names)
{
    namespace RG = Rendering::RenderGraph;
    if (!refl.GpuValid)
        return;

    // Clear freshly (re)allocated reflection state before the first blend
    // reads it as "previous" — uninitialized GPU memory blended with
    // Hysteresis close to 1 would take many ticks to converge out (the same
    // hazard DeclareCascadeGridPasses's NeedsClear guards against for
    // irradiance/depth state). Runs in PassPhase::kEarlySetup, before this
    // same call's kDefault blend passes below.
    //
    // The glossy WEIGHT buffer is float-strided while the clear kernel is
    // vec4-strided; 324 texels per probe is exactly 81 vec4s, so it clears
    // cleanly as a quarter-length vec4 buffer.
    const uint32_t glossyTexelsPerProbe = static_cast<uint32_t>(kDDGIGlossyTile * kDDGIGlossyTile);
    if (refl.NeedsClear)
    {
        const float zeroFill[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        const uint32_t roughElementCount = static_cast<uint32_t>(grid.ProbeTotal) * uint32_t(kTile * kTile);
        DispatchClear(frame, refl.RoughState, roughElementCount, zeroFill, names.ClearRough);

        if (refl.GlossyAllocated)
        {
            const uint32_t glossyElementCount =
                static_cast<uint32_t>(grid.ProbeTotal) * glossyTexelsPerProbe;
            DispatchClear(frame, refl.GlossyNumerator, glossyElementCount, zeroFill, names.ClearGlossyNum);
            DispatchClear(frame, refl.GlossyWeight, glossyElementCount / 4u, zeroFill,
                          names.ClearGlossyWeight);
        }
        refl.NeedsClear = false;
    }

    // Both blends reuse THIS cascade's JUST-TRACED ray buffer for THIS SAME
    // tick's window — no separate reflection trace pass (ddgi_rough_blend.comp's
    // doc). Both trace lanes fill that buffer, so reflections work on the
    // software lane exactly as on the hardware one.
    const Rendering::BufferHandle rayBuf = grid.RayBuffer;
    const uint64_t rayBytes = static_cast<uint64_t>(grid.ProbeTotal) *
                              static_cast<uint64_t>(grid.AllocatedRaysPerProbe) * sizeof(float) * 4;
    // Same physical handle DeclareCascadeGridPasses imported for the trace, so
    // dedup-by-handle resolves to ONE resource and these blends order after the
    // trace that produced the rays they read.
    const RG::RGBuffer rayRG = frame.ImportExternalBuffer(names.RayBuffer, rayBuf, rayBytes);
    // Same physical handle DeclareCascadeGridPasses imported and Blend wrote,
    // so both lobes order after this tick's stamp and read its changed flag.
    const RG::RGBuffer probeCellRG =
        frame.ImportExternalBuffer(names.ProbeCell, grid.ProbeCellBuffer, CascadeProbeStateBytes(grid));
    const Rendering::BufferHandle probeCellBuf = grid.ProbeCellBuffer;
    const uint64_t probeCellBytes = CascadeProbeStateBytes(grid);
    const int32_t probeTotal = grid.ProbeTotal;

    struct RoughBlendParamsUBO
    {
        float GridMinWS[4];
        float GridSizeWS[4];
        int32_t ProbeCount[4];
        float Params0[4];
        uint32_t Dispatch0[4];
        float Timing[4];
    };
    auto roughUb = frame.AllocUpload<RoughBlendParamsUBO>();
    if (!roughUb.Valid())
        return;
    RoughBlendParamsUBO rp{};
    rp.GridMinWS[0] = gridMinWS[0];
    rp.GridMinWS[1] = gridMinWS[1];
    rp.GridMinWS[2] = gridMinWS[2];
    rp.GridSizeWS[0] = gridSizeWS[0];
    rp.GridSizeWS[1] = gridSizeWS[1];
    rp.GridSizeWS[2] = gridSizeWS[2];
    rp.ProbeCount[0] = grid.ProbeCount[0];
    rp.ProbeCount[1] = grid.ProbeCount[1];
    rp.ProbeCount[2] = grid.ProbeCount[2];
    rp.ProbeCount[3] = probeTotal;
    rp.Params0[0] = grid.MinCellWS;
    rp.Params0[1] = m_Volume.NormalBiasScale;
    rp.Params0[2] = m_Volume.Hysteresis;
    rp.Dispatch0[0] = probeBaseThisTick;
    rp.Dispatch0[1] = static_cast<uint32_t>(grid.AllocatedRaysPerProbe);
    rp.Dispatch0[2] = probesThisTick;
    rp.Dispatch0[3] = static_cast<uint32_t>(shared.RayEpoch);  // MUST match trace's ray-set rotation
    rp.Timing[0] = shared.TickTimeMs;
    *roughUb.Ptr = rp;

    const Rendering::BufferHandle roughStateBuf = refl.RoughState;
    const uint64_t roughStateBytes =
        static_cast<uint64_t>(probeTotal) * uint64_t(kTile * kTile) * sizeof(float) * 4;
    const RG::RGBuffer roughStateRG = frame.ImportExternalBuffer(
        names.RoughState, roughStateBuf, roughStateBytes);
    const Rendering::ComputePipelineId roughBlendPipe = m_RoughBlend.PipelineId;
    const Rendering::DescriptorSetLayoutDesc roughBlendLayout = m_RoughBlend.Set0Layout;
    const Rendering::ShaderMeta* roughBlendMeta = m_RoughBlend.Meta.get();
    frame.AddComputePass(
        names.RoughBlend, Rendering::PassPhase::kDefault,
        [rayRG, probeCellRG, roughStateRG](RG::RGPassBuilder& p)
        {
            p.PreventCulling();
            p.Read(rayRG, RG::RGBufferRead::Storage);
            p.Write(roughStateRG, RG::RGBufferWrite::Storage);
            p.Read(probeCellRG, RG::RGBufferRead::Storage);
        },
        [roughUb, rayBuf, rayBytes, roughStateBuf, roughStateBytes, probeCellBuf, probeCellBytes, roughBlendPipe,
         roughBlendLayout, roughBlendMeta, probesThisTick](RG::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl || !roughBlendMeta)
                return;
            Rendering::DescriptorSetDesc dsDesc{};
            dsDesc.layout = roughBlendLayout;
            dsDesc.transient = true;
            dsDesc.debugName = "DDGI.RoughBlend.Set0";
            auto ds = dev->CreateDescriptorSet(dsDesc);
            Rendering::NamedDescriptorWriter wd(dev, ds, *roughBlendMeta, 0);
            wd.AddUniformBuffer("DDGIParams", roughUb.Buffer, roughUb.Offset, sizeof(RoughBlendParamsUBO));
            wd.AddStorageBuffer("DDGIRayBufferRO", rayBuf, 0, rayBytes);
            wd.AddStorageBuffer("DDGIRoughState", roughStateBuf, 0, roughStateBytes);
            wd.AddStorageBuffer("DDGIProbeCellRO", probeCellBuf, 0, probeCellBytes);
            wd.Flush();
            Rendering::PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(roughBlendPipe);
            if (!pipe.IsValid())
                return;
            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            cl->Dispatch(probesThisTick, 1, 1);
        });

    struct RoughUploadParamsUBO
    {
        int32_t ProbeCount[4];
        uint32_t Dispatch0[4];
    };
    auto roughUploadUb = frame.AllocUpload<RoughUploadParamsUBO>();
    if (!roughUploadUb.Valid())
        return;
    RoughUploadParamsUBO rup{};
    rup.ProbeCount[0] = rp.ProbeCount[0];
    rup.ProbeCount[1] = rp.ProbeCount[1];
    rup.ProbeCount[2] = rp.ProbeCount[2];
    rup.ProbeCount[3] = probeTotal;
    const uint32_t roughUploadCount = refl.RoughAtlasUploaded ? probesThisTick : static_cast<uint32_t>(probeTotal);
    rup.Dispatch0[0] = refl.RoughAtlasUploaded ? probeBaseThisTick : 0u;
    rup.Dispatch0[1] = roughUploadCount;
    *roughUploadUb.Ptr = rup;

    const Rendering::TextureHandle roughAtlasTex = refl.RoughAtlas;
    // Declared so a reader of the atlas (the glossy resolve, the world pass) orders after this upload.
    const RG::RGTexture roughAtlasRG =
        frame.ImportExternalTexture(names.RoughAtlas, roughAtlasTex, Rendering::ResourceState::UnorderedAccess);
    const Rendering::ComputePipelineId roughUploadPipe = m_RoughUpload.PipelineId;
    const Rendering::DescriptorSetLayoutDesc roughUploadLayout = m_RoughUpload.Set0Layout;
    const Rendering::ShaderMeta* roughUploadMeta = m_RoughUpload.Meta.get();
    frame.AddComputePass(
        names.RoughUpload, Rendering::PassPhase::kDefault,
        [roughStateRG, roughAtlasRG](RG::RGPassBuilder& p)
        {
            p.PreventCulling();
            p.Read(roughStateRG, RG::RGBufferRead::Storage);
            p.Write(roughAtlasRG, RG::RGTextureWrite::Storage);
        },
        [roughUploadUb, roughStateBuf, roughStateBytes, roughAtlasTex, roughUploadPipe, roughUploadLayout,
         roughUploadMeta, roughUploadCount](RG::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl || !roughUploadMeta)
                return;
            Rendering::DescriptorSetDesc dsDesc{};
            dsDesc.layout = roughUploadLayout;
            dsDesc.transient = true;
            dsDesc.debugName = "DDGI.RoughUpload.Set0";
            auto ds = dev->CreateDescriptorSet(dsDesc);
            Rendering::NamedDescriptorWriter wd(dev, ds, *roughUploadMeta, 0);
            wd.AddUniformBuffer("DDGIParams", roughUploadUb.Buffer, roughUploadUb.Offset,
                                sizeof(RoughUploadParamsUBO));
            wd.AddStorageBuffer("DDGIRoughStateRO", roughStateBuf, 0, roughStateBytes);
            wd.Flush();
            Pipeline::Nodes::Detail::BindStorageImageByName(dev, ds, *roughUploadMeta, "uRoughAtlas",
                                                            roughAtlasTex);
            Rendering::PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(roughUploadPipe);
            if (!pipe.IsValid())
                return;
            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            cl->Dispatch(roughUploadCount, 1, 1);
        });

    refl.RoughAtlasUploaded = true;

    if (!refl.GlossyAllocated)
        return;

    struct GlossyBlendParamsUBO
    {
        int32_t ProbeCount[4];
        float Params0[4];
        uint32_t Dispatch0[4];
        float Timing[4];
    };
    auto glossyUb = frame.AllocUpload<GlossyBlendParamsUBO>();
    if (!glossyUb.Valid())
        return;
    GlossyBlendParamsUBO gp{};
    gp.ProbeCount[0] = rp.ProbeCount[0];
    gp.ProbeCount[1] = rp.ProbeCount[1];
    gp.ProbeCount[2] = rp.ProbeCount[2];
    gp.ProbeCount[3] = probeTotal;
    gp.Params0[0] = m_Volume.Hysteresis;
    gp.Dispatch0[0] = probeBaseThisTick;
    gp.Dispatch0[1] = static_cast<uint32_t>(grid.AllocatedRaysPerProbe);
    gp.Dispatch0[2] = probesThisTick;
    gp.Dispatch0[3] = static_cast<uint32_t>(shared.RayEpoch);
    gp.Timing[0] = shared.TickTimeMs;
    *glossyUb.Ptr = gp;

    const Rendering::BufferHandle glossyNumBuf = refl.GlossyNumerator;
    const Rendering::BufferHandle glossyWeightBuf = refl.GlossyWeight;
    const uint64_t glossyNumBytes =
        static_cast<uint64_t>(probeTotal) * uint64_t(glossyTexelsPerProbe) * sizeof(float) * 4;
    const uint64_t glossyWeightBytes =
        static_cast<uint64_t>(probeTotal) * uint64_t(glossyTexelsPerProbe) * sizeof(float);
    const RG::RGBuffer glossyNumRG = frame.ImportExternalBuffer(
        names.GlossyNumerator, glossyNumBuf, glossyNumBytes);
    const RG::RGBuffer glossyWeightRG = frame.ImportExternalBuffer(
        names.GlossyWeight, glossyWeightBuf, glossyWeightBytes);
    const Rendering::ComputePipelineId glossyBlendPipe = m_GlossyBlend.PipelineId;
    const Rendering::DescriptorSetLayoutDesc glossyBlendLayout = m_GlossyBlend.Set0Layout;
    const Rendering::ShaderMeta* glossyBlendMeta = m_GlossyBlend.Meta.get();
    frame.AddComputePass(
        names.GlossyBlend, Rendering::PassPhase::kDefault,
        [rayRG, probeCellRG, glossyNumRG, glossyWeightRG](RG::RGPassBuilder& p)
        {
            p.PreventCulling();
            p.Read(rayRG, RG::RGBufferRead::Storage);
            p.Write(glossyNumRG, RG::RGBufferWrite::Storage);
            p.Write(glossyWeightRG, RG::RGBufferWrite::Storage);
            p.Read(probeCellRG, RG::RGBufferRead::Storage);
        },
        [glossyUb, rayBuf, rayBytes, glossyNumBuf, glossyNumBytes, glossyWeightBuf, glossyWeightBytes,
         probeCellBuf, probeCellBytes, glossyBlendPipe, glossyBlendLayout, glossyBlendMeta,
         probesThisTick](RG::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl || !glossyBlendMeta)
                return;
            Rendering::DescriptorSetDesc dsDesc{};
            dsDesc.layout = glossyBlendLayout;
            dsDesc.transient = true;
            dsDesc.debugName = "DDGI.GlossyBlend.Set0";
            auto ds = dev->CreateDescriptorSet(dsDesc);
            Rendering::NamedDescriptorWriter wd(dev, ds, *glossyBlendMeta, 0);
            wd.AddUniformBuffer("DDGIParams", glossyUb.Buffer, glossyUb.Offset, sizeof(GlossyBlendParamsUBO));
            wd.AddStorageBuffer("DDGIRayBufferRO", rayBuf, 0, rayBytes);
            wd.AddStorageBuffer("DDGIGlossyNumerator", glossyNumBuf, 0, glossyNumBytes);
            wd.AddStorageBuffer("DDGIGlossyWeight", glossyWeightBuf, 0, glossyWeightBytes);
            wd.AddStorageBuffer("DDGIProbeCellRO", probeCellBuf, 0, probeCellBytes);
            wd.Flush();
            Rendering::PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(glossyBlendPipe);
            if (!pipe.IsValid())
                return;
            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            cl->Dispatch(probesThisTick, 1, 1);
        });

    struct GlossyUploadParamsUBO
    {
        int32_t ProbeCount[4];
        int32_t Atlas0[4];
        uint32_t Dispatch0[4];
    };
    auto glossyUploadUb = frame.AllocUpload<GlossyUploadParamsUBO>();
    if (!glossyUploadUb.Valid())
        return;
    GlossyUploadParamsUBO gup{};
    gup.ProbeCount[0] = rp.ProbeCount[0];
    gup.ProbeCount[1] = rp.ProbeCount[1];
    gup.ProbeCount[2] = rp.ProbeCount[2];
    gup.ProbeCount[3] = probeTotal;
    gup.Atlas0[0] = refl.GlossyLayout.TilesX;
    const uint32_t glossyUploadCount = refl.GlossyAtlasUploaded ? probesThisTick : static_cast<uint32_t>(probeTotal);
    gup.Dispatch0[0] = refl.GlossyAtlasUploaded ? probeBaseThisTick : 0u;
    gup.Dispatch0[1] = glossyUploadCount;
    *glossyUploadUb.Ptr = gup;

    const Rendering::TextureHandle glossyAtlasTex = refl.GlossyAtlas;
    // Declared so a reader of the atlas (the glossy resolve, the world pass) orders after this upload.
    const RG::RGTexture glossyAtlasRG =
        frame.ImportExternalTexture(names.GlossyAtlas, glossyAtlasTex, Rendering::ResourceState::UnorderedAccess);
    const Rendering::ComputePipelineId glossyUploadPipe = m_GlossyUpload.PipelineId;
    const Rendering::DescriptorSetLayoutDesc glossyUploadLayout = m_GlossyUpload.Set0Layout;
    const Rendering::ShaderMeta* glossyUploadMeta = m_GlossyUpload.Meta.get();
    frame.AddComputePass(
        names.GlossyUpload, Rendering::PassPhase::kDefault,
        [glossyNumRG, glossyWeightRG, glossyAtlasRG](RG::RGPassBuilder& p)
        {
            p.PreventCulling();
            p.Read(glossyNumRG, RG::RGBufferRead::Storage);
            p.Read(glossyWeightRG, RG::RGBufferRead::Storage);
            p.Write(glossyAtlasRG, RG::RGTextureWrite::Storage);
        },
        [glossyUploadUb, glossyNumBuf, glossyNumBytes, glossyWeightBuf, glossyWeightBytes, glossyAtlasTex,
         glossyUploadPipe, glossyUploadLayout, glossyUploadMeta, glossyUploadCount](RG::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl || !glossyUploadMeta)
                return;
            Rendering::DescriptorSetDesc dsDesc{};
            dsDesc.layout = glossyUploadLayout;
            dsDesc.transient = true;
            dsDesc.debugName = "DDGI.GlossyUpload.Set0";
            auto ds = dev->CreateDescriptorSet(dsDesc);
            Rendering::NamedDescriptorWriter wd(dev, ds, *glossyUploadMeta, 0);
            wd.AddUniformBuffer("DDGIParams", glossyUploadUb.Buffer, glossyUploadUb.Offset,
                                sizeof(GlossyUploadParamsUBO));
            wd.AddStorageBuffer("DDGIGlossyNumeratorRO", glossyNumBuf, 0, glossyNumBytes);
            wd.AddStorageBuffer("DDGIGlossyWeightRO", glossyWeightBuf, 0, glossyWeightBytes);
            wd.Flush();
            Pipeline::Nodes::Detail::BindStorageImageByName(dev, ds, *glossyUploadMeta, "uGlossyAtlas",
                                                            glossyAtlasTex);
            Rendering::PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(glossyUploadPipe);
            if (!pipe.IsValid())
                return;
            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            cl->Dispatch(glossyUploadCount, 1, 1);
        });
    refl.GlossyAtlasUploaded = true;
}

namespace
{
// GE_DDGI_ATLAS_TRACE=1 reports whether each material-map layer's blit is
// actually RECORDED. A layer that is armed every frame but never recorded is
// a different defect from one that records and still samples black.
bool DDGIAtlasTraceEnabled()
{
    static const bool kOn = []
    {
        const char* v = std::getenv("GE_DDGI_ATLAS_TRACE");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return kOn;
}
}  // namespace

void DDGIProbeFeature::DeclareMapAtlasPasses(Rendering::RenderGraph::RGFrame& frame)
{
    namespace RG = Rendering::RenderGraph;
    if (!m_MapAtlas)
        return;
    m_MapAtlas->Refresh();

    const auto pending = m_MapAtlas->GetPendingBlits();
    if (DDGIAtlasTraceEnabled())
        Logger::Log::Info("[DDGIAtlasTrace] pending={} blitPipeline={}", pending.size(),
                          m_MapBlit.PipelineId.IsValid() ? "valid" : "INVALID");
    if (pending.empty() || !m_MapBlit.PipelineId.IsValid())
        return;

    const Rendering::TextureHandle atlas = m_MapAtlas->GetTexture();
    const Rendering::SamplerHandle sampler = m_MapAtlas->GetSampler();
    if (!atlas.IsValid() || !sampler.IsValid())
        return;

    // The atlas is a persistent device texture, so the graph only learns about
    // it through an import. Without the import plus the Write/Read pair below,
    // nothing emits the StorageWrite->SampledRead barrier between this blit and
    // the trace kernels that sample the atlas later in the SAME frame, and the
    // fill is never visible to them. Layers are filled once and never rewritten,
    // so a missed barrier is permanent rather than a one-frame artifact.
    const RG::RGTexture atlasRG = frame.ImportExternalTexture(
        "DDGI.MaterialMapAtlas", atlas, Rendering::ResourceState::ShaderResource,
        Rendering::TextureFormat::RGBA8_UNORM, 1, std::max(m_MapAtlas->GetLayerCount(), 1u));

    struct BlitParamsUBO
    {
        uint32_t LayerAndSize[4];
    };

    const Rendering::ComputePipelineId blitPipe = m_MapBlit.PipelineId;
    const Rendering::DescriptorSetLayoutDesc blitLayout = m_MapBlit.Set0Layout;
    const Rendering::ShaderMeta* blitMeta = m_MapBlit.Meta.get();
    constexpr uint32_t kBlitGroupSize = 8;
    const uint32_t groups = (DDGIMaterialMapAtlas::kLayerSize + kBlitGroupSize - 1) / kBlitGroupSize;

    for (const DDGIMaterialMapAtlas::PendingBlit& blit : pending)
    {
        if (!blit.Source.IsValid())
            continue;
        auto ub = frame.AllocUpload<BlitParamsUBO>();
        if (!ub.Valid())
            return;
        // The source is a streamed material texture whose GPU upload is encoded
        // outside the graph (TextureService::FlushPendingUploads, frame top).
        // Importing it and declaring the sampled read puts that read under the
        // graph's hazard tracking, so this dispatch cannot observe the texture
        // ahead of the upload that filled it. Without it the very first blit of
        // a freshly streamed map can sample an empty texture, and PendingBlit's
        // Recorded latch then keeps that empty layer forever.
        const RG::RGTexture sourceRG =
            frame.ImportExternalTexture("DDGI.MapBlit.Source", blit.Source,
                                        Rendering::ResourceState::ShaderResource);
        ub.Ptr->LayerAndSize[0] = blit.Layer;
        ub.Ptr->LayerAndSize[1] = DDGIMaterialMapAtlas::kLayerSize;
        ub.Ptr->LayerAndSize[2] = 0;
        ub.Ptr->LayerAndSize[3] = 0;

        // kEarlySetup, like the hardware lane's AS build: the trace kernels
        // sample this atlas in the SAME frame's kDefault phase, so the fill
        // has to be recorded ahead of them.
        frame.AddComputePass(
            "DDGI.MapBlit", Rendering::PassPhase::kEarlySetup,
            [atlasRG, sourceRG, layer = blit.Layer](RG::RGPassBuilder& p)
            {
                p.PreventCulling();
                p.Read(sourceRG, RG::RGTextureRead::Sampled);
                RG::RGRange range = RG::RGRange::All();
                range.BaseMip = 0;
                range.MipCount = 1;
                range.BaseLayer = layer;
                range.LayerCount = 1;
                p.Write(atlasRG, RG::RGTextureWrite::Storage, range);
            },
            [ub, atlas, sampler, source = blit.Source, blitPipe, blitLayout, blitMeta,
             layerIdx = blit.Layer, recorded = blit.Recorded](RG::RGContext& ctx)
            {
                auto* dev = ctx.GetDevice();
                auto* cl = ctx.Cmd;
                if (!dev || !cl || !blitMeta)
                    return;
                Rendering::DescriptorSetDesc dsDesc{};
                dsDesc.layout = blitLayout;
                dsDesc.transient = true;
                dsDesc.debugName = "DDGI.MapBlit.Set0";
                auto ds = dev->CreateDescriptorSet(dsDesc);
                Rendering::NamedDescriptorWriter wd(dev, ds, *blitMeta, 0);
                wd.AddCombinedImageSampler("uSourceTexture", source, sampler);
                // Instance name, not block name — see DDGIClearParams's fix.
                wd.AddUniformBuffer("DDGIParams", ub.Buffer, ub.Offset, sizeof(BlitParamsUBO));
                wd.Flush();
                // A silent false here is an unbound write target: the dispatch runs
                // and stores nothing. This helper's result must never be dropped.
                if (!Pipeline::Nodes::Detail::BindStorageImageByName(dev, ds, *blitMeta, "uAtlas",
                                                                     atlas))
                {
                    Logger::Log::Error(
                        "DDGI.MapBlit: 'uAtlas' storage-image bind FAILED (set valid={}, image valid={}) "
                        "— layer {} would be left unwritten; skipping the dispatch",
                        ds.IsValid(), atlas.IsValid(), layerIdx);
                    return;
                }
                if (DDGIAtlasTraceEnabled())
                    Logger::Log::Info("[DDGIAtlasTrace] uAtlas bound OK for layer {}", layerIdx);
                Rendering::PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(blitPipe);
                if (!pipe.IsValid())
                    return;
                cl->SetPipeline(pipe);
                cl->BindDescriptorSet(0, ds, pipe);
                cl->Dispatch(groups, groups, 1);
                // Acknowledge only here. Every early return above leaves the
                // layer unfilled, and the atlas re-arms it next Refresh — the
                // alternative (consuming at declaration) turns a one-frame
                // pipeline miss into a permanently black map, because the
                // layout hash has no reason to move again.
                if (recorded)
                    recorded->store(true, std::memory_order_release);
                if (DDGIAtlasTraceEnabled())
                    Logger::Log::Info("[DDGIAtlasTrace] RECORDED blit for layer {}",
                                      layerIdx);
            });
    }
}

void DDGIProbeFeature::DeclareSwSkinnedRefit(Rendering::RenderGraph::RGFrame& frame,
                                             RenderServices& services,
                                             const std::vector<Rendering::GPUInstance>& instances,
                                             bool solveWillRun)
{
    namespace RG = Rendering::RenderGraph;
    m_TickSwNodesRG = {};
    m_TickSwVertexDataRG = {};
    if (!m_SkinnedGeometry || !solveWillRun || !m_SwRefit.PipelineId.IsValid())
        return;
    const std::vector<DDGISceneService::SkinnedRefitRange>& ranges =
        m_SceneService->GetSkinnedRefitRanges();
    if (ranges.empty())
        return;

    const SkinPaletteAtlas& palette = services.GetSkinPaletteAtlas();
    m_SkinnedGeometry->Tick(frame, instances, palette.GetBuffer(), palette.GetBufferBytes(),
                            /*hardwareLane=*/false);

    // Mirrors ddgi_bvh_refit.comp's DDGIRefitParams (std140 uvec4).
    struct RefitParamsUBO
    {
        uint32_t NodeBegin = 0;
        uint32_t NodeCount = 0;
        uint32_t VertexBegin = 0;
        uint32_t VertexCount = 0;
    };
    struct RefitDispatch
    {
        RG::RGFrame::TypedUpload<RefitParamsUBO> Ub;
        Rendering::BufferHandle Posed{};
        uint64_t PosedBytes = 0;
    };
    auto dispatches = std::make_shared<std::vector<RefitDispatch>>();
    std::vector<RG::RGBuffer> posedHandles;
    for (const DDGISceneService::SkinnedRefitRange& range : ranges)
    {
        DDGISkinnedGeometry::PosedBuffer posed;
        if (!m_SkinnedGeometry->TryGetPosedBuffer(range.SkinnedRuntimeId, range.GpuMeshIndex,
                                                  range.VertexCount, posed))
            continue;  // despawned mid-plan or repooled — the tree keeps its last pose
        RefitDispatch d;
        d.Ub = frame.AllocUpload<RefitParamsUBO>();
        if (!d.Ub.Valid())
            continue;
        d.Ub.Ptr->NodeBegin = range.NodeBegin;
        d.Ub.Ptr->NodeCount = range.NodeCount;
        d.Ub.Ptr->VertexBegin = range.VertexBegin;
        d.Ub.Ptr->VertexCount = range.VertexCount;
        d.Posed = posed.Buffer;
        d.PosedBytes = posed.Bytes;
        dispatches->push_back(d);
        posedHandles.push_back(posed.RG);
    }
    if (dispatches->empty())
        return;

    const DDGISceneService::SceneBuffers& sw = m_SceneService->GetSceneBuffers();
    RG::RGBuffer nodesRG = frame.ImportExternalBuffer("DDGI.SwNodes", sw.Nodes, sw.NodesBytes);
    RG::RGBuffer vertsRG =
        frame.ImportExternalBuffer("DDGI.SwVertexData", sw.VertexData, sw.VertexDataBytes);

    const Rendering::ComputePipelineId refitPipe = m_SwRefit.PipelineId;
    const Rendering::DescriptorSetLayoutDesc refitLayout = m_SwRefit.Set0Layout;
    const Rendering::ShaderMeta* refitMeta = m_SwRefit.Meta.get();
    const Rendering::BufferHandle triIndices = sw.TriangleIndices;
    const uint64_t triIndicesBytes = sw.TriangleIndicesBytes;
    const Rendering::BufferHandle nodesBuf = sw.Nodes;
    const uint64_t nodesBytes = sw.NodesBytes;
    const Rendering::BufferHandle vertsBuf = sw.VertexData;
    const uint64_t vertsBytes = sw.VertexDataBytes;
    frame.AddComputePass(
        "DDGI.SwSkinnedRefit", Rendering::PassPhase::kEarlySetup,
        [nodesRG, vertsRG, posedHandles](RG::RGPassBuilder& p)
        {
            p.PreventCulling();
            p.Write(nodesRG, RG::RGBufferWrite::Storage);
            p.Write(vertsRG, RG::RGBufferWrite::Storage);
            for (RG::RGBuffer b : posedHandles)
                p.Read(b, RG::RGBufferRead::Storage);
        },
        [dispatches, refitPipe, refitLayout, refitMeta, triIndices, triIndicesBytes, nodesBuf,
         nodesBytes, vertsBuf, vertsBytes](RG::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl || !refitMeta)
                return;
            Rendering::PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(refitPipe);
            if (!pipe.IsValid())
                return;
            cl->SetPipeline(pipe);
            for (const RefitDispatch& d : *dispatches)
            {
                Rendering::DescriptorSetDesc dsDesc{};
                dsDesc.layout = refitLayout;
                dsDesc.transient = true;
                dsDesc.debugName = "DDGI.SwSkinnedRefit.Set0";
                auto ds = dev->CreateDescriptorSet(dsDesc);
                Rendering::NamedDescriptorWriter wd(dev, ds, *refitMeta, 0);
                wd.AddUniformBuffer("RefitParams", d.Ub.Buffer, d.Ub.Offset,
                                    sizeof(RefitParamsUBO));
                wd.AddStorageBuffer("SkinnedVerts", d.Posed, 0, d.PosedBytes);
                wd.AddStorageBuffer("BvhNodes", nodesBuf, 0, nodesBytes);
                wd.AddStorageBuffer("BvhTriangleIndices", triIndices, 0, triIndicesBytes);
                wd.AddStorageBuffer("BvhVertexData", vertsBuf, 0, vertsBytes);
                wd.Flush();
                cl->BindDescriptorSet(0, ds, pipe);
                cl->Dispatch(1, 1, 1);
            }
        });

    m_TickSwNodesRG = nodesRG;
    m_TickSwVertexDataRG = vertsRG;
}

bool DDGIProbeFeature::ClaimHardwareLane()
{
    if (m_TlasSlot.IsValid())
        return true;
    m_TlasSlot = m_SceneAS->AcquireTlasChannel("DDGI");
    if (!m_TlasSlot.IsValid())
        return false;
    // Entries built for the software lane carry no BLAS to build.
    if (m_SkinnedGeometry)
        m_SkinnedGeometry->RetireAll();
    return true;
}

void DDGIProbeFeature::ReleaseHardwareLane()
{
    if (!m_TlasSlot.IsValid())
        return;
    // The channel's TLAS dies with it; nothing below may trust it again.
    m_SceneAS->ReleaseTlasChannel(m_TlasSlot);
    m_TlasSlot = {};
    m_TlasContentValid = false;
    m_BuiltInstanceHash = 0;
    m_PendingTlasHash = 0;
    m_TlasBuildExecuted.reset();
    m_HaveContentEpoch = false;
    m_LastContentEpoch = 0;
    // No backend handle may outlive the claim: an unclaimed pool is purged,
    // which restarts handle ids.
    if (m_SkinnedGeometry)
        m_SkinnedGeometry->RetireAll();
}

bool DDGIProbeFeature::TickHardwareLane(Rendering::RenderGraph::RGFrame& frame, bool anyBlasBecameReady,
                                        const std::vector<Rendering::GPUInstance>& instances,
                                        Rendering::BufferHandle skinPaletteBuffer,
                                        uint64_t skinPaletteBytes, bool solveWillRun)
{
    namespace RG = Rendering::RenderGraph;
    Rendering::IAccelerationStructureBackend* backend = m_SceneAS->GetBackend();
    if (!backend)
        return false;

    // 1. TLAS exec confirmation (M4): a declared build becomes trusted
    // content only after its execution is confirmed (RTShadowMaskService::
    // Schedule's discipline, mirrored) — a declared-but-uncalled pass (RG
    // culling, device loss between declare and execute) must not leave
    // m_TlasContentValid claiming valid content the epoch/hash gate below
    // would then trust across frames. Single/shared across cascades — one
    // scene, one TLAS.
    if (m_TlasBuildExecuted)
    {
        if (m_TlasBuildExecuted->load(std::memory_order_acquire))
        {
            m_TlasContentValid = true;
            m_BuiltInstanceHash = m_PendingTlasHash;
        }
        m_TlasBuildExecuted.reset();
    }

    // Drain retired instance-staging buffers every tick regardless of
    // whether a new one is staged below — same frame-margin discipline as
    // the shared BLAS pool's own retirement queues.
    ++m_FrameClock;
    constexpr uint64_t kRetireMargin = Rendering::IDevice::kMaxSupportedFramesInFlight + 1;
    std::erase_if(m_RetiredInstanceBuffers,
                  [&](const RetiredBuffer& r)
                  {
                      if (m_FrameClock - r.FrameStamp <= kRetireMargin)
                          return false;
                      m_Device->DestroyBuffer(r.Buffer);
                      return true;
                  });

    // Skinned per-instance BLASes: skin-compute + rebuild declared here so the
    // instance filter below can reference confirmed skinned rows, and so a
    // pose change (BLAS content moving under a stable address) forces a TLAS
    // rebuild even when the instance list hashes identical.
    // Held solve (strict idle-gating, camera moving): skip the skin-compute +
    // rebuild work entirely — nothing below the gate consumes it this tick,
    // and the first accepted tick re-skins from the live palettes before the
    // trace runs (the skin pass and AS pass both declare in kEarlySetup).
    DDGISkinnedGeometry::TickResult skinned;
    if (m_SkinnedGeometry && solveWillRun)
        skinned = m_SkinnedGeometry->Tick(frame, instances, skinPaletteBuffer, skinPaletteBytes,
                                          /*hardwareLane=*/true);
    const bool skinnedActive = !skinned.Builds.empty() || !skinned.TlasRows.empty();
    m_TickSkinnedRowMapBuffer = skinned.RowMapBuffer;
    m_TickSkinnedRowMapOffset = skinned.RowMapOffset;
    m_TickSkinnedRowMapBytes = skinned.RowMapBytes;
    m_TickSkinnedGeomRowsBuffer = skinned.GeomRowsBuffer;
    m_TickSkinnedGeomRowsOffset = skinned.GeomRowsOffset;
    m_TickSkinnedGeomRowsBytes = skinned.GeomRowsBytes;

    // 2. Collect+claim this frame's pending BLAS builds (shared pool; may be
    // empty if every mesh DDGI needs is already Ready) — collected BEFORE
    // the epoch gate below so a pending BLAS build still gets recorded even
    // on a tick where DDGI's own TLAS content is unchanged.
    std::shared_ptr<SceneAccelerationStructureService::BuildConfirmToken> blasConfirmToken;
    std::vector<SceneAccelerationStructureService::PendingBlasBuild> builds =
        m_SceneAS->CollectPendingBuilds(blasConfirmToken);

    // 3. Epoch gate (M4): an unchanged GPUScene content epoch, no BLAS that
    // just became ready, and already-confirmed TLAS content means the
    // current TLAS is exact — skip rebuilding the CPU instance list AND
    // declaring a TLAS build entirely (only a pending BLAS build, if any,
    // still needs a pass).
    const uint64_t contentEpoch = m_GpuScene->GetContentEpoch();
    const bool epochChanged = !m_HaveContentEpoch || contentEpoch != m_LastContentEpoch;
    m_HaveContentEpoch = true;
    m_LastContentEpoch = contentEpoch;
    const bool needRefresh = epochChanged || anyBlasBecameReady || !m_TlasContentValid || skinnedActive;

    bool doTlasBuild = false;
    Rendering::BufferHandle instanceBuffer{};
    uint32_t instanceCount = 0;
    if (needRefresh)
    {
        // 4. Refresh: rebuild the filtered instance list and hash it — the
        // epoch signal over-invalidates (any GPUScene content change bumps
        // it, not just transforms DDGI's filter cares about), the hash
        // keeps those spurious bumps off the GPU (RTShadowMaskService's
        // exact rationale).
        std::vector<Rendering::TlasInstanceData> instanceScratch;
        instanceScratch.reserve(instances.size());
        for (uint32_t i = 0; i < instances.size(); ++i)
        {
            const Rendering::GPUInstance& inst = instances[i];
            if (inst.skinPaletteOffset != 0u)
                continue;
            if ((inst.sectorPacked[0] | inst.sectorPacked[1]) != 0u)
                continue;
            if (!m_SceneAS->IsBlasReady(inst.meshIndex))
                continue;
            const float* m = inst.transform.Data();
            bool sane = std::isfinite(inst.boundingRadius) && inst.boundingRadius < kMaxSaneWorldUnits;
            for (int f = 0; f < 16 && sane; ++f)
                sane = std::isfinite(m[f]) && std::fabs(m[f]) < kMaxSaneWorldUnits;
            if (!sane)
                continue;

            Rendering::TlasInstanceData dst{};
            for (uint32_t r = 0; r < 3; ++r)
                for (uint32_t c = 0; c < 4; ++c)
                    dst.Transform[r * 4 + c] = m[c * 4 + r];
            dst.CustomIndexAndMask = (i & 0x00FFFFFFu) | (0xFFu << 24);
            dst.SbtOffsetAndFlags = Rendering::kTlasInstanceFlagTriangleFacingCullDisable << 24;
            dst.BlasAddress = m_SceneAS->GetBlasAddress(inst.meshIndex);
            instanceScratch.push_back(dst);
        }
        for (const DDGISkinnedGeometry::SkinnedTlasRow& row : skinned.TlasRows)
        {
            const Rendering::GPUInstance& inst = instances[row.InstanceIndex];
            const float* m = inst.transform.Data();
            bool sane = std::isfinite(inst.boundingRadius) && inst.boundingRadius < kMaxSaneWorldUnits;
            for (int f = 0; f < 16 && sane; ++f)
                sane = std::isfinite(m[f]) && std::fabs(m[f]) < kMaxSaneWorldUnits;
            if (!sane)
                continue;
            Rendering::TlasInstanceData dst{};
            for (uint32_t r = 0; r < 3; ++r)
                for (uint32_t c = 0; c < 4; ++c)
                    dst.Transform[r * 4 + c] = m[c * 4 + r];
            dst.CustomIndexAndMask = (row.InstanceIndex & 0x00FFFFFFu) | (0xFFu << 24);
            dst.SbtOffsetAndFlags = Rendering::kTlasInstanceFlagTriangleFacingCullDisable << 24;
            dst.BlasAddress = row.BlasAddress;
            instanceScratch.push_back(dst);
        }
        instanceCount = static_cast<uint32_t>(instanceScratch.size());

        uint64_t refreshHash = 1469598103934665603ull ^ instanceCount;  // FNV-1a, count-seeded
        const auto* bytes = reinterpret_cast<const uint8_t*>(instanceScratch.data());
        const size_t byteCount = static_cast<size_t>(instanceCount) * sizeof(Rendering::TlasInstanceData);
        for (size_t b = 0; b < byteCount; ++b)
        {
            refreshHash ^= bytes[b];
            refreshHash *= 1099511628211ull;
        }
        doTlasBuild = !m_TlasContentValid || refreshHash != m_BuiltInstanceHash || !skinned.Builds.empty();

        if (doTlasBuild && refreshHash != m_BuiltInstanceHash)
        {
            // What the trace actually sees, once per content change: a probe
            // ray that leaks through a wall is far more often a wall that is
            // not in this list than a weighting fault.
            Logger::Log::Info("DDGI: trace scene rebuilt with {} of {} instances", instanceCount,
                              instances.size());
            for (const Rendering::TlasInstanceData& d : instanceScratch)
            {
                const uint32_t idx = d.CustomIndexAndMask & 0x00FFFFFFu;
                const Rendering::GPUInstance& gi = instances[idx];
                Logger::Log::Debug("DDGI:   instance {} at ({:.2f}, {:.2f}, {:.2f}) scale ({:.2f}, {:.2f}, {:.2f}) "
                                   "normalCols ({:.3f} {:.3f} {:.3f}) ({:.3f} {:.3f} {:.3f}) ({:.3f} {:.3f} {:.3f})",
                                   idx, d.Transform[3], d.Transform[7], d.Transform[11], d.Transform[0],
                                   d.Transform[5], d.Transform[10], gi.normalMatrixCol0.x, gi.normalMatrixCol0.y,
                                   gi.normalMatrixCol0.z, gi.normalMatrixCol1.x, gi.normalMatrixCol1.y,
                                   gi.normalMatrixCol1.z, gi.normalMatrixCol2.x, gi.normalMatrixCol2.y,
                                   gi.normalMatrixCol2.z);
            }
        }

        if (doTlasBuild)
        {
            if (m_TlasInstanceStaging.IsValid() && refreshHash == m_StagedInstanceHash)
            {
                // Pose-only rebuild: the instance bytes already staged are
                // identical, so the build re-reads them — no allocation, no
                // host write, no CPU-write-vs-in-flight-read hazard.
                instanceBuffer = m_TlasInstanceStaging;
            }
            else
            {
                Rendering::BufferDesc instDesc{};
                instDesc.size = std::max<uint64_t>(instanceCount, 1u) * sizeof(Rendering::TlasInstanceData);
                instDesc.usage = static_cast<uint32_t>(Rendering::BufferUsage::AccelerationStructureBuildInput |
                                                       Rendering::BufferUsage::ShaderDeviceAddress);
                instDesc.memoryUsage = Rendering::BufferMemoryUsage::Upload;
                instDesc.flags = Rendering::BufferCreateFlags::PersistentlyMapped;
                instDesc.debugName = "DDGI.TlasInstances";
                instanceBuffer = m_Device->CreateBuffer(instDesc);
                void* instanceMapped = instanceBuffer.IsValid() ? m_Device->MapBuffer(instanceBuffer) : nullptr;
                if (!instanceMapped)
                {
                    if (instanceBuffer.IsValid())
                        m_Device->DestroyBuffer(instanceBuffer);
                    return false;
                }
                if (instanceCount > 0)
                    std::memcpy(instanceMapped, instanceScratch.data(),
                                instanceCount * sizeof(Rendering::TlasInstanceData));
                // Retire the PREVIOUS staging buffer (in-flight frames may
                // still read it); the new one lives in the member until it is
                // replaced in turn.
                if (m_TlasInstanceStaging.IsValid())
                    m_RetiredInstanceBuffers.push_back(
                        RetiredBuffer{m_TlasInstanceStaging, m_FrameClock});
                m_TlasInstanceStaging = instanceBuffer;
                m_StagedInstanceHash = refreshHash;
            }
            if (!backend->PrepareTlas(m_TlasSlot, instanceCount))
                return false;
            m_PendingTlasHash = refreshHash;
            m_TlasContentValid = false;  // until the exec confirmation above sees it ran
            m_TlasBuildExecuted = std::make_shared<std::atomic<bool>>(false);
        }
    }

    if (doTlasBuild || !builds.empty() || !skinned.Builds.empty())
    {
        // 5. AS build pass (BLAS batch + this feature's TLAS, conditionally).
        // A TLAS build writes the graph's DDGI.TLAS, which the classify and
        // trace passes read, so they schedule after it and wait for it across
        // queues; a BLAS-only tick has no tracked output and is kept by
        // PreventCulling, as RTShadowMaskService's ASBuild is. recordTlas is captured
        // separately from doTlasBuild's outer scope so a builds-only tick
        // (TLAS unchanged, but a mesh's BLAS is ready to build) declares a
        // pass that skips RecordTlasBuild rather than replaying a stale one.
        const bool recordTlas = doTlasBuild;
        auto tlasExecuted = m_TlasBuildExecuted;  // null unless doTlasBuild
        auto blasBuilds = std::make_shared<std::vector<SceneAccelerationStructureService::PendingBlasBuild>>(
            std::move(builds));
        auto skinnedBuilds = std::make_shared<std::vector<DDGISkinnedGeometry::PendingSkinnedBuild>>(
            std::move(skinned.Builds));
        auto skinnedConfirm = skinned.ConfirmToken;
        const Rendering::TlasSlotHandle tlasSlot = m_TlasSlot;
        const RG::RGAccelerationStructure tlasRG =
            recordTlas ? frame.ImportAccelerationStructure("DDGI.TLAS", tlasSlot) : RG::RGAccelerationStructure{};
        frame.AddPass(
            "DDGI.ASBuild", Rendering::PassPhase::kEarlySetup,
            [skinnedBuilds, tlasRG](RG::RGPassBuilder& p)
            {
                p.PreventCulling();
                if (tlasRG.IsValid())
                    p.Write(tlasRG);
                // Order this pass after the skin-compute writes it consumes.
                for (const auto& b : *skinnedBuilds)
                    if (b.SkinnedRG.IsValid())
                        p.Read(b.SkinnedRG, RG::RGBufferRead::Storage);
            },
            [backend, blasBuilds, skinnedBuilds, skinnedConfirm, blasConfirmToken, tlasSlot,
             instanceBuffer, instanceCount, recordTlas, tlasExecuted](RG::RGContext& ctx)
            {
                if (!ctx.Cmd)
                    return;
                backend->RecordPreBuildBarrier(*ctx.Cmd);
                for (const auto& b : *blasBuilds)
                    backend->RecordBlasBuild(*ctx.Cmd, b.Handle, b.Geometry);
                for (const auto& b : *skinnedBuilds)
                    backend->RecordBlasBuild(*ctx.Cmd, b.Handle, b.Geometry);
                if (recordTlas)
                    backend->RecordTlasBuild(*ctx.Cmd, tlasSlot, instanceBuffer, 0, instanceCount);
                if (blasConfirmToken)
                    blasConfirmToken->MarkExecuted();
                if (skinnedConfirm)
                    skinnedConfirm->store(true, std::memory_order_release);
                if (tlasExecuted)
                    tlasExecuted->store(true, std::memory_order_release);
            });
    }

    if (!m_TlasContentValid && !m_TlasBuildExecuted)
        return false;  // no confirmed TLAS and no build in flight — nothing to trace against yet
    return true;
}

void DDGIProbeFeature::DeclareProbePasses(Rendering::RenderGraph::RGFrame& frame, RenderServices& services,
                               float deltaTimeSeconds, bool anyBlasBecameReady)
{
    namespace RG = Rendering::RenderGraph;
    if (!m_Device || !m_GpuScene)
        return;
    if (!m_Volume.Enabled)
    {
        ReleaseHardwareLane();
        return;
    }

    // M7 lane selection: hardware ray-query when the device supports it and
    // a build-time-visible env var doesn't force the software lane for A/B
    // (mirrors this codebase's established GE_VK_* dev-toggle convention —
    // e.g. GE_VK_CAPTURE_COMPAT — rather than a new EditorSettingsRegistry
    // page for what is fundamentally a developer/QA switch, not an end-user
    // setting). Decided ONCE per tick so C0/C1 and glossy never disagree
    // about which lane is active within the same frame.
    const bool forceSoftware = []
    {
        const char* v = std::getenv("GE_DDGI_FORCE_SOFTWARE");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    // m_HardwareLaneFailed stays in the gate as permanent defence in depth:
    // a pipeline variant's compile is only provable at execution time, and a
    // backend that cannot build one must degrade to the software lane rather
    // than retry (and re-log) every tick.
    //
    // No backend is special-cased out of the hardware lane. Metal was pinned to
    // software for a while because its GI drifted into visible low-frequency
    // blobs; that was the hysteresis/ray-basis mismatch, not the lane — the port
    // rotated the spherical-Fibonacci basis every solve while retaining only the
    // HELD basis's 0.6 history. Paired correctly the hardware lane's residual
    // drift is 1.15% of pixels past 6/255 with max 16 (the blob state was 16.73%,
    // max 37) and it costs 55 ms/frame less on Sponza — 136.09 vs 191.07 ms total
    // GPU, the software trace alone accounting for 73.14 ms of the difference.
    // GE_DDGI_FORCE_SOFTWARE=1 forces the software lane on any backend.
    const bool useHardware = m_SceneAS && m_Device->GetCapabilities().supportsRayQuery && !forceSoftware &&
                             !m_HardwareLaneFailed.load(std::memory_order_acquire);
    if (!useHardware)
        ReleaseHardwareLane();
    if (!useHardware && !m_SceneService)
        return;  // no hardware lane available and the software lane never initialized

    LoadKernelsIfNeeded();
    RG::RGBuffer swPackedSceneRG{};
    if (useHardware)
    {
        if (!m_Classify.PipelineId.IsValid() || !m_TraceHw.PipelineId.IsValid() ||
            !m_Blend.PipelineId.IsValid() || !m_Upload.PipelineId.IsValid() || !m_Clear.PipelineId.IsValid())
            return;
    }
    else
    {
        if (!m_TraceSw.PipelineId.IsValid() || !m_Blend.PipelineId.IsValid() ||
            !m_Upload.PipelineId.IsValid() || !m_Clear.PipelineId.IsValid())
            return;
    }

    // Fine depth needs both of its kernels; without them the volume degrades
    // to Shared (a correct field at the coarser moments) rather than to no
    // field. Resolved once per tick so C0 and C1 never disagree.
    const bool fineDepthAvailable = m_DepthBlend.PipelineId.IsValid() && m_DepthUpload.PipelineId.IsValid();
    const int32_t depthTile =
        m_Volume.DepthResolution == Components::DDGIDepthResolution::Fine && fineDepthAvailable ? kDepthTileFine
                                                                                                 : kTile;
    EnsureCascadeGpuResources(m_C0, m_Volume.GridSizeWS, m_Volume.ProbesLongAxis, m_Volume.RaysPerProbe,
                              depthTile, deltaTimeSeconds, kC0DebugNames);
    float fineMinWS[3] = {0.0f, 0.0f, 0.0f};
    float fineSizeWS[3] = {1.0f, 1.0f, 1.0f};
    if (m_Volume.EnableFineCascade)
    {
        ComputeFineCascadeBounds(m_Volume, fineMinWS, fineSizeWS);
        EnsureCascadeGpuResources(m_C1, fineSizeWS, m_Volume.ProbesLongAxis, m_Volume.RaysPerProbe,
                                  depthTile, deltaTimeSeconds, kC1DebugNames);
    }
    if (!m_C0.GpuGridValid)
        return;

    // Reflection lobes follow each cascade's resolved size. Safe to call
    // every tick — it no-ops once sizes already match (see its doc).
    EnsureReflectionResources(deltaTimeSeconds);

    // Cheap either way (GPUScene owns this list already) — hoisted above the
    // lane branch since the shared SharedTickInputs population below still
    // needs it (GpuInstanceBytes) even on the hardware lane after
    // TickHardwareLane returns.
    const std::vector<Rendering::GPUInstance>& instances = m_GpuScene->GetInstances();

    // Before either lane: the software lane's scene rebuild bakes the atlas's
    // layer indices into its uber-material records, so the layout must already
    // be current when DDGISceneService::Tick runs below.
    DeclareMapAtlasPasses(frame);

    // The idle gate is sampled here, ahead of the lane tick, so the lane can
    // skip skinned skin/rebuild work on a held tick — but the HOLD itself is
    // applied below, after the latched correctness work (see the block there).
    const bool cameraMoving =
        m_CameraIdleGate.UpdateAndIsMoving(services.Views(), m_Volume.WorldId, deltaTimeSeconds);
    const bool solveWillRun = m_Volume.ContinuousSolve || !cameraMoving;
    // Per-tick RG handles: a lane that does not re-import must not leak last
    // frame's resource ids into this frame's pass declarations.
    m_TickSwNodesRG = {};
    m_TickSwVertexDataRG = {};

    if (useHardware)
    {
        if (!ClaimHardwareLane())
            return;
        const SkinPaletteAtlas& palette = services.GetSkinPaletteAtlas();
        if (!TickHardwareLane(frame, anyBlasBecameReady, instances, palette.GetBuffer(),
                              palette.GetBufferBytes(), solveWillRun))
            return;
    }
    else
    {
        // M7 software lane: DDGISceneService owns its own sweep/rebuild
        // cycle (epoch/hash-gated, same discipline as the hardware lane's
        // TLAS gate above — see its Tick doc) — no TLAS/BLAS bookkeeping
        // needed here at all.
        m_SceneService->Tick(deltaTimeSeconds);
        if (!m_SceneService->GetSceneBuffers().IsValid())
            return;  // scene still resolving (streaming meshes) — nothing to trace against yet
        const auto& sw = m_SceneService->GetSceneBuffers();
        swPackedSceneRG = frame.ImportExternalBuffer("DDGI.Sw.PackedScene", sw.PackedScene,
                                                     sw.PackedSceneBytes);
        if (!m_SceneService->DeclareMaterialUploads(frame, swPackedSceneRG))
            return;
        DeclareSwSkinnedRefit(frame, services, instances, solveWillRun);
    }

    // Idle-gated solve. The gate is sampled unconditionally (see
    // m_CameraIdleGate's doc); only the hold is conditional. Placed HERE, not
    // at the top of the tick: everything above is latched or self-gated
    // correctness work — grid allocation, the NeedsClear flag, the material-map
    // atlas blit, the epoch-gated TLAS rebuild, the software lane's own
    // scene-rebuild cycle — and holding those would let a volume enabled
    // mid-motion come back with a stale or unbuilt scene. Everything BELOW is
    // solve: classify, trace, blend and upload for both cascades plus both
    // reflection lobes, exactly the set the reference holds (js/gi_probes.js:
    // `if (moving && !continuous) return;` returns ahead of its whole solve
    // list, upload included). Nothing is drained — in-flight GPU work finishes
    // on its own — and NeedsClear/ProbeCursor stay latched, so the first
    // resting tick clears and resumes where it left off. One policy for both
    // lanes: the hardware and software traces are downstream of this return.
    if (!solveWillRun)
    {
        m_SolveWasHeld = true;
        return;
    }

    // Accepted solve tick: advance the adaptive ray budget on the same frame
    // delta the blend's hysteresis normalization sees. A resume after a hold
    // clamps first (reference: probeBudgetAfterInteraction on the
    // moving->rest transition) — held stretches are never solve pressure.
    if (m_SolveWasHeld)
    {
        m_SolveBudget.OnRestResume();
        m_SolveWasHeld = false;
    }
    m_SolveBudget.Tick(std::max(0.0f, deltaTimeSeconds * 1000.0f));
    // Converged-field throttle: read the newest finished reduction and move
    // the budget's cap BEFORE this tick's windows are sized from it.
    // Once throttled the readback must keep running so a change can release
    // it; before that, a controller already at the patrol floor has nothing
    // for the throttle to take, so the reduction is not dispatched.
    const bool throttleActive = m_Volume.ConvergedSolve == Components::DDGIConvergedSolve::Throttle &&
                                m_Variability.PipelineId.IsValid() &&
                                (m_SolveBudget.IsConvergedThrottled() || m_SolveBudget.ThrottleWouldReduce());
    if (throttleActive)
    {
        ReadbackConvergence();
    }
    else if (m_SolveBudget.IsConvergedThrottled() || m_VariabilityRing.IsInitialized())
    {
        // Switched off (or the kernel is missing): release the cap and forget
        // results in flight, so a later re-enable starts from a fresh read.
        m_SolveBudget.SetConvergedThrottle(false);
        m_VariabilityRing.DropPendings();
    }

    // 6. Pack this frame's lights ONCE from the SAME CPU source
    // LightUploadNode uses, into DDGI's own buffer (see DDGIPackedLight's
    // doc for why this cannot share the forward pass's per-view LightBuffer)
    // — shared by both cascades' trace this tick.
    const auto lights = services.GetWorldLights(m_Volume.WorldId);
    const size_t lightCount = std::min<size_t>(lights.size(), kDDGIMaxLights);

    // Emissive NEE tier 1: an instance flagged MeshRenderer.giEmitter publishes
    // its emissive as a type-3 SPHERE PROXY light, so every probe samples it
    // directly instead of waiting for a trace ray to land on it — the reason a
    // small bright emitter otherwise never converges. Both trace lanes exclude
    // these surfaces from per-hit emissive (GPUInstance flag bit 6 / instance
    // record slot 16), so the energy is delivered exactly once.
    //
    // Emitters are gathered BEFORE the upload is sized, because they share the
    // one buffer and the same kDDGIMaxLights ceiling the NEE loop costs against.
    struct EmitterProxy
    {
        float CenterWS[3];
        float Radius;
        float Radiance[3];
    };
    std::vector<EmitterProxy> emitters;
    if (lightCount < kDDGIMaxLights)
    {
        m_EmitterAreas.Update(*m_MeshRegistry, instances);
        // materialIndex -> scene-linear emissive, built once per tick. Only
        // materials that actually emit are kept, so a scene with no emitters
        // pays one registry walk and nothing else.
        struct EmitterMaterial
        {
            DDGIEmissive Emissive;
            bool DoubleSided = false;
        };
        std::unordered_map<uint32_t, EmitterMaterial> emissiveByMaterial;
        m_Materials->Registry().ForEach(
            [&](const GameEngine::GUID&, const Material& material)
            {
                const uint32_t slot = material.GetGpuSceneMaterialIndex();
                if (slot == ~0u)
                    return;
                float tint[3] = {0.0f, 0.0f, 0.0f};
                material.GetVector(HashStringId("emissive"), tint, 3);
                const float nits = material.GetFloat(HashStringId("emissionLuminance"), 0.0f);
                const DDGIEmissive e = ComputeDDGIEmissive(tint[0], tint[1], tint[2], nits);
                if (e.Color[0] > 0.0f || e.Color[1] > 0.0f || e.Color[2] > 0.0f)
                    emissiveByMaterial.emplace(slot, EmitterMaterial{e, material.IsDoubleSided()});
            });

        for (size_t instanceIndex = 0; instanceIndex < instances.size(); ++instanceIndex)
        {
            const Rendering::GPUInstance& inst = instances[instanceIndex];
            if (lightCount + emitters.size() >= kDDGIMaxLights)
                break;
            if ((inst.flags & Rendering::kInstanceFlagGIEmitter) == 0u)
                continue;
            const auto it = emissiveByMaterial.find(inst.materialIndex);
            if (it == emissiveByMaterial.end())
                continue;  // flagged but not actually emissive — nothing to publish
            if (!(inst.boundingRadius > 0.0f))
                continue;
            EmitterProxy proxy{};
            proxy.CenterWS[0] = inst.boundingCenter.x;
            proxy.CenterWS[1] = inst.boundingCenter.y;
            proxy.CenterWS[2] = inst.boundingCenter.z;
            proxy.Radius = inst.boundingRadius;
            // Weight the proxy by the mesh's mean projected area (A/4, or A/2
            // when both faces emit), not the bounding sphere's area: thin
            // panels and strips can have a large sphere while emitting very
            // little power. Keep the sphere for visibility and for GPU-only
            // meshes without CPU geometry.
            const std::optional<float> surfaceArea = m_EmitterAreas.GetSurfaceArea(instanceIndex);
            const float projectedArea = surfaceArea
                ? ComputeDDGIEmitterProjectedArea(*surfaceArea, it->second.DoubleSided)
                : GE_PI_F * proxy.Radius * proxy.Radius;
            if (!(projectedArea > 0.0f))
                continue;
            const DDGIEmissive& emissive = it->second.Emissive;
            proxy.Radiance[0] = emissive.Color[0] * projectedArea;
            proxy.Radiance[1] = emissive.Color[1] * projectedArea;
            proxy.Radiance[2] = emissive.Color[2] * projectedArea;
            emitters.push_back(proxy);
        }
    }

    if (const char* v = std::getenv("GE_DDGI_EMITTER_TRACE"); v && v[0] != '\0' && v[0] != '0')
    {
        uint32_t flagged = 0;
        for (const Rendering::GPUInstance& inst : instances)
            if ((inst.flags & Rendering::kInstanceFlagGIEmitter) != 0u)
                ++flagged;
        Logger::Log::Info("[DDGIEmitter] instances={} flagged={} published={} analyticLights={}",
                          instances.size(), flagged, emitters.size(), lightCount);
    }

    const size_t totalLights = lightCount + emitters.size();
    const size_t lightBytes = sizeof(uint32_t) * 4 + totalLights * sizeof(DDGIPackedLight);
    // 256, not 16: this buffer binds as a compute ReadOnlyStorage, and WebGPU requires a
    // 256-byte storage-buffer offset alignment (looser on Vulkan). The default AllocUpload
    // alignment is already 256; the explicit 16 undercut it and failed CreateBindGroup on web.
    auto lightAlloc = frame.AllocUpload(lightBytes, 256);
    if (!lightAlloc.Ptr)
        return;
    std::memset(lightAlloc.Ptr, 0, sizeof(uint32_t) * 4);
    *reinterpret_cast<uint32_t*>(lightAlloc.Ptr) = static_cast<uint32_t>(totalLights);
    auto* outLights = reinterpret_cast<DDGIPackedLight*>(static_cast<uint8_t*>(lightAlloc.Ptr) + 16);
    for (size_t li = 0; li < lightCount; ++li)
    {
        const auto& in = lights[li];
        DDGIPackedLight p{};
        p.type = static_cast<uint32_t>(in.type);
        p.castsShadows = in.castsShadows;
        p.areaShape = static_cast<uint32_t>(in.areaShape);
        p.castsLight = in.castsLight;
        const float inner = std::clamp(in.innerAngle, 0.0f, 3.1415926f);
        const float outer = std::clamp(in.outerAngle, 0.0f, 3.1415926f);
        p.spotCosInner = std::cos(std::min(inner, outer));
        p.spotCosOuter = std::cos(std::max(inner, outer));
        p.positionWS[0] = in.positionWS[0];
        p.positionWS[1] = in.positionWS[1];
        p.positionWS[2] = in.positionWS[2];
        p.range = in.range;
        p.directionWS[0] = in.directionWS[0];
        p.directionWS[1] = in.directionWS[1];
        p.directionWS[2] = in.directionWS[2];
        p.areaRightWS[0] = in.rightWS[0];
        p.areaRightWS[1] = in.rightWS[1];
        p.areaRightWS[2] = in.rightWS[2];
        p.areaUpWS[0] = in.upWS[0];
        p.areaUpWS[1] = in.upWS[1];
        p.areaUpWS[2] = in.upWS[2];
        p.intensity = in.intensity;
        p.color[0] = in.color[0];
        p.color[1] = in.color[1];
        p.color[2] = in.color[2];
        p.areaWidth = std::max(in.areaWidth, 0.001f);
        p.areaHeight = std::max(in.areaHeight, 0.001f);
        p.areaRadius = std::max(in.areaRadius, 0.001f);
        p.falloffMode = static_cast<float>(static_cast<uint32_t>(in.falloff));
        p.decay = std::max(in.decay, 0.0f);
        p.fogContribution = std::max(in.fogContribution, 0.0f);
        p.fogDensityBoost = std::max(in.fogDensityBoost, 0.0f);
        p.fogOriginFade = std::clamp(in.fogOriginFade, 0.0f, 1.0f);
        p.fogAnisotropy = std::clamp(in.fogAnisotropy, -0.95f, 0.95f);
        outLights[li] = p;
    }

    // Sphere proxies land AFTER the analytic lights, so an existing scene's
    // light indices are untouched.
    for (size_t ei = 0; ei < emitters.size(); ++ei)
    {
        const EmitterProxy& e = emitters[ei];
        DDGIPackedLight p{};
        p.type = 3u;  // sphere proxy — see ddgi_hit_shade.glsl's NEE loop
        p.castsShadows = 1u;
        p.castsLight = 1u;
        p.positionWS[0] = e.CenterWS[0];
        p.positionWS[1] = e.CenterWS[1];
        p.positionWS[2] = e.CenterWS[2];
        // Range bounds the NEE loop's distance test. An emitter is a local
        // light; without a finite range every probe in the volume would shade
        // against it regardless of distance.
        p.range = std::max(e.Radius * kDDGIEmitterRangeScale, kDDGIEmitterMinRange);
        p.intensity = 1.0f;  // radiance rides in color; the proxy carries no separate scale
        p.color[0] = e.Radiance[0];
        p.color[1] = e.Radiance[1];
        p.color[2] = e.Radiance[2];
        p.areaRadius = e.Radius;  // source radius: the solid-angle term
        p.decay = 2.0f;
        outLights[lightCount + ei] = p;
    }

    m_LastTickTimeMs = m_LastTickTimeMs < 0.0f ? (1000.0f / 60.0f)
                                               : std::max(0.0f, deltaTimeSeconds * 1000.0f);

    SharedTickInputs shared;
    shared.LightBuffer = lightAlloc.Buffer;
    shared.LightOffset = lightAlloc.Offset;
    shared.LightBytes = lightBytes;
    // totalLights, NOT lightCount: the trace kernels bound their NEE loop by
    // this value (uDispatch0.w), so the published emitter proxies live past the
    // analytic lights and would never be visited if this stopped at the
    // analytic count. The buffer header carries the same total.
    shared.LightCount = static_cast<uint32_t>(totalLights);
    const auto materialParams = services.Materials().PackedMaterialParams();
    shared.MaterialParamsBuffer = materialParams.Buffer;
    shared.MaterialParamsOffset = materialParams.Offset;
    shared.MaterialParamsSize = materialParams.Size;
    auto* ibl = services.GetFeature<ImageBasedLightingFeature>();
    shared.SkyIrradiance = ibl ? ibl->GetIrradianceCube() : Rendering::TextureHandle{};
    shared.SkySampler = ibl ? ibl->GetCubeSampler() : Rendering::SamplerHandle{};
    shared.SkyIntensityScale =
        m_Volume.SkyIntensity * (ibl ? std::max(ibl->GetIblIntensity(), 0.0f) : 1.0f);
    if (m_MapAtlas)
    {
        shared.MapAtlas = m_MapAtlas->GetTexture();
        shared.MapSampler = m_MapAtlas->GetSampler();
        shared.MapTable = m_MapAtlas->GetTableBuffer();
        shared.MapTableBytes = m_MapAtlas->GetTableBytes();
    }
    // Gated: rotation 0 every tick — the fixed ray set, so a converged field
    // is a fixed point (the reference's default regime; its Monte Carlo mode
    // is the only one that advances the basis). m_FrameClock itself always
    // advances — it also drives the retired-buffer margin.
    shared.RayEpoch = m_Volume.JitterMode == Components::DDGIJitterMode::MonteCarlo ? m_FrameClock : 0;
    shared.RaysPerTickBudget = m_SolveBudget.RaysPerTick();
    shared.SwNodesRG = m_TickSwNodesRG;
    shared.SwPackedSceneRG = swPackedSceneRG;
    shared.SwVertexDataRG = m_TickSwVertexDataRG;
    if (m_TickSkinnedRowMapBuffer.IsValid() && m_TickSkinnedGeomRowsBuffer.IsValid())
    {
        shared.SkinnedRowMapBuffer = m_TickSkinnedRowMapBuffer;
        shared.SkinnedRowMapOffset = m_TickSkinnedRowMapOffset;
        shared.SkinnedRowMapBytes = m_TickSkinnedRowMapBytes;
        shared.SkinnedGeomRowsBuffer = m_TickSkinnedGeomRowsBuffer;
        shared.SkinnedGeomRowsOffset = m_TickSkinnedGeomRowsOffset;
        shared.SkinnedGeomRowsBytes = m_TickSkinnedGeomRowsBytes;
    }
    else
    {
        // No skinned rows this tick: bind a 1-element no-override map and one
        // zero row so the kernel's declared bindings stay valid.
        auto dummyMap = frame.AllocUpload(sizeof(uint32_t), 256); // storage binding: 256-align for WebGPU
        auto dummyRow = frame.AllocUpload(40, 256); // storage binding: 256-align for WebGPU
        if (dummyMap.Valid() && dummyRow.Valid())
        {
            *static_cast<uint32_t*>(dummyMap.Ptr) = 0xFFFFFFFFu;
            std::memset(dummyRow.Ptr, 0, 40);
            shared.SkinnedRowMapBuffer = dummyMap.Buffer;
            shared.SkinnedRowMapOffset = dummyMap.Offset;
            shared.SkinnedRowMapBytes = sizeof(uint32_t);
            shared.SkinnedGeomRowsBuffer = dummyRow.Buffer;
            shared.SkinnedGeomRowsOffset = dummyRow.Offset;
            shared.SkinnedGeomRowsBytes = 40;
        }
    }
    shared.TickTimeMs = m_LastTickTimeMs;
    shared.UseSoftwareLane = !useHardware;
    if (useHardware)
    {
        shared.GpuInstanceBuffer = m_GpuScene->GetInstanceBuffer();
        shared.GpuInstanceBytes = static_cast<uint64_t>(instances.size()) * sizeof(Rendering::GPUInstance);
        shared.MeshGeomBuffer = m_SceneAS->PublishMeshGeometry();
        shared.MeshGeomBytes = m_SceneAS->GetMeshGeometryCapacityBytes();
    }
    else
    {
        const DDGISceneService::SceneBuffers& sw = m_SceneService->GetSceneBuffers();
        shared.SwPackedScene = sw.PackedScene;
        shared.SwNodes = sw.Nodes;
        shared.SwTriangleIndices = sw.TriangleIndices;
        shared.SwTriangleMaterials = sw.TriangleMaterials;
        shared.SwVertexData = sw.VertexData;
        shared.SwPackedSceneBytes = sw.PackedSceneBytes;
        shared.SwNodesBytes = sw.NodesBytes;
        shared.SwTriangleIndicesBytes = sw.TriangleIndicesBytes;
        shared.SwTriangleMaterialsBytes = sw.TriangleMaterialsBytes;
        shared.SwVertexDataBytes = sw.VertexDataBytes;
        shared.SwTlasNodeCount = sw.TlasNodeCount;
        shared.SwInstanceBase = sw.InstanceBase;
        shared.SwTlasBase = sw.TlasBase;
    }

    uint32_t c0ProbeBase = 0;
    uint32_t c0ProbesThisTick = 0;
    DeclareCascadeGridPasses(frame, m_C0, m_Volume.GridMinWS, m_Volume.GridSizeWS, shared, kC0DebugNames,
                             &c0ProbeBase, &c0ProbesThisTick);
    uint32_t c1ProbeBase = 0;
    uint32_t c1ProbesThisTick = 0;
    if (m_Volume.EnableFineCascade && m_C1.GpuGridValid)
        DeclareCascadeGridPasses(frame, m_C1, fineMinWS, fineSizeWS, shared, kC1DebugNames, &c1ProbeBase,
                                 &c1ProbesThisTick);

    // Convergence reduction: one slot per tick, both cascades' partial sums
    // side by side, declared after each cascade's blend so it sees this
    // tick's counts. The C1 half stays zero (and its texel count 0) while the
    // fine cascade is off, so the CPU sum skips it.
    if (throttleActive && c0ProbesThisTick > 0)
    {
        if (!m_VariabilityRing.IsInitialized())
        {
            Rendering::BufferDesc slotDesc{};
            slotDesc.size = static_cast<uint64_t>(kVariabilityGroups) * 2u * sizeof(uint32_t);
            slotDesc.usage = static_cast<uint32_t>(Rendering::BufferUsage::Storage);
            slotDesc.memoryUsage = Rendering::BufferMemoryUsage::Readback;
            slotDesc.flags = Rendering::BufferCreateFlags::PersistentlyMapped;
            slotDesc.debugName = "DDGI.Variability.Readback";
            m_VariabilityRing.Init(m_Device, slotDesc, std::max(1u, m_Device->GetFramesInFlight()) + 2u);
        }
        const bool fineReduced = m_Volume.EnableFineCascade && m_C1.GpuGridValid && c1ProbesThisTick > 0;
        VariabilityPayload payload;
        payload.Texels[0] = static_cast<uint32_t>(m_C0.ProbeTotal) * uint32_t(kTile * kTile);
        payload.Texels[1] = fineReduced ? static_cast<uint32_t>(m_C1.ProbeTotal) * uint32_t(kTile * kTile) : 0u;
        const Rendering::BufferHandle slot = m_VariabilityRing.BeginWrite(frame, payload);
        if (slot.IsValid())
        {
            DeclareVariabilityPass(frame, m_C0, slot, 0, kC0DebugNames);
            if (fineReduced)
                DeclareVariabilityPass(frame, m_C1, slot, kVariabilityGroups, kC1DebugNames);
        }
    }

    // Reflection lobes piggyback on each cascade's just-traced ray buffer for
    // the SAME window — must run after that cascade's passes, never before.
    const bool roughKernelsReady = m_RoughBlend.PipelineId.IsValid() && m_RoughUpload.PipelineId.IsValid();
    const bool glossyKernelsReady = m_GlossyBlend.PipelineId.IsValid() && m_GlossyUpload.PipelineId.IsValid();
    if (m_C0Refl.GpuValid && roughKernelsReady && (!m_C0Refl.GlossyAllocated || glossyKernelsReady) &&
        c0ProbesThisTick > 0)
    {
        DeclareReflectionPasses(frame, m_C0, m_C0Refl, m_Volume.GridMinWS, m_Volume.GridSizeWS, c0ProbeBase,
                                c0ProbesThisTick, shared, kC0DebugNames);
    }
    if (m_C1Refl.GpuValid && roughKernelsReady && (!m_C1Refl.GlossyAllocated || glossyKernelsReady) &&
        c1ProbesThisTick > 0)
    {
        DeclareReflectionPasses(frame, m_C1, m_C1Refl, fineMinWS, fineSizeWS, c1ProbeBase, c1ProbesThisTick,
                                shared, kC1DebugNames);
    }
}

void DDGIProbeFeature::DeclareVariabilityPass(Rendering::RenderGraph::RGFrame& frame, CascadeGrid& grid,
                                              Rendering::BufferHandle slot, uint32_t slotOffset,
                                              const CascadeDebugNames& names)
{
    namespace RG = Rendering::RenderGraph;
    struct VariabilityParamsUBO
    {
        uint32_t Params0[4];  // x = texel count, y = first output slot
    };
    static_assert(sizeof(VariabilityParamsUBO) == 16,
                  "must match ddgi_variability.comp's DDGIVariabilityParams");
    auto ub = frame.AllocUpload<VariabilityParamsUBO>();
    if (!ub.Valid())
        return;
    const uint32_t texelCount = static_cast<uint32_t>(grid.ProbeTotal) * uint32_t(kTile * kTile);
    ub.Ptr->Params0[0] = texelCount;
    ub.Ptr->Params0[1] = slotOffset;
    ub.Ptr->Params0[2] = 0;
    ub.Ptr->Params0[3] = 0;

    // Same physical handle DeclareCascadeGridPasses imported, so the graph
    // dedups to one resource and orders this read after Blend's write.
    const uint64_t temporalBytes = TemporalStateBytes(grid.ProbeTotal);
    const RG::RGBuffer temporalRG = frame.ImportExternalBuffer(names.TemporalState, grid.TemporalState, temporalBytes);
    const Rendering::BufferHandle temporalBuf = grid.TemporalState;
    const uint64_t slotBytes = m_VariabilityRing.SlotBytes();
    const Rendering::ComputePipelineId pipeId = m_Variability.PipelineId;
    const Rendering::DescriptorSetLayoutDesc layout = m_Variability.Set0Layout;
    const Rendering::ShaderMeta* meta = m_Variability.Meta.get();
    frame.AddComputePass(
        names.Variability, Rendering::PassPhase::kDefault,
        [temporalRG](RG::RGPassBuilder& p)
        {
            p.PreventCulling();
            p.Read(temporalRG, RG::RGBufferRead::Storage);
        },
        [ub, temporalBuf, temporalBytes, slot, slotBytes, pipeId, layout, meta](RG::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl || !meta)
                return;
            Rendering::DescriptorSetDesc dsDesc{};
            dsDesc.layout = layout;
            dsDesc.transient = true;
            dsDesc.debugName = "DDGI.Variability.Set0";
            auto ds = dev->CreateDescriptorSet(dsDesc);
            Rendering::NamedDescriptorWriter wd(dev, ds, *meta, 0);
            wd.AddUniformBuffer("DDGIParams", ub.Buffer, ub.Offset, sizeof(VariabilityParamsUBO));
            wd.AddStorageBuffer("DDGITemporalStateRO", temporalBuf, 0, temporalBytes);
            wd.AddStorageBuffer("DDGIVariabilityOut", slot, 0, slotBytes);
            wd.Flush();
            Rendering::PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(pipeId);
            if (!pipe.IsValid())
                return;
            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            cl->Dispatch(kVariabilityGroups, 1, 1);
        });
}

void DDGIProbeFeature::ReadbackConvergence()
{
    VariabilityPayload payload;
    const void* mapped = m_VariabilityRing.MapNewestReady(m_Device, &payload);
    if (!mapped)
        return;
    const auto* sums = static_cast<const uint32_t*>(mapped);
    uint64_t total = 0;
    uint64_t texels = 0;
    for (uint32_t cascade = 0; cascade < 2; ++cascade)
    {
        if (payload.Texels[cascade] == 0)
            continue;
        for (uint32_t g = 0; g < kVariabilityGroups; ++g)
            total += sums[cascade * kVariabilityGroups + g];
        texels += payload.Texels[cascade];
    }
    m_VariabilityRing.Unmap(m_Device);
    if (texels == 0)
        return;
    // Mean steady-tick count against the blend kernel's history cap
    // (GE_DDGI_TEMPORAL_MAX_HISTORY = 64): 1 means every texel has sat at
    // the cap, so the field is a fixed point of the solve.
    constexpr double kMaxHistory = 64.0;
    const double converged = static_cast<double>(total) / (static_cast<double>(texels) * kMaxHistory);
    const bool wasThrottled = m_SolveBudget.IsConvergedThrottled();
    if (converged >= kConvergedEnter)
        m_SolveBudget.SetConvergedThrottle(true);
    else if (converged < kConvergedExit)
        m_SolveBudget.SetConvergedThrottle(false);
    // One line per transition: the only observable of the throttle state a
    // measurement can be anchored to (the ray budget itself is not exported).
    if (m_SolveBudget.IsConvergedThrottled() != wasThrottled)
        Logger::Log::Info("DDGI: converged throttle {} (mean history {:.3f}, budget {} rays/tick)",
                          m_SolveBudget.IsConvergedThrottled() ? "engaged" : "released", converged,
                          m_SolveBudget.RaysPerTick());
}

void DDGIProbeFeature::OnFrameSubmittedRG(Rendering::RenderGraph::RGFrame& frame,
                                          const Rendering::IDevice::GpuSyncToken& token)
{
    m_VariabilityRing.OnFrameSubmitted(frame, token);
}

void DDGIProbeFeature::OnFrameStreamRetiredRG(Rendering::RenderGraph::RGFrame& frame)
{
    m_VariabilityRing.OnFrameStreamRetired(frame);
}

}  // namespace GameEngine::Engine::Renderer
