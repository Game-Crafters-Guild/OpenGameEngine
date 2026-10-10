#include "CBTTerrainECS/CBTRenderFeature.h"

#include "CBTTerrainECS/CBTTerrainMaterial.h"

#include "CBTTerrain/CBTDemandTuning.h"
#include "CBTTerrain/CBTKernelSet.h"
#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrain/CBTNearBias.h"
#include "CBTTerrain/CBTPlanetShading.h"
#include "CBTTerrain/CBTResources.h"
#include "CBTTerrain/CBTScreenTarget.h"

#include "Engine/Rendering/DepthDrawRecorder.h"
#include "Engine/Rendering/DrawCommand.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/PipelineVariantCache.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ViewRegistry.h"

#include "Components/Terrain/Terrain.h" // kNoTerrainSeaLevel
#include "Ocean/OceanRenderFeature.h"
#include "Ocean/OceanSeabedVisibility.h"
#include "Terrain/TerrainMaterialRecord.h"
#include "TerrainECS/TerrainHeightPageFeature.h"
#include "TerrainECS/TerrainRenderFeature.h"

#include "Rendering/CameraTypes.h" // CameraData
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/MaterialAlphaMode.h"
#include "Rendering/Materials/MaterialDocument.h"

#include "AssetCore/GUID.h"
#include "Core/Application.h" // PathUtils::GetInstallAssetsRoot
#include "Core/CpuProfiler.h"
#include "Logger/Logger.h"
#include "Types/Types.h" // HashStringId

#include <algorithm> // std::min/max (sculpt region accumulation, water margin)
#include <type_traits>
#include <cmath>     // std::sqrt (planet surface-height sampling)
#include <cstdlib>   // getenv (GE_CBT_DESC_DEBUG)
#include <cstring>   // memcpy
#include <filesystem>
#include <system_error>
#include <vector>

namespace GameEngine::CBTTerrainECS
{

using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::CBTTerrain;

namespace
{
// View-prioritised split metric. Redistributes the fixed bisector pool
// toward the camera when the pool is contended, so the near patch reaches the pixel target
// while the periphery stays coarse. Ships default-ON; A/B on the same build via
// GE_CBT_NEAR_BIAS=0 (force off) / =1 (force on). The near/far radii scale SUB-LINEARLY with
// the camera's altitude above the surface (CBTNearBias.h: nearR = 100 * sqrt(alt)) so the
// full-detail disc shrinks slowly on descent — a mid-distance feature keeps its detail as the
// camera drops toward it instead of coarsening (the #620 zoom-in fix). The in-shader occupancy
// gate makes an unsaturated frame classify exactly as today regardless of the toggle.
constexpr bool kNearBiasDefaultEnabled = true;

bool NearBiasEnabled()
{
    static const bool enabled = [] {
        if (const char* v = std::getenv("GE_CBT_NEAR_BIAS"))
            return std::atoi(v) != 0;
        return kNearBiasDefaultEnabled;
    }();
    return enabled;
}

// Walking-headroom demand shaping (walking-headroom slice — DemandTuning UBO field). Ships
// default-ON; A/B on the same build via GE_CBT_DEMAND_TUNING=0 (force off). See cbt_layout.glsl
// demandTuning for the field semantics; the editor defaults live in CBTDemandTuning.h, shared
// with the headless probes that run the production demand model.
constexpr bool kDemandTuningDefaultEnabled = true;

bool DemandTuningEnabled()
{
    static const bool enabled = [] {
        if (const char* v = std::getenv("GE_CBT_DEMAND_TUNING"))
            return std::atoi(v) != 0;
        return kDemandTuningDefaultEnabled;
    }();
    return enabled;
}

// Screen-area priority ordering (round-8c look-back — PriorityParams UBO field). Orders the fixed
// pool's split budget by ON-SCREEN facet area under saturation so the largest (most jarring) facets
// refine first: the fix for "look away from the flatten plateau, look back, it rebuilds slowly with
// artifacts". Ships default-ON; A/B on the same build via GE_CBT_PRIORITY=0 (force off, the pre-slice
// unordered metric). See cbt_layout.glsl priorityParams for the field semantics; the editor
// defaults live in CBTDemandTuning.h, shared with the headless probes that run the production
// demand model (the headless look-back oracle sets its own values to characterize each knob).
constexpr bool kPriorityDefaultEnabled = true;

bool PriorityEnabled()
{
    static const bool enabled = [] {
        if (const char* v = std::getenv("GE_CBT_PRIORITY"))
            return std::atoi(v) != 0;
        return kPriorityDefaultEnabled;
    }();
    return enabled;
}

// Water plane (the WaterPlane UBO field; the term itself is in Kernel_Classify). Keeps the seabed
// a camera above the water cannot see coarse — split and merge thresholds scaled by
// kSubmergedCoarsen — instead of refining it to TargetPixelError. The
// plane is the Ocean module's sea level when an ocean is drawn, else the Terrain component's
// authored SeaLevel (a water mesh the engine cannot otherwise recognise). Ships default-ON; A/B
// on the same build via GE_CBT_WATER_PLANE=0 (force off).
constexpr bool kWaterPlaneDefaultEnabled = true;
// Threshold multiplier for a submerged bisector: 8x the pixel target, 1/64 the triangles. Hidden
// seabed only has to (a) sample the heightfield often enough that a rise inside one facet is
// found — at 8 x TPE 8 a facet is ~64 px across — and (b) give the water's depth fog a depth
// buffer, which is smooth at that size. The far-field near-bias coarsens VISIBLE ground by up to
// kNearBiasMaxCoarsen (6x); ground nobody sees can afford more.
constexpr float kSubmergedCoarsen = 8.0f;

// Content-aware split (the WaterPlane UBO field's w; the term itself is in Kernel_Classify). A planar
// facet splits only while its height range projects above this many pixels: flat ground stops
// refining where the screen metric alone would carry it to the depth cap, and relief keeps its
// detail. GE_CBT_FLAT_PX overrides it for a measurement arm on the same build (0 = off); not a
// setting.
constexpr float kContentAwareSplitPx = 1.0f;

float ContentAwareSplitPx()
{
    static const float px = [] {
        if (const char* v = std::getenv("GE_CBT_FLAT_PX"))
            return std::max(0.0f, static_cast<float>(std::atof(v)));
        return kContentAwareSplitPx;
    }();
    return px;
}
// Margin below an authored (opaque) water surface within which the seabed still refines at full
// detail: the height slack of one coarsened facet next to the shoreline, so a rise that breaks the
// surface inside a facet whose corners all sit just under it is still sampled. A ~7 m facet
// (8 x TPE 8 at 100 m, 1080p) on a seabed sloping <= 15 degrees varies ~1.9 m across.
constexpr float kOpaqueWaterMarginM = 2.0f;

bool WaterPlaneEnabled()
{
    static const bool enabled = [] {
        if (const char* v = std::getenv("GE_CBT_WATER_PLANE"))
            return std::atoi(v) != 0;
        return kWaterPlaneDefaultEnabled;
    }();
    return enabled;
}

// The ocean's margin is how deep its seabed stays visible from above (Ocean's
// OceanSeabedVisibleDepthM: the depth fog's density and end distance, and the shallow clarity
// window, which keeps the shallows clear over its first meters of view ray), beyond which the
// seabed reads as deep water whatever its shape. Never below the opaque margin (the shoreline
// slack stays).
float OceanVisibleDepthM(const Ocean::OceanParamsGPU& params)
{
    return std::max(Ocean::OceanSeabedVisibleDepthM(params), kOpaqueWaterMarginM);
}

struct ResolvedWaterPlane
{
    bool Present = false;
    float SurfaceY = 0.0f; // world Y of the calm water surface
    float MarginM = 0.0f;  // seabed within this depth of it still refines at full detail
};

// The Ocean module's surface, when one is drawn, IS the water the terrain sits under: its sea
// level (preset, origin shift and all) is what the surface shades at, so the classifier reads the
// same value. Without an ocean, the Terrain component's authored SeaLevel names a water mesh the
// engine cannot otherwise see, under which nothing shows through.
ResolvedWaterPlane ResolveWaterPlane(RenderServices& rs, float terrainSeaLevel)
{
    if (!WaterPlaneEnabled())
        return {};
    if (const auto* ocean = rs.GetFeature<Ocean::OceanRenderFeature>(); ocean && ocean->HasOcean())
    {
        const Ocean::OceanParamsGPU params = ocean->GetParams();
        return {true, params.SeaLevel, OceanVisibleDepthM(params)};
    }
    if (terrainSeaLevel > Components::kNoTerrainSeaLevel)
        return {true, terrainSeaLevel, kOpaqueWaterMarginM};
    return {};
}

// The staged CBT shader dir (the kernel program(s) + the graphics shaders + cbt_layout.glsl)
// under the install assets root; empty when @p probeName is not staged there. The probe asks
// CBTKernelSet what its own loader will open, so the two cannot disagree about which build's
// program shape is on disk.
std::filesystem::path FindCBTShaderDir(const std::string& probeName)
{
    const std::filesystem::path shaderDir = PathUtils::GetInstallAssetsRoot() / "Shaders" / "CBT";
    std::error_code ec;
    if (!std::filesystem::exists(shaderDir / probeName, ec))
        return {};
    return shaderDir;
}
// Appends the raw bytes of a padding-free trivially copyable value to the update-input key.
template <class T>
void AppendInputBytes(std::vector<uint8_t>& out, const T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}
} // namespace

CBTRenderFeature::~CBTRenderFeature()
{
    m_ActivityReadback.Shutdown();
    m_Instance.Shutdown();
    m_KernelSet.Shutdown();
}

bool CBTRenderFeature::EnsureInitialized(IDevice& device, RenderServices& rs)
{
    if (m_Initialized)
        return true;
    if (m_InitAttempted)
        return false; // one attempt; don't re-run failing GPU bring-up every frame
    m_Device = &device;
    m_Services = &rs;

    // The kernel pipelines build off this thread (CBTKernelSet::Poll); until all of
    // them have landed the feature declares nothing and the rest of the bring-up waits.
    if (!m_KernelSet.IsRequested() && !RequestKernels(device))
    {
        m_InitAttempted = true;
        return false;
    }
    const CBTTerrain::CBTKernelSetState kernels = m_KernelSet.Poll();
    if (kernels == CBTTerrain::CBTKernelSetState::Pending)
        return false;
    m_InitAttempted = true;
    if (kernels == CBTTerrain::CBTKernelSetState::Failed)
    {
        Logger::Log::Error("CBTRenderFeature: the CBT compute kernels failed to build");
        return false;
    }
    if (!m_Instance.Initialize(device, m_KernelSet))
    {
        Logger::Log::Error("CBTRenderFeature: CBTInstance initialize failed");
        return false;
    }
    // Without its readback the gate never sees a quiet update, so the update simply always runs.
    if (!m_ActivityReadback.Initialize(device))
        Logger::Log::Warning("CBTRenderFeature: activity readback allocation failed; the CBT "
                             "update runs every frame");
    // Drop the whole-pool debug Validate kernel from the shipping update: it produces nothing the
    // draw consumes and is the one per-frame pool-linear cost the S3 indirect conversion cannot
    // fold into the live compact stream (its zombie check must scan the free slots). Tests keep it.
    // GE_CBT_VALIDATE=1 re-arms it; CBTRenderNode reads the Validation counters back per frame.
    m_Instance.SetValidateEachUpdate(std::getenv("GE_CBT_VALIDATE") != nullptr);
    if (!m_Instance.InitializeRoots(m_DomainConfig.DomainMode))
    {
        Logger::Log::Error("CBTRenderFeature: CBTInstance root init failed");
        return false;
    }
    m_TreeSeed.Seeded(m_DomainConfig.DomainMode, m_Classify.TargetDepth);
    // maxSubdiv is the effective decode cap (numSubdiv = depth - baseDepth), and it follows the
    // kernel arm: a device without shaderInt64 runs the narrow-heap kernels, whose u32 heap ID
    // caps subdivision below the int64 arm's exact-N cap.
    Logger::Log::Info("GE_CBT.Planet domain={} roots={} baseDepth={} maxSubdiv={} (seeded)",
                      m_TreeSeed.Domain() == CBTTerrain::kDomainSpherical ? "spherical" : "planar",
                      m_Instance.GetRootCount(), m_Instance.GetBaseDepth(),
                      m_KernelSet.IsNarrowHeap() ? CBTTerrain::kHeap32DecodeSubdiv
                                                 : CBTTerrain::kMaxDecodeSubdiv);

    // Register the material now (its compile runs on a worker); a failed registration
    // is retried in BuildDrawCommand.
    EnsureMaterial(rs);

    m_Initialized = true;
    Logger::Log::Info("CBTRenderFeature: initialized (kernels + {} MiB instance)",
                      m_Instance.GetResources().GetPersistentByteSize() / (1024u * 1024u));
    return true;
}

bool CBTRenderFeature::RequestKernels(IDevice& device)
{
    // A device without 64-bit integers runs the narrow-heap (GE_CBT_HEAP32) kernels;
    // CBTKernelSet picks the arm, and this probe asks it for the file it will open.
    const std::string kernelProgram = CBTTerrain::CBTKernelSet::KernelProgramFileName(
        device.PreferredShaderSource(), !device.GetCapabilities().supportsShaderInt64, 0);
    const std::filesystem::path shaderDir = FindCBTShaderDir(kernelProgram);
    if (shaderDir.empty())
    {
        Logger::Log::Error("CBTRenderFeature: {} not found under Assets/Shaders/CBT",
                           kernelProgram);
        return false;
    }
    if (!m_KernelSet.Initialize(device, shaderDir))
    {
        Logger::Log::Error("CBTRenderFeature: failed to load CBT compute kernels from {}",
                           shaderDir.string());
        return false;
    }
    return true;
}

void CBTRenderFeature::OnDeviceRebuilt(IDevice* device)
{
    if (!m_InitAttempted)
        return; // never brought up on the old device — nothing of ours is dead

    // Forget the dead GPU state. The rebuild teardown already destroyed the kernel
    // pipelines and every instance buffer/texture/sampler, so this must NOT destroy
    // anything itself; it only drops handles that still read IsValid(). KernelSet's
    // Shutdown is already a pure reference-drop (its pipelines are owned by the device
    // pipeline cache), so it is the right call here — CBTResources needs the dedicated
    // forget path because its Shutdown destroys.
    m_Instance.ReprovisionAfterDeviceRebuild();
    m_KernelSet.Shutdown();
    m_ActivityReadback.ForgetAfterDeviceRebuild();
    m_RestGate.Reset();
    m_UpdateAtRest.store(false, std::memory_order_relaxed);

    // Re-arm the lazy bring-up rather than re-creating inline: EnsureInitialized needs a
    // RenderServices (for the material) that this hook is not handed, and it allocates + submits
    // its GPU state, which does not belong inside the re-provision window. The next
    // CBTRenderNode declare re-creates the pipelines and buffers on the render thread by
    // the ordinary path. Clearing m_InitAttempted is deliberate: the "one attempt" memo
    // was about the OLD device, and the rebuilt one deserves a fresh try.
    m_Device = device;
    m_Initialized = false;
    m_InitAttempted = false;

    // Derived caches keyed on the dead resources.
    m_DrawBuffers.clear();  // rebuilt from live handles by BuildDrawCommand
    m_BoundHeightmap = {};
    m_TerrainShadowSource = {};
    m_TerrainShadowBake.OnDeviceRebuilt();
    for (uint32_t i = 0; i < CBTTerrain::kCBTFrameParamsRing; ++i)
    {
        m_AtlasUploadedTableVersion[i] = 0u;
        m_AtlasBoundTexture[i] = {};
        m_PageUploadedTableVersion[i] = ~0ull;
    }
    // The sculpt/atlas rings come back zeroed, so the upload gates must forget what they
    // believe each slot holds or an edited planet would never re-upload its edits.
    m_SphereSculptUploadGate.Reset();
    // Re-sync (not act) on the terrain retire generation: the terrain feature is being
    // re-provisioned in this same sweep, and a stale compare here would fire a spurious
    // whole-ring rebind on a ring that is about to be rebuilt anyway.
    m_LastTerrainRetireGeneration = 0;
    m_TerrainRetireGenerationSynced = false;
    // The claim key holds a raw graph pointer from the dead frame; a new graph allocated
    // at the same address would otherwise look like it had already claimed this frame.
    m_UpdateGraph = nullptr;
    m_UpdateRgFrame = ~0ull;
    // Every cached bisector corner was evaluated against textures that no longer exist.
    m_ForceVertexEval.store(true, std::memory_order_relaxed);

    // m_Material is a non-owning pointer into the RenderServices material registry, which
    // survives the rebuild and re-provisions its own GPU state; leaving it (and
    // m_MaterialAttempted) alone avoids a redundant variant recompile.
    Logger::Log::Info("CBTRenderFeature: device rebuild — dropped kernel pipelines + instance "
                      "resources; bring-up re-armed for the next declare");
}

void CBTRenderFeature::EnsureSeeded(uint32_t frameCounter)
{
    // InitializeRoots retires prior graphics users before a live re-seed and descriptor
    // rebind. The domain-independent identity index and draw-count buffers stay settled.
    if (!m_Initialized)
        return;
    const CBTTreeRestart reason =
        m_TreeSeed.RestartReason(m_DomainConfig.DomainMode, m_Classify.TargetDepth, frameCounter);
    if (reason == CBTTreeRestart::None)
        return;
    if (!m_Instance.InitializeRoots(m_DomainConfig.DomainMode))
    {
        Logger::Log::Error("CBTRenderFeature: tree re-seed failed ({})", ToString(reason));
        return;
    }
    m_Instance.RefineUniform(std::min(m_Classify.TargetDepth, CBTTreeSeed::kRestartSeedDepth),
                             frameCounter);
    m_TreeSeed.Seeded(m_DomainConfig.DomainMode, m_Classify.TargetDepth);
    // Defensive: a restart across domains re-creates the sculpt rings zeroed (CBTInstance::
    // InitializeRoots). The store's version is monotonic and a domain round trip clears the store, so
    // its next version already differs from every slot's record; the reset keeps the re-upload from
    // depending on that.
    m_SphereSculptUploadGate.Reset();
    m_ForceVertexEval.store(true, std::memory_order_relaxed); // re-seed staled every corner
    Logger::Log::Info("GE_CBT.Planet domain={} roots={} baseDepth={} maxSubdiv={} (re-seeded: {})",
                      m_TreeSeed.Domain() == CBTTerrain::kDomainSpherical ? "spherical" : "planar",
                      m_Instance.GetRootCount(), m_Instance.GetBaseDepth(),
                      m_KernelSet.IsNarrowHeap() ? CBTTerrain::kHeap32DecodeSubdiv
                                                 : CBTTerrain::kMaxDecodeSubdiv,
                      ToString(reason));
}

void CBTRenderFeature::ConsumeTerrainRetire(uint64_t retireGeneration)
{
    if (!m_Initialized)
        return;
    // First sight: adopt the current generation without acting. A re-provision (or a terrain
    // teardown) that ran before CBT initialized must not trigger a rebind of an already-clean ring.
    if (!m_TerrainRetireGenerationSynced)
    {
        m_LastTerrainRetireGeneration = retireGeneration;
        m_TerrainRetireGenerationSynced = true;
        return;
    }
    if (retireGeneration == m_LastTerrainRetireGeneration)
        return;
    m_LastTerrainRetireGeneration = retireGeneration;

    // A terrain texture set was retired this frame. Drop the retired heightmap from EVERY ring
    // element now (RefreshTerrainSources retires the submitted graphics work first), so no
    // descriptor outlives the image once the terrain feature's quarantine frees it. BuildFrameParams
    // re-binds this frame's slot to the live source (the flat default during the re-provision
    // withhold, then the new heightmap) immediately after.
    if (m_Instance.RefreshTerrainSources())
    {
        // The recreated heightmap staled every cached bisector corner (gVertex was evaluated from
        // the old texture at the old resolution); force a full-pool VertexEval on the next update.
        m_ForceVertexEval.store(true, std::memory_order_relaxed);
        Logger::Log::Info("CBT.HeightSource re-provision refresh: dropped retired terrain views from "
                          "the height/atlas/coarse rings (gen={})",
                          m_Instance.GetTerrainSourceGeneration());
    }
    // The tree grew on the retired terrain: EnsureSeeded restarts it from its roots (CBTTreeSeed
    // decides when).
    m_TreeSeed.NoteTerrainRetired();
}

bool CBTRenderFeature::EnsureMaterial(RenderServices& rs)
{
    if (m_Material)
        return true;
    if (m_MaterialAttempted)
        return false;
    m_MaterialAttempted = true;

    auto& registry = rs.Materials().Registry();
    if ((m_Material = registry.Find(CBTTerrainMaterialGuid())) != nullptr)
        return true;

    // Document and keywords come from the shared builder so this registration and
    // MaterialVariantCook's row cannot drift: the variant cache is keyed on the
    // composed source, so a divergence is a cooked program the runtime never asks for.
    //
    // The base compile runs on a worker: on a cold shader cache it takes seconds, and
    // this is reached from the render-graph declaration on the main thread. The terrain
    // is not drawn until the pipeline publishes (BuildDrawCommand).
    m_Material = rs.Materials().RegisterMaterialFromDocument(
        CBTTerrainMaterialGuid(), BuildCBTTerrainMaterialDocument(),
        CBTTerrainMaterialKeywords(), MaterialSystem::BaseCompileMode::Async);
    if (!m_Material)
    {
        Logger::Log::Error("CBTRenderFeature: failed to register the CBT material");
        return false;
    }
    Logger::Log::Info("CBTRenderFeature: CBT material registered");
    return true;
}

bool CBTRenderFeature::ClaimUpdate(const void* graph, uint64_t rgFrameIndex)
{
    // Single-frame-graph assumption (documented C3 limitation): the CBT sim runs
    // its ping-pong update EXACTLY once per engine frame, in the first render graph
    // that claims it. A second window (its own graph, same rgFrameIndex) loses the
    // claim and CBTRenderNode refuses its draw — so CBT renders in one window only.
    // Deliberately NOT fixed by updating per graph: two updates per engine frame
    // would double-step the ping-pong and corrupt the sim (worse than a missing
    // draw). Multi-window CBT needs a shared-update-once + per-graph-read-edge
    // design (import the same outputs into each window's graph). C4+ ledger item.
    if (m_UpdateRgFrame == rgFrameIndex)
        return false; // the once-per-frame update is already claimed (this or another graph)
    m_UpdateGraph = graph;
    m_UpdateRgFrame = rgFrameIndex;
    return true;
}

bool CBTRenderFeature::WritesDynamicDepth() const
{
    if (!m_Initialized || !m_Active)
        return false;
    return !m_UpdateAtRest.load(std::memory_order_relaxed) ||
           m_ForceVertexEval.load(std::memory_order_relaxed) || m_TreeSeed.IsRestartHeld() ||
           TerrainSourceChanged();
}

bool CBTRenderFeature::TerrainSourceChanged() const
{
    const auto* terrainFeature =
        m_Services ? m_Services->GetFeature<TerrainECS::TerrainRenderFeature>() : nullptr;
    if (!terrainFeature)
        return m_BoundHeightmap.IsValid();
    if (m_TerrainRetireGenerationSynced &&
        terrainFeature->GetTerrainTextureRetireGeneration() != m_LastTerrainRetireGeneration)
        return true;
    const TerrainECS::TerrainRenderFeature::ActiveHeightSource active =
        terrainFeature->GetActiveHeightSource();
    std::span<const uint32_t> pageWords;
    uint64_t pageVersion = 0;
    const Rendering::TextureHandle paged = PagedHeightSource(m_BoundTerrainIndex, pageWords, pageVersion);
    const Rendering::TextureHandle source =
        !active.Present ? Rendering::TextureHandle{}
        : paged.IsValid()
            ? paged
            : BoundHeightSource(active.AtlasBacked, active.AtlasHeightTexture, active.HeightmapTexture);
    return source != m_BoundHeightmap;
}

Rendering::TextureHandle CBTRenderFeature::PagedHeightSource(uint32_t terrainIndex, std::span<const uint32_t>& words,
                                                             uint64_t& version) const
{
    const auto* pages = m_Services ? m_Services->GetFeature<TerrainECS::TerrainHeightPageFeature>() : nullptr;
    if (pages == nullptr || m_KernelSet.IsNarrowHeap() || !pages->CacheTexture().IsValid() ||
        !pages->LatchedTable(terrainIndex, words, version))
        return {};
    return pages->CacheTexture();
}

void CBTRenderFeature::LogCommittedHeightSource(uint32_t terrainIndex, uint32_t generation, bool paged,
                                                uint32_t frameIndex)
{
    if (terrainIndex == m_CommittedSourceTerrain && generation == m_CommittedSourceGeneration &&
        paged == m_CommittedSourcePaged)
        return;
    m_CommittedSourceTerrain = terrainIndex;
    m_CommittedSourceGeneration = generation;
    m_CommittedSourcePaged = paged;
    Logger::Log::Info("Terrain pages: the CBT draws terrain {} (generation {}) from its {} from frame {}", terrainIndex,
                      generation, paged ? "pages" : "texture", frameIndex);
}

Rendering::TextureHandle CBTRenderFeature::BoundHeightSource(bool atlasBacked, Rendering::TextureHandle atlas,
                                                             Rendering::TextureHandle heightmap)
{
    return atlasBacked && atlas.IsValid() ? atlas : heightmap;
}

void CBTRenderFeature::ConsumeUpdateActivity()
{
    uint64_t sequence = 0;
    CBTTerrain::CBTUpdateActivity activity{};
    if (!m_ActivityReadback.TryRead(sequence, activity))
        return;
    m_RestGate.OnActivityRead(sequence, activity);
    // GE_CBT_VALIDATE: the re-evaluation load of each read update (0 on a full-pool forced pass,
    // which does not count).
    static const bool kLogActivity = std::getenv("GE_CBT_VALIDATE") != nullptr;
    if (kLogActivity)
        Logger::Log::Info("CBT activity: update {} re-evaluated {} bisectors (split {}, merge {})", sequence,
                          activity.VertexEvalCount, activity.SplitServed, activity.MergeServed);
}

bool CBTRenderFeature::IsUpdateAtRest(const CBTTerrain::CBTFrameParams& params)
{
    CBTTerrain::CBTClassifyDesc classify = m_Classify;
    classify.GateVertexEval = 0u; // chosen per recorded update, not an input
    m_UpdateInputs.clear();
    AppendInputBytes(m_UpdateInputs, params);
    AppendInputBytes(m_UpdateInputs, classify);
    AppendInputBytes(m_UpdateInputs, m_BoundHeightmap.id);
    const bool canSkip = m_RestGate.CanSkip(m_UpdateInputs);
    const bool edited =
        classify.DirtyMaxU > classify.DirtyMinU && classify.DirtyMaxV > classify.DirtyMinV;
    const bool atRest = canSkip && !edited && !m_ForceVertexEval.load(std::memory_order_relaxed);
    m_UpdateAtRest.store(atRest, std::memory_order_relaxed);
    return atRest;
}

CBTTerrain::CBTActivitySlot CBTRenderFeature::BeginUpdateActivityReadback(
    const Rendering::RenderGraph::RGFrame& frame, bool forced)
{
    return m_ActivityReadback.Begin(frame, m_RestGate.OnUpdateRecorded(forced));
}

bool CBTRenderFeature::BuildPrepassHead(RenderServices& rs, ViewId viewId, const DrawCommand& colour,
                                        DrawCommand& outHead) const
{
    if (!m_Material)
        return false;
    // The vertex stage fetches the geometry (gIdxVis, gVertex from set 2), so the head is vertex-modified: the
    // material's depth variant with no fragment stage, never the shared depth pipeline, which knows nothing of
    // the modifier. It draws with the colour draw's layout (no vertex stream), and binds its geometry through
    // the sets its pipeline declares (PipelineSetCount below).
    const MaterialKeyword headKeywords =
        ChooseDepthHeadPipeline(DepthPassKeywords(rs.GetWorldPassKeywords(viewId)), DepthPassType::Prepass,
                                /*alphaTest=*/false, /*vertexModified=*/true, /*sharedDepthOffered=*/false,
                                /*writesReliefDepth=*/false)
            .Keywords;
    const VertexAttributeFlags headVertexFlags = colour.VertexFlags;
    PipelineVariantCache& variants = rs.Materials().Variants();
    const PipelineVariantCache::VariantServeUnit* head = variants.FindOrRequestPrepassVariant(
        *m_Material, headVertexFlags, PrimitiveTopology::TriangleList, headKeywords, FrontFace::CounterClockwise);
    // A head whose pipeline the device has not built (still building, or failed) is not drawn: the colour
    // draw writes its own depth instead.
    if (!head || !variants.EnsureConcreteWarm(*m_Material, head->PipelineId, rs.Views().GetViewPrepassFormatKey(viewId)))
        return false;
    outHead = colour;
    outHead.VertexFlags = headVertexFlags;
    outHead.InternedPipeline = head->PipelineId;
    outHead.PipelineMeta = head->VariantMeta.get();
    // The modifier reads gIdxVis and gVertex from set 2: the binder walks the sets the head's pipeline declares.
    outHead.PipelineSetCount = static_cast<uint32_t>(head->SetLayouts.size());
    outHead.PassKeywords = headKeywords;
    return true;
}

bool CBTRenderFeature::BuildDrawCommand(RenderServices& rs, ViewId /*viewId*/,
                                        MaterialKeyword passKeywords, DrawCommand& outCmd)
{
    if (!EnsureMaterial(rs) || !m_Material)
        return false;

    // Null-variant guard (plan §9): the POC's silent failure was a vertex-stage
    // compile error that produced a pipeline with setLayouts == 0, so
    // ExecuteIndirectDraws early-outed every frame with nothing on screen. Assert
    // loudly here instead — an empty draw is better than a black frame with no clue.
    const GraphicsPipelineId pipeId = m_Material->GetGraphicsPipelineId();
    // Still compiling on a worker (EnsureMaterial): nothing to draw yet, and not a failure.
    if (!pipeId.IsValid() && rs.Materials().IsBaseCompileInFlight(CBTTerrainMaterialGuid()))
        return false;
    const GraphicsPipelineDesc* pipeDesc = m_Device ? m_Device->LookupGraphicsPipeline(pipeId) : nullptr;
    if (!pipeId.IsValid() || !pipeDesc || pipeDesc->DescriptorSetLayouts.empty())
    {
        Logger::Log::Error("CBTRenderFeature: CBT graphics variant has no descriptor set layouts "
                           "(setLayouts=0) — the composed vertex/fragment shader failed to compile. "
                           "Skipping the draw (plan §9 null-variant trap).");
        return false;
    }

    CBTResources& res = m_Instance.GetResources();

    // The surface samples the shared terrain splatmap + per-layer albedo/roughness from
    // the TerrainParams SSBO (set 2, binding 1) — the SAME buffer the CDLOD surface
    // reads, filled by the extraction system (renderer-blind), so a painted terrain is
    // A/B identical. CBT is single-terrain, so the surface reads terrains[0] (the terrain
    // BuildFrameParams selected). Without this binding the composed surface resolves a
    // null set-2 descriptor -> device fault, so refuse the draw if it is not ready
    // (BuildFrameParams already gates on an active terrain; this only trips in teardown).
    auto* terrainFeature = rs.GetFeature<TerrainECS::TerrainRenderFeature>();
    const uint32_t paramsSlot = terrainFeature ? terrainFeature->GetLastTerrainParamsSlot() : 0u;
    const uint32_t terrainParamsCount =
        terrainFeature ? terrainFeature->GetTerrainParamsCount(paramsSlot) : 0u;
    const BufferHandle paramsSSBO =
        terrainFeature ? terrainFeature->GetTerrainParamsSSBO(paramsSlot) : BufferHandle{};
    // The material table, taken from the SAME published slot as the params above: a params entry
    // names its materials by absolute table index, so mixing slots would resolve a real but wrong
    // material rather than fail.
    const uint32_t terrainMaterialCount =
        terrainFeature ? terrainFeature->GetTerrainMaterialCount(paramsSlot) : 0u;
    const BufferHandle materialsSSBO =
        terrainFeature ? terrainFeature->GetTerrainMaterialTableSSBO(paramsSlot) : BufferHandle{};
    if (!paramsSSBO.IsValid() || terrainParamsCount == 0u || !materialsSSBO.IsValid() ||
        terrainMaterialCount == 0u)
    {
        Logger::Log::Warning("CBTRenderFeature: terrain params/material SSBO not ready — skipping "
                             "the CBT draw");
        return false;
    }

    // Set-2 SSBOs, keyed by reflected INSTANCE name: the vertex modifier reads gIdxVis
    // (VISIBLE stream — Classify frustum-culls into it; shadows, a later slice, would
    // ride the ALL stream) + gVertex; the surface reads TerrainParamsBuffer. REFRESH the
    // handles from the live resources every frame (not cached-once): even though the CBT
    // buffers are persistent today, a cached handle that ever diverged from the live one
    // would resolve to a stale-but-nonnull descriptor (silent — MaterialBinder only warns
    // on a NULL miss). Updating in place (no clear/realloc) keeps a span already handed to
    // an earlier view's pass this frame valid.
    // Set-2 SSBOs by reflected instance name: the vertex modifier's gIdxVis/gVertex, the shared
    // TerrainParamsBuffer + TerrainMaterialTableBuffer (both nameless blocks, so the block name IS
    // the reflected name), and the planet-shading fragment buffers (gCbtSurf = per-pixel normal
    // params, gCbtSculpt = the sculpt page pool, gCbtSculptTable = the sculpt page table), plus the
    // atlas rows. The sculpt/surface buffers are offset-bound to THIS frame's ring slot so the
    // fragment reads element [0] / page-id-relative-0.
    if (m_DrawBuffers.size() != 8)
    {
        m_DrawBuffers.assign(8u, Engine::Renderer::DrawBindings::BufferEntry{});
        m_DrawBuffers[0].Name = HashStringId("gIdxVis");
        m_DrawBuffers[1].Name = HashStringId("gVertex");
        m_DrawBuffers[2].Name = HashStringId("TerrainParamsBuffer");
        m_DrawBuffers[3].Name = HashStringId("gCbtSurf");
        m_DrawBuffers[4].Name = HashStringId("gCbtSculpt");
        m_DrawBuffers[5].Name = HashStringId("gAtlasRows");
        m_DrawBuffers[6].Name = HashStringId("gCbtSculptTable");
        m_DrawBuffers[7].Name = HashStringId("TerrainMaterialTableBuffer");
    }
    m_DrawBuffers[0].Buffer = res.GetBuffer(CBTBinding::IndicesVisible);
    m_DrawBuffers[1].Buffer = res.GetBuffer(CBTBinding::CurrentVertex);
    m_DrawBuffers[2].Buffer = paramsSSBO;
    m_DrawBuffers[2].Offset = 0u;
    m_DrawBuffers[2].Range = static_cast<uint64_t>(terrainParamsCount) * sizeof(Terrain::TerrainGPUParams);
    m_DrawBuffers[3].Buffer = res.GetSurfaceParamsBuffer();
    m_DrawBuffers[3].Offset =
        static_cast<uint64_t>(m_FrameSlot) * CBTTerrain::kCBTSurfaceParamsSlotStride;
    m_DrawBuffers[3].Range = sizeof(CBTTerrain::CBTSurfaceParams);
    // gCbtSculpt = the physical page pool slot; gCbtSculptTable = the page table slot (both per ring
    // slot, so the fragment indexes from 0).
    const uint64_t sculptPoolSlotBytes = res.GetSculptPoolSlotBytes();
    m_DrawBuffers[4].Buffer = res.GetSphereSculptBuffer();
    m_DrawBuffers[4].Offset = static_cast<uint64_t>(m_FrameSlot) * sculptPoolSlotBytes;
    m_DrawBuffers[4].Range = sculptPoolSlotBytes;
    const uint64_t sculptTableSlotBytes = res.GetSculptTableSlotBytes();
    m_DrawBuffers[6].Buffer = res.GetSphereSculptPageTableBuffer();
    m_DrawBuffers[6].Offset = static_cast<uint64_t>(m_FrameSlot) * sculptTableSlotBytes;
    m_DrawBuffers[6].Range = sculptTableSlotBytes;
    // The atlas indirection rows (quality-sweep slice 1): the SAME binding-17 ring the compute samples,
    // offset-bound to this frame's ring slot so the surface reads Rows[tileIndex] directly. Bound
    // unconditionally (the shader declares it always); a non-atlas terrain's surface never reads it.
    const uint64_t atlasRowsSlotBytes =
        static_cast<uint64_t>(CBTTerrain::kAtlasMaxTiles) * CBTTerrain::kAtlasRowBytes;
    m_DrawBuffers[5].Buffer = res.GetAtlasRowsBuffer();
    m_DrawBuffers[5].Offset = static_cast<uint64_t>(m_FrameSlot) * atlasRowsSlotBytes;
    m_DrawBuffers[5].Range = atlasRowsSlotBytes;
    m_DrawBuffers[7].Buffer = materialsSSBO;
    m_DrawBuffers[7].Offset = 0u;
    m_DrawBuffers[7].Range =
        static_cast<uint64_t>(terrainMaterialCount) * sizeof(Terrain::TerrainMaterialRecord);

    // Env-gated (GE_CBT_DESC_DEBUG) diagnostic: dump every set-2 handle the binder
    // resolves for this draw (by reflected name), plus the indirect buffer + the
    // visible-record byte offset the DrawIndexedIndirectCount reads. A device-lost
    // repro then shows whether a set-2 handle went 0/stale (unresolved -> the binder's
    // own "set=2 binding=N not resolvable" warning fires; grep for it), whether the
    // per-frame params handle churns (a cycling handle on the fragile per-draw-SSBO
    // path — the 4dec2485c C3 concern), or whether all handles stay valid (=> the fault
    // is downstream in the descriptor-buffer write / the offset indirect read). The
    // record CONTENTS are proven well-formed offline by VisibleRecordDrawInputWellFormed.
    static const bool kCbtDescDebug = std::getenv("GE_CBT_DESC_DEBUG") != nullptr;
    if (kCbtDescDebug)
    {
        Logger::Log::Info("CBT.DrawBindings basePipe=0x{:x} set2[gIdxVis]=0x{:x} set2[gVertex]=0x{:x} "
                          "set2[TerrainParamsBuffer]=0x{:x} paramsRange={} identityIB=0x{:x} "
                          "countBuf=0x{:x} indirectBuf=0x{:x} visibleRecordOffset={} meta={}",
                          pipeId.Value, m_DrawBuffers[0].Buffer.id, m_DrawBuffers[1].Buffer.id,
                          m_DrawBuffers[2].Buffer.id, m_DrawBuffers[2].Range,
                          res.GetIdentityIndexBuffer().id, res.GetDrawCountBuffer().id,
                          res.GetBuffer(CBTBinding::IndirectDraw).id,
                          static_cast<uint32_t>(kDrawStreamVisible) * kIndirectDrawStrideBytes,
                          static_cast<const void*>(m_Material->GetShaderMeta().get()));
    }

    outCmd = DrawCommand{};
    // No pinned pipeline: the base one above is compiled from the material's own
    // keywords and knows nothing about what the pass adds. Leaving it unset lets
    // the world pass resolve the variant for `PassKeywords` — which is what makes
    // the SSSR normal-roughness + albedo outputs exist on the terrain at all, and
    // matches the rows MaterialVariantCook emits for this material.
    outCmd.Material = m_Material;
    outCmd.VertexFlags = VertexAttributeFlags::None;
    outCmd.PassKeywords = passKeywords;
    // No vertex buffer (customVertexShader); an identity index buffer so the
    // DrawIndexedIndirectCount path walks gl_VertexIndex 0..indexCount-1.
    outCmd.Geometry.AltGeom = {BufferHandle{}, res.GetIdentityIndexBuffer(), IndexType::Uint32};
    outCmd.UseIndirect = true;
    outCmd.IndirectCommandBuffer = res.GetBuffer(CBTBinding::IndirectDraw);
    outCmd.IndirectCountBuffer = res.GetDrawCountBuffer();
    outCmd.IndirectMaxDrawCount = kDrawCountValue; // 1
    outCmd.IndirectStride = kIndirectDrawStrideBytes; // 20 = VkDrawIndexedIndirectCommand
    // Draw the VISIBLE-stream record (Classify frustum-culls into it); its indexCount
    // is bounded by PrepareBisectorIndirect's clamp (3*POOL), same as the ALL record,
    // so the offset-selected draw can never walk past the identity index buffer.
    outCmd.IndirectCommandOffset =
        static_cast<size_t>(kDrawStreamVisible) * kIndirectDrawStrideBytes; // 20
    outCmd.Bindings.Buffers = m_DrawBuffers;

    // The twelve maps cbt_surface declares by name under the compat profile (set 2, b20-31). The
    // bindless profile reaches the same images through the indices already in the params/material
    // records, so this whole set exists only there. An invalid handle binds nothing and the shader's
    // own unbound-sentinel tests (AlbedoTex == 0 and friends) still decide what gets sampled.
    //
    // Two layers, not four: the compat arm clamps the layer ordinal and wraps 2-3 onto the last
    // bound layer. The ordinal is the ROLE SLOT (a fixed per-terrain mapping), not a per-pixel
    // choice, which is what makes a fixed binding correct rather than approximate.
    const std::vector<TerrainECS::TerrainInstanceInfo> activeTerrains =
        rs.GetProfile().IsCompat() && terrainFeature != nullptr
            ? terrainFeature->GetActiveTerrains()
            : std::vector<TerrainECS::TerrainInstanceInfo>{};
    if (!activeTerrains.empty())
    {
        // Terrain 0, the same one CBT already renders and warns about when there are more.
        const TerrainECS::TerrainHandle primary = activeTerrains.front().Handle;
        const auto layers = terrainFeature->GetCompatLayerTextures();
        // One sampler for all twelve: the compat surface declares separate texture + sampler
        // because a sampler2D apiece would blow WebGPU's 16-per-stage sampler limit and fail
        // pipeline creation outright. It REPEATS — the layer maps tile from a world-space UV, so
        // the heightmap's clamp sampler would collapse every tap onto one edge texel.
        const auto sampler = terrainFeature->GetLayerSampler();
        m_DrawTextures.clear();
        m_DrawTextures.reserve(13u);
        const auto bind = [&](const char* name, Rendering::TextureHandle tex) {
            Engine::Renderer::DrawBindings::TextureEntry e{};
            e.Name = HashStringId(name);
            e.Texture = tex;
            e.Sampler = sampler;
            m_DrawTextures.push_back(e);
        };
        bind("cbt_Normalmap", terrainFeature->GetNormalmapTexture(primary));
        bind("cbt_Splatmap", terrainFeature->GetSplatmapTexture(primary));
        bind("cbt_AtlasNormal", terrainFeature->GetAtlasNormalTexture(primary));
        bind("cbt_AtlasNormalCoarse", terrainFeature->GetAtlasNormalCoarseTexture(primary));
        bind("cbt_AtlasSplat", terrainFeature->GetAtlasSplatTexture(primary));
        bind("cbt_AtlasSplatCoarse", terrainFeature->GetAtlasSplatCoarseTexture(primary));
        bind("cbt_Layer0Albedo", layers.Albedo[0]);
        bind("cbt_Layer0Normal", layers.Normal[0]);
        bind("cbt_Layer0Orm", layers.Orm[0]);
        bind("cbt_Layer1Albedo", layers.Albedo[1]);
        bind("cbt_Layer1Normal", layers.Normal[1]);
        bind("cbt_Layer1Orm", layers.Orm[1]);
        {
            // The shared sampler is its own binding (32), so it needs its own entry; a texture
            // entry's Sampler only pairs with that texture.
            Engine::Renderer::DrawBindings::TextureEntry e{};
            e.Name = HashStringId("cbt_MapSampler");
            e.Sampler = sampler;
            m_DrawTextures.push_back(e);
        }
        outCmd.Bindings.Textures = m_DrawTextures;
    }

    return true;
}

float CBTRenderFeature::SampleSphereSurfaceHeight(float dx, float dy, float dz) const
{
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len <= 0.0f)
        return 0.0f;
    const float inv = 1.0f / len;
    dx *= inv;
    dy *= inv;
    dz *= inv;

    // Procedural relief is closed-form (device-free, from the active planet tuning); the
    // editable sculpt layer (dab + baked modifier stack) lives in TerrainService and is sampled
    // by direction (seam-free). Together they are the exact height the sphere VertexEval
    // displaces to — the editor brush refines its ray->planet hit against this so the cursor
    // tracks tall relief / sculpt instead of drifting on the analytic base sphere (#488).
    float h = CBTTerrain::PlanetRelief(dx, dy, dz, m_DomainConfig.ReliefAmplitude,
                                       m_DomainConfig.ReliefFrequency,
                                       std::max(m_DomainConfig.ReliefOctaves, 1u));
    // Composed sample (S2): the relief just computed is handed in so an analytic flatten cancels
    // it — the cursor tracks the exact pad the GPU renders, not the pre-flatten relief.
    if (auto* svc = TerrainECS::TerrainService::TryGet())
        h += svc->SampleSphereSculptHeight(dx, dy, dz, h);
    return h;
}

bool CBTRenderFeature::BuildFrameParams(RenderServices& rs, Rendering::ViewId viewId,
                                        uint32_t renderWidth, uint32_t renderHeight,
                                        uint32_t frameIndex, CBTTerrain::CBTFrameParams& out)
{
    // The first active terrain from TerrainRenderFeature — the SAME heightmap +
    // world size CDLOD renders, so the A/B compares identical data. C4 is
    // single-terrain (plan §8 C4); multi-terrain selection is a later slice.
    auto* terrainFeature = rs.GetFeature<TerrainECS::TerrainRenderFeature>();
    if (!terrainFeature)
    {
        m_Instance.SetHeightSource(frameIndex, Rendering::TextureHandle{}); // reset slot to default
        m_Instance.SetAtlasSource(frameIndex, Rendering::TextureHandle{});
        m_Instance.SetCoarseSource(frameIndex, Rendering::TextureHandle{});
        m_BoundHeightmap = {};
        m_TerrainShadowSource = {};
        return false;
    }
    const std::vector<TerrainECS::TerrainInstanceInfo> terrains = terrainFeature->GetActiveTerrains();
    if (terrains.empty())
    {
        m_Instance.SetHeightSource(frameIndex, Rendering::TextureHandle{}); // no dangling descriptor
        m_Instance.SetAtlasSource(frameIndex, Rendering::TextureHandle{});
        m_Instance.SetCoarseSource(frameIndex, Rendering::TextureHandle{});
        m_BoundHeightmap = {};
        m_TerrainShadowSource = {};
        return false;
    }
    if (terrains.size() > 1 && !m_MultiTerrainWarned)
    {
        m_MultiTerrainWarned = true;
        Logger::Log::Warning("CBTRenderFeature: {} active terrains — CBT renders only the first; "
                            "the A/B against CDLOD covers terrain 0 only (multi-terrain is a later slice)",
                            terrains.size());
    }
    const TerrainECS::TerrainInstanceInfo& t = terrains.front();

    // Bind the terrain heightmap into THIS frame's height-source ring slot (or the
    // flat default until it is uploaded). Only the current frame slot is rewritten, so
    // a mid-session heightmap handle change never touches an in-flight ring element.
    m_Instance.SetHeightSource(frameIndex, t.HeightmapTexture);
    // The paged height resolve (TerrainHeightPageFeature): when the terrain's height is paged, the
    // update samples the page cache through its page table (bindings 23 and 22) instead of the
    // unified or atlas texture, which stay bound for the readers that still use them.
    std::span<const uint32_t> pageWords;
    uint64_t pageVersion = 0;
    Rendering::TextureHandle pageCache = PagedHeightSource(t.Handle.Index, pageWords, pageVersion);
    // The ring slot grows to the terrain's table (its platform cap is checked where it is paged); a
    // failed allocation keeps the terrain on its texture this frame.
    if (pageCache.IsValid() && !m_Instance.ProvisionPageTableWords(static_cast<uint32_t>(pageWords.size())))
        pageCache = {};
    m_BoundTerrainIndex = t.Handle.Index;
    LogCommittedHeightSource(t.Handle.Index, t.Handle.Generation, pageCache.IsValid(), frameIndex);
    // Invalid until the heightmap uploads; feeds the C5 read edge (the page cache when paged, the
    // atlas when atlas-backed).
    m_BoundHeightmap =
        pageCache.IsValid() ? pageCache : BoundHeightSource(t.AtlasBacked, t.AtlasHeightTexture, t.HeightmapTexture);
    // The clearance map reads the unified height texture; an atlas-backed terrain has none.
    m_TerrainShadowSource.HeightTexture = t.AtlasBacked ? Rendering::TextureHandle{} : t.HeightmapTexture;
    m_TerrainShadowSource.HeightBindlessIndex = t.AtlasBacked ? 0u : t.HeightmapBindlessIndex;
    m_TerrainShadowSource.CastShadows = t.CastShadows;

    // Phase E resident-window atlas (design §3). An atlas-backed terrain drives the CBT
    // height sample through the indirection SSBO (binding 17) + atlas texture (binding 18)
    // instead of the unified heightmap: bind the atlas texture for this ring slot, upload the
    // indirection rows ONLY when the residency table changed since this slot was last written
    // (quiescence — a parked camera uploads nothing), and set the atlas geometry in the frame
    // params so VertexEval's CBT_SampleHeight resolves UV -> tile -> slot. The C5 read edge
    // then tracks the atlas texture (patched by TerrainUploadNode) instead of a heightmap.
    const uint32_t atlasSlot = CBTTerrain::CBTFrameRingSlot(frameIndex);
    if (t.AtlasBacked && t.AtlasHeightTexture.IsValid())
    {
        m_Instance.SetAtlasSource(frameIndex, t.AtlasHeightTexture);
        // Bind the out-of-window coarse height field (binding 19): an out-of-window tile resolves
        // through it, height-continuous with the resident relief at the window edge (design Risk 3).
        m_Instance.SetCoarseSource(frameIndex, t.AtlasCoarseTexture);
        // F1: a bound-terrain change brings a different atlas texture whose TableVersion may
        // numerically equal the previous terrain's; the version gate alone would then skip the row
        // upload and terrain B would resolve through terrain A's rows. A texture change resets the
        // slot's uploaded version so the new rows re-upload (this also forces the first upload).
        if (m_AtlasBoundTexture[atlasSlot] != t.AtlasHeightTexture)
        {
            m_AtlasBoundTexture[atlasSlot] = t.AtlasHeightTexture;
            m_AtlasUploadedTableVersion[atlasSlot] = ~0ull;
        }
        // Upload the indirection rows only when this slot has not seen the current table version
        // (a parked camera re-uploads nothing — GPU quiescence). Extraction publishes the rows every
        // frame, so a slot flagged dirty by a version bump or F1's texture-change reset always finds
        // them — no stale-row hole (see the binding-17 proof).
        if (m_AtlasUploadedTableVersion[atlasSlot] != t.AtlasTableVersion)
        {
            m_Instance.UploadAtlasRows(frameIndex, t.AtlasRowBytes.data(), t.AtlasRowCount);
            m_AtlasUploadedTableVersion[atlasSlot] = t.AtlasTableVersion;
        }
        out.AtlasParams0[0] = static_cast<float>(t.AtlasDim);
        out.AtlasParams0[1] = static_cast<float>(t.AtlasSlotStride);
        out.AtlasParams0[2] = static_cast<float>(t.AtlasSlotsPerRow);
        out.AtlasParams0[3] = static_cast<float>(t.AtlasTileRes);
        out.AtlasParams1[0] = static_cast<float>(t.AtlasTilesPerAxisX);
        out.AtlasParams1[1] = static_cast<float>(t.AtlasTilesPerAxisZ);
        out.AtlasParams1[2] = 1.0f;                                 // enabled
        out.AtlasParams1[3] = static_cast<float>(t.AtlasCoarseDim); // coarse field texels/axis (binding 19)
    }
    else
    {
        m_Instance.SetAtlasSource(frameIndex, Rendering::TextureHandle{});  // reset slot to default
        m_Instance.SetCoarseSource(frameIndex, Rendering::TextureHandle{}); // no dangling coarse descriptor
        m_AtlasBoundTexture[atlasSlot] = {};              // a later re-enable re-uploads the rows
        m_AtlasUploadedTableVersion[atlasSlot] = ~0ull;
        out.AtlasParams1[2] = 0.0f; // disabled -> the unified height sample path
    }
    const uint32_t pageSlot = CBTTerrain::CBTFrameRingSlot(frameIndex);
    m_Instance.SetPageCacheSource(frameIndex, pageCache);
    if (pageCache.IsValid())
    {
        // Upload the page table only when this slot has not seen this terrain's current version.
        if (m_PageBoundTerrain[pageSlot] != t.Handle.Index || m_PageUploadedTableVersion[pageSlot] != pageVersion)
        {
            m_Instance.UploadPageTable(frameIndex, pageWords);
            m_PageBoundTerrain[pageSlot] = t.Handle.Index;
            m_PageUploadedTableVersion[pageSlot] = pageVersion;
        }
        out.AtlasParams1[2] = 2.0f; // the paged resolve (cbt_layout.glsl CBT_SampleHeight)
    }
    else
    {
        m_PageUploadedTableVersion[pageSlot] = ~0ull; // a later page-in re-uploads the table
    }

    const Rendering::CameraData cam = rs.Views().ResolveCameraData(viewId);
    // Earth-scale slice 1b: feed Classify the REBASED (render-origin-relative) view-proj and
    // the render origin sector — the SAME CameraData the CameraUBO draw path reads (both come
    // from ViewRegistry::ResolveCameraData -> RenderOrigin.h::ComputeRebasedView), so terrain
    // and meshes cannot disagree about where the camera is (Risk 2 cross-pass agreement), and
    // the GE_ES_FORCE_WORLD_SPACE kill switch — honored inside ComputeRebasedView — pins BOTH
    // to sector 0 / world-space together (the A/B discriminator covers CBT for free). viewProjRel
    // == the world viewProj when the origin is inactive, so this is byte-identical near origin.
    std::memcpy(out.ViewProjRel, cam.viewProjRel, sizeof(out.ViewProjRel));
    // cameraPos stays FULL world (planet-center space): the horizon cull is a conservative
    // occlusion test about the planet centre and needs true radii, not the rebased frame.
    out.CameraPos[0] = cam.cameraPos[0];
    out.CameraPos[1] = cam.cameraPos[1];
    out.CameraPos[2] = cam.cameraPos[2];
    out.CameraPos[3] = 1.0f;
    // Sector as float(int) — exact for |sector| < 2^23; the shader reconstructs
    // originWorld = xyz * CBT_SECTOR_SIZE, matching GE_RenderOriginWorld bit-for-bit.
    out.RenderOriginSector[0] = static_cast<float>(cam.renderOriginSector[0]);
    out.RenderOriginSector[1] = static_cast<float>(cam.renderOriginSector[1]);
    out.RenderOriginSector[2] = static_cast<float>(cam.renderOriginSector[2]);
    out.RenderOriginSector[3] = static_cast<float>(cam.renderOriginSector[3]); // sector size (informational)

    const float splitPx = CBTTerrain::SplitThresholdPixels(m_TargetPixelError, renderHeight);
    m_AppliedSplitThresholdPx = splitPx;
    m_AppliedRenderHeightPx = renderHeight;
    out.Screen[0] = static_cast<float>(renderWidth);
    out.Screen[1] = static_cast<float>(renderHeight);
    out.Screen[2] = splitPx;         // split threshold
    out.Screen[3] = splitPx * 0.5f;  // merge threshold — the hysteresis band

    // World XZ = origin + uv * size; height = texel * heightScale + originY. Matches
    // the CDLOD vertex path (terrain_vertex_modifier.glsl) exactly.
    out.TerrainSize[0] = t.SizeX;
    out.TerrainSize[1] = t.SizeZ;
    out.TerrainSize[2] = t.HeightScale;
    out.TerrainSize[3] = t.WorldOriginY;

    out.TerrainOrigin[0] = t.WorldOriginX;
    out.TerrainOrigin[1] = t.WorldOriginZ;
    out.TerrainOrigin[2] = static_cast<float>(m_Classify.TargetDepth); // max-depth cap
    // The terrain normalmap bindless index (refreshed per frame — bindless indices
    // are per-frame). The graphics surface samples GE_BTEX(index) per-pixel for the
    // smooth normal, matching CDLOD. Index < 2^23 -> exact as float.
    out.TerrainOrigin[3] = static_cast<float>(t.NormalmapBindlessIndex);

    // C7 spherical: the planet radius + procedural relief the cube-sphere decode uses
    // (VertexEval reads these instead of the heightmap). Harmless in planar mode
    // (VertexEval only reads them when pc.domainMode == spherical). PlanetParams.w carries
    // the fBM octave count (plan §planet-shading) — VertexEval and the fragment shade with
    // the SAME multi-octave relief so geometry + normals agree.
    out.PlanetParams[0] = m_DomainConfig.PlanetRadius;
    out.PlanetParams[1] = m_DomainConfig.ReliefAmplitude;
    out.PlanetParams[2] = m_DomainConfig.ReliefFrequency;
    out.PlanetParams[3] = static_cast<float>(m_DomainConfig.ReliefOctaves);

    // View-prioritised split metric (arc slice S2, Option C). Aims the fixed pool at the camera
    // when it is contended; a no-op when disabled or on an unsaturated pool (the in-shader gate).
    // The near/far radii scale with camera altitude so the redistribution is planet-radius
    // invariant. Spherical altitude = distance-to-centre − radius; planar = height above the
    // terrain base. Clamped to a floor so a camera on the deck keeps a usable near disc.
    float altitude;
    if (m_DomainConfig.DomainMode == CBTTerrain::kDomainSpherical)
    {
        const float camLen = std::sqrt(out.CameraPos[0] * out.CameraPos[0] +
                                       out.CameraPos[1] * out.CameraPos[1] +
                                       out.CameraPos[2] * out.CameraPos[2]);
        altitude = camLen - m_DomainConfig.PlanetRadius;
    }
    else
    {
        altitude = out.CameraPos[1] - out.TerrainSize[3]; // camera Y above the terrain base (originY)
    }
    const CBTTerrain::CBTNearBiasRadii nearBias = CBTTerrain::ComputeNearBiasRadii(altitude);
    out.NearBias[0] = NearBiasEnabled() ? 1.0f : 0.0f;
    out.NearBias[1] = nearBias.NearRadius;
    out.NearBias[2] = nearBias.FarRadius;
    out.NearBias[3] = CBTTerrain::kNearBiasMaxCoarsen;

    // Walking-headroom demand shaping (walking-headroom slice). Bounds the behind-eye/straddle
    // force-split at a world facet target (reclaims the near-plane over-refinement that saturates
    // the walking pose), keeps off-frustum depth while the pool has headroom (kills the yaw
    // re-refine churn) and coarsens under pressure, and rescues flat-disc grazing facets the area
    // metric starves. Spherical-domain gates in the shader; every field 0 => the pre-slice metric.
    if (DemandTuningEnabled())
    {
        out.DemandTuning[0] = kNearFieldFacetTargetM;
        out.DemandTuning[1] = kOffFrustumKeepOcc;
        // Screen.z = split threshold px (the target scaled to the render height).
        out.DemandTuning[2] = kEdgeRescueTpeMul * out.Screen[2];
        out.DemandTuning[3] = kEdgeRescueOcc;
    }

    // Screen-area priority ordering (round-8c look-back). Under pool saturation, refine the largest-
    // on-screen facets first and hold a large just-off-frustum facet (the plateau you glanced away
    // from) at depth. The in-shader occupancy ramp makes an unsaturated pool classify exactly as
    // today regardless of the toggle; spherical-domain gates in the shader. x=0 => the pre-slice metric.
    out.PriorityParams[0] = PriorityEnabled() ? 1.0f : 0.0f;
    out.PriorityParams[1] = kPriorityRampStartOcc;
    out.PriorityParams[2] = kPriorityMaxAreaFloorPx2;
    out.PriorityParams[3] = kPriorityKeepLargeNdc;

    // Water plane: the seabed below it stays coarse (Kernel_Classify). No water => zeros => the
    // term is off and the metric is byte-identical to one without it. Planar gates in the shader.
    const ResolvedWaterPlane water = ResolveWaterPlane(rs, m_TerrainSeaLevel);
    if (water.Present)
    {
        out.WaterPlane[0] = water.SurfaceY;
        out.WaterPlane[1] = water.MarginM;
        out.WaterPlane[2] = kSubmergedCoarsen;
    }
    out.WaterPlane[3] = ContentAwareSplitPx();

    // Planet-shading surface params (plan §planet-shading): the small graphics-only buffer the
    // fragment reads for the per-pixel analytic sphere normal + slope/altitude splat. Written
    // once per frame into this frame's ring slot; the fragment offset-binds this slot in
    // BuildDrawCommand. O(1) per frame (does not touch the ~6.3 MB sculpt atlas — quiescence
    // neutral). SphereSculptEnabled tracks the edit version so an unedited planet skips the
    // per-pixel sculpt taps entirely (the common case pays nothing).
    m_FrameSlot = CBTTerrain::CBTFrameRingSlot(frameIndex);
    const bool spherical = m_DomainConfig.DomainMode == CBTTerrain::kDomainSpherical;
    CBTTerrain::CBTSurfaceParams surfaceParams{};
    surfaceParams.Radius = m_DomainConfig.PlanetRadius;
    surfaceParams.ReliefAmplitude = m_DomainConfig.ReliefAmplitude;
    surfaceParams.ReliefFrequency = m_DomainConfig.ReliefFrequency;
    surfaceParams.ReliefOctaves = m_DomainConfig.ReliefOctaves;
    surfaceParams.DebugMode = m_DebugView;
    // The surface's material inputs are NOT here: both terrain domains resolve them from the
    // terrain material table (TerrainRenderFeature), authored once by TerrainExtractionSystem.
    // This buffer carries only what is genuinely per-view/per-frame CBT state.
    // Phase E: an atlas-backed planar terrain resolves splat + normal per-pixel through the atlas
    // (quality-sweep slice 1). Hand the surface the atlas geometry + the four bindless texture indices
    // so cbt_surface.glsl runs the SAME UV -> slot resolve VertexEval runs and taps splat/normal there.
    surfaceParams.AtlasBacked = t.AtlasBacked ? 1u : 0u;
    if (t.AtlasBacked)
    {
        surfaceParams.AtlasDim = t.AtlasDim;
        surfaceParams.AtlasSlotStride = t.AtlasSlotStride;
        surfaceParams.AtlasSlotsPerRow = t.AtlasSlotsPerRow;
        surfaceParams.AtlasTileRes = t.AtlasTileRes;
        surfaceParams.AtlasTilesPerAxisX = t.AtlasTilesPerAxisX;
        surfaceParams.AtlasTilesPerAxisZ = t.AtlasTilesPerAxisZ;
        surfaceParams.AtlasCoarseDim = t.AtlasCoarseDim;
        surfaceParams.AtlasSplatBindless = t.AtlasSplatBindlessIndex;
        surfaceParams.AtlasNormalBindless = t.AtlasNormalBindlessIndex;
        surfaceParams.AtlasSplatCoarseBindless = t.AtlasSplatCoarseBindlessIndex;
        surfaceParams.AtlasNormalCoarseBindless = t.AtlasNormalCoarseBindlessIndex;
    }
    if (spherical)
    {
        if (auto* svc = TerrainECS::TerrainService::TryGet())
        {
            // Size the sculpt page store from the planet radius (idempotent + frozen after edits) so
            // the fragment, the compute VertexEval, and physics all derive the SAME radius-scaled
            // virtual dim. Then feed that geometry to the fragment (surface params) and the compute
            // path (frame params atlasParams0, free on the sphere).
            svc->ConfigurePlanetSculpt(m_DomainConfig.PlanetRadius);
            const CBTTerrain::SphereSculptGeometry geom = svc->GetPlanetSculptGeometry();
            surfaceParams.SphereSculptEnabled = svc->HasSphereSculptEdits() ? 1u : 0u;
            surfaceParams.SphereSculptVirtualDim = geom.VirtualDim;
            surfaceParams.SphereSculptCap = geom.Cap;
            surfaceParams.SphereSculptPagesPerAxis = geom.PagesPerAxis;
            surfaceParams.SphereSculptPoolPageCount = geom.PoolPageCount;
            out.AtlasParams0[0] = static_cast<float>(geom.VirtualDim);
            out.AtlasParams0[1] = static_cast<float>(geom.Cap);
            out.AtlasParams0[2] = static_cast<float>(geom.PagesPerAxis);
            out.AtlasParams0[3] = static_cast<float>(geom.PoolPageCount);
            // Analytic sphere modifiers (S2) + transient brush dabs (S3): the bounded placement
            // set rides BOTH param blocks — the compute tail (VertexEval / crease) and the
            // surface tail (per-pixel normal + altitude) — packed identically so every consumer
            // evaluates one closed form. Flattens fill slots [0, F), the held stroke's dabs
            // [F, F+D) (clamped to the shared cap — the service budgets the transient below the
            // published count, so the clamp is belt-and-braces). The set is tiny (16 x 48 B) and
            // rewritten with the params every frame, so no upload gate is needed; an empty set
            // (flag off) leaves the zero-filled tail = every branch skipped.
            std::array<CBTTerrain::SphereAnalyticFlatten,
                       CBTTerrain::kMaxSphereAnalyticModifiers> analytic{};
            const uint32_t analyticCount = svc->CopyPlanetAnalyticModifiers(analytic);
            std::array<CBTTerrain::SphereAnalyticDab,
                       CBTTerrain::kMaxSphereAnalyticModifiers> transientDabs{};
            const uint32_t dabCount =
                std::min(svc->CopyPlanetTransientDabs(transientDabs),
                         CBTTerrain::kMaxSphereAnalyticModifiers - analyticCount);
            out.AnalyticParams[0] = static_cast<float>(analyticCount);
            out.AnalyticParams[1] = static_cast<float>(dabCount);
            surfaceParams.SphereAnalyticCount = analyticCount;
            surfaceParams.SphereAnalyticDabCount = dabCount;
            for (uint32_t i = 0; i < analyticCount; ++i)
            {
                CBTTerrain::PackSphereAnalyticFlatten(
                    analytic[i], &out.SphereAnalytic[i * CBTTerrain::kSphereAnalyticFloatsPerModifier]);
                CBTTerrain::PackSphereAnalyticFlatten(
                    analytic[i],
                    &surfaceParams.SphereAnalytic[i * CBTTerrain::kSphereAnalyticFloatsPerModifier]);
            }
            for (uint32_t i = 0; i < dabCount; ++i)
            {
                const uint32_t slot = analyticCount + i;
                CBTTerrain::PackSphereAnalyticDab(
                    transientDabs[i],
                    &out.SphereAnalytic[slot * CBTTerrain::kSphereAnalyticFloatsPerModifier]);
                CBTTerrain::PackSphereAnalyticDab(
                    transientDabs[i],
                    &surfaceParams
                         .SphereAnalytic[slot * CBTTerrain::kSphereAnalyticFloatsPerModifier]);
            }
            // S3 per-cell placement culling: the compute chokepoints (crease gate / crease
            // midpoint / VertexEval) read one 16-bit mask per sample-point cell and loop only
            // the set bits — the cure for the all-N covers loop per live facet. Built here per
            // frame (384 cone tests x N — microseconds); zero placements leave the zero mask.
            if (analyticCount + dabCount > 0u)
                CBTTerrain::BuildSphereAnalyticCellMasks(analytic.data(), analyticCount,
                                                         transientDabs.data(), dabCount,
                                                         out.AnalyticCellMask);
        }
    }
    // Uploaded for BOTH domains: the sphere fragment reads the relief/sculpt fields, and the
    // debug-view fragment (planar or sphere) reads DebugMode — so a stale slot must never be
    // read. It is a 32-byte host-visible ring write (not the ~6.3 MB sculpt atlas below), so
    // this is quiescence-neutral. With DebugMode == 0 the planar surface output is unchanged.
    m_Instance.UploadSurfaceParams(frameIndex, surfaceParams);

    // One-shot signal naming the CBT debug view on toggle (ledger cleanup).
    if (m_DebugView != m_LoggedDebugView)
    {
        m_LoggedDebugView = m_DebugView;
        Logger::Log::Info("CBT.DebugView mode={}",
                          m_DebugView == 1u   ? "facets"
                          : m_DebugView == 2u ? "atlas-slots"
                                              : "off");
    }

    if (spherical && !m_SphereShadingSignalLogged)
    {
        m_SphereShadingSignalLogged = true;
        Logger::Log::Info("CBT.SphereShading normals=analytic splat=slope-altitude octaves={} "
                          "sculpt={} virtualDim={} pages={}",
                          m_DomainConfig.ReliefOctaves,
                          surfaceParams.SphereSculptEnabled != 0u ? "on" : "off",
                          surfaceParams.SphereSculptVirtualDim, surfaceParams.SphereSculptPoolPageCount);
    }

    // Planet editing v2: upload the sculpt page POOL + page TABLE into this frame's ring slot so the
    // sphere VertexEval samples this frame's store. Only when spherical AND this ring slot has not yet
    // uploaded the current sculpt version — so after an edit each ring slot refreshes ONCE and idle
    // frames upload nothing (BuildFrameParams runs per view per frame; an unconditional upload would
    // re-memcpy the whole pool forever). Before the first edit both SSBOs stay default (pool 0, table
    // NoPage) and VertexEval's sculpt sample is gated off, so the default pays nothing. The mutex
    // guards the store against a concurrent brush dab / modifier bake.
    if (m_DomainConfig.DomainMode == CBTTerrain::kDomainSpherical)
    {
        if (auto* svc = TerrainECS::TerrainService::TryGet())
        {
            const uint64_t sculptVersion = svc->SphereSculptVersion();
            if (m_SphereSculptUploadGate.ShouldUpload(frameIndex, sculptVersion))
            {
                CBTTerrain::SphereSculptGeometry geom;
                svc->CopySphereSculptUpload(m_SphereSculptScratch, m_SphereSculptTableScratch, geom);
                m_Instance.UploadSphereSculptPool(frameIndex, m_SphereSculptScratch.data(),
                                                  static_cast<uint32_t>(m_SphereSculptScratch.size()));
                m_Instance.UploadSphereSculptPageTable(
                    frameIndex, m_SphereSculptTableScratch.data(),
                    static_cast<uint32_t>(m_SphereSculptTableScratch.size()));
            }
        }
    }
    return true;
}

} // namespace GameEngine::CBTTerrainECS
