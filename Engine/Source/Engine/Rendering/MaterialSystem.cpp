// MaterialSystem.cpp — the material stack extracted from RenderServices (A1.3).
// The bulk of this file relocated verbatim from RenderServicesMaterials.cpp; the
// only substantive edits are the member re-homing (m_PerFrameWritePool ->
// injected m_FramePool, m_CullMode/m_FrontFace -> the RenderServices debug-
// override edge) and the two-phase Initialize/Shutdown split. Layout constants
// stay in RenderServicesDetail.h (§0a-A6), included below.
#include "Engine/Rendering/MaterialSystem.h"

#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/CompileConcurrencyGate.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialBinder.h"
#include "Engine/Rendering/MaterialColorClassify.h"
#include "Engine/Rendering/MaterialCompiler.h"
#include "Engine/Rendering/MaterialDepthClassify.h"
#include "Engine/Rendering/MaterialDeformationClassify.h"
#include "Engine/Rendering/PackageShaderDirs.h"
#include "Engine/Rendering/PerFrameWritePool.h"
#include "Engine/Rendering/MaterialPrewarmVariants.h"
#include "Engine/Rendering/PipelineVariantCache.h"
#include "Engine/Rendering/ProjectMaterialPrewarmService.h"
#include "Engine/Rendering/ShaderGraphMaterial.h"
#include "Engine/Rendering/TextureService.h"

#include "AssetCore/AssetEvents.h"
#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Assets/ShaderProgramAsset.h"
#include "Assets/AssetRegistry.h"
#include "Assets/MaterialAsset.h"
#include "Assets/AlphaCutoffThreshold.h"
#include "Assets/TextureAlphaDecodeProbe.h"
#include "Assets/TextureAlphaProbe.h"
#include "Core/Engine.h"
#include "Logger/Logger.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/DeviceFormatting.h"
#include "Rendering/Core/GPUDrawStreamBuilder.h"
#include "Rendering/Core/HashUtils.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Geometry/VertexLayoutBuilder.h"
#include "Rendering/Materials/MaterialBlendDerive.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/Materials/MaterialKeywordDerivation.h"
#include "AssetCore/SharedFileRead.h"
#include "Rendering/Materials/MaterialParamsLayout.h"
#include "Rendering/Materials/ShaderComposer.h"
#include "Rendering/Materials/ShaderPropertyTableCache.h"
#include "Types/StringId.h"
#include "Types/PathUtils.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "RenderServicesDetail.h"

namespace GameEngine
{
namespace Engine::Renderer
{
using namespace ::GameEngine::Rendering;

namespace
{

// The directory a material's shader references resolve from first. EMPTY when
// the asset path is unknown: ResolveShaderReference then skips the material-dir
// probe and walks project roots -> packages -> engine tree, because a stand-in
// directory must never masquerade as the material's own (the copied-project
// portability trap).
std::filesystem::path MaterialDirOf(const std::filesystem::path& materialAssetPath)
{
    return materialAssetPath.empty() ? std::filesystem::path{} : materialAssetPath.parent_path();
}

// The surface as a user knows it, for messages: its root-relative reference without the
// extension ("Surfaces/triplanar_pbr"), whether the document authored it or took the default.
std::string SurfaceDisplayName(const std::filesystem::path& surfacePath, const std::filesystem::path& materialDir,
                               const Rendering::MaterialBuildContext& context)
{
    std::filesystem::path relative = Rendering::ShaderComposer::ShaderRootRelative(surfacePath, materialDir, context);
    relative.replace_extension();
    return relative.generic_string();
}

// Seed for the PackMaterialSSBO content stamp (FNV-1a offset basis, the same
// chaining HashUtils::HashValue continues).
constexpr uint64_t kMaterialStampSeed = 1469598103934665603ull;

// The bindless texture indices an UNASSIGNED material slot must carry, slot for
// slot as Material::InitBindlessDefaults bakes them. Bindless index 0 is the
// engine-wide "not set" sentinel and the texture array's descriptor[0] is never
// written, so any row left at zero samples an unwritten descriptor: undefined
// per Vulkan, and on NVIDIA an alias of the most recently written descriptor —
// the stray-texture-leak symptom.
std::array<uint32_t, kTextureSlotArraySize> UnassignedSlotTextureIndices(const TextureService& textures)
{
    std::array<uint32_t, kTextureSlotArraySize> indices{};
    for (uint32_t ordinal = 0; ordinal < kTextureSlotArraySize; ++ordinal)
        indices[ordinal] = textures.UnassignedSlotBindlessIndex(ordinal);
    return indices;
}

// Write one MaterialParams row's unassigned-slot defaults over an already
// zeroed row: the texture indices above plus identity UV transforms. The SSBO
// stores the transforms as two split arrays (TextureST[8] then TextureST2[8]),
// NOT interleaved, so identity means row0.x = 1 in the first block and
// row1.y = 1 in the second; addressing them as one interleaved array yields a
// garbled transform that collapses UVs. The packed sampler word is left at
// zero: sampler-preset 0 (LinearRepeat) is a real entry of the dense
// sampler-preset array, unlike bindless texture 0.
void WriteUnassignedRowDefaults(uint8_t* rowPtr,
                                const std::array<uint32_t, kTextureSlotArraySize>& textureIndices)
{
    std::memcpy(rowPtr + kMaterialParamBytes, textureIndices.data(), kTextureIndexBytes);
    float* stPtr = reinterpret_cast<float*>(rowPtr + kMaterialParamBytes + kTextureIndexBytes);
    for (uint32_t s = 0; s < kTextureSlotArraySize; ++s)
    {
        stPtr[s * 4 + 0] = 1.0f;                           // TextureST[s].x  (row0)
        stPtr[(kTextureSlotArraySize + s) * 4 + 1] = 1.0f; // TextureST2[s].y (row1)
    }
}

// Material recompiles a single shader-source drain runs before carrying the
// remainder to later frames. Each is a full shaderc build (~250-500ms cold) on the serial
// frame-begin thread, so K materials sharing an edited surface would otherwise
// stall a single frame for K compiles. Two keeps the one- and two-material cases
// (the overwhelming majority of authoring saves) same-frame while bounding the
// worst-case hitch; materials awaiting their turn keep their last-good pipeline.
constexpr uint32_t kShaderEditRecompileBudgetPerFrame = 2;

// Build + intern the base material pipeline from a compiled variant. Factored out
// of CompileMaterialPipeline's inline PSO assembly so the synchronous path and the
// async base-compile worker share one definition. Uses only the thread-safe device
// caches (InternGraphicsPipeline / InternDescriptorSetLayout via the meta apply) and
// pure helpers, so it is safe to call from a worker thread. `variant` is taken by
// shared_ptr so the shader-byte aliasing shared_ptrs keep it alive for the pipeline
// desc's lifetime. Returns an invalid id on meta-apply failure (already logged).
Rendering::GraphicsPipelineId InternBaseMaterialPipeline(
    Rendering::IDevice& device,
    const std::shared_ptr<SharedShaderVariant>& variant,
    Rendering::VertexAttributeFlags vertexFlags,
    Rendering::CullModeFlags cullMode,
    Rendering::FrontFace frontFace,
    const Rendering::DerivedBlendState& blend,
    const std::string& debugName)
{
    Rendering::GraphicsPipelineDesc gd{};
    gd.Kind = Rendering::GraphicsPipelineKind::VertexFragment;
    // Shader bytes share storage with the variant via shared_ptr aliasing — no
    // duplication, and the variant outlives the pipeline cache entries.
    gd.VertexShader = std::shared_ptr<const std::vector<uint8_t>>(variant, &variant->vertexBytes);
    gd.PixelShader  = std::shared_ptr<const std::vector<uint8_t>>(variant, &variant->fragmentBytes);

    Rendering::BuildVertexLayoutFromFlags(vertexFlags, gd);

    gd.Rasterization.cullMode  = cullMode;
    gd.Rasterization.frontFace = frontFace;
    gd.DepthStencil.depthTestEnable  = blend.DepthTestEnable;
    gd.DepthStencil.depthCompareOp   = Rendering::CompareOp::GreaterOrEqual;
    gd.DepthStencil.depthWriteEnable = blend.DepthWriteEnable;
    gd.ColorBlend.attachments = {blend.Attachment};
    if (!blend.BlendEnable)
        gd.ColorBlend.attachments[0].blendEnable = false;

    gd.DebugName = debugName;

    // Set 1 is always the material texture set; the patch replaces its reflected
    // layout with the canonical one for the device's indexing mode so the
    // VkPipelineLayout matches the set actually bound at draw time.
    auto patchLayout = [&device](uint32_t setIndex, Rendering::DescriptorSetLayoutDesc& dsl) {
        if (setIndex == 1)
            dsl = CreateMaterialTextureSetLayout(&device);
    };

    std::string applyErr;
    if (!Rendering::MaterialHelper::ApplyShaderMetaToGraphicsDesc(
            device, *variant->meta, gd,
            Rendering::MaterialBuilder::MergeMode::Auto,
            {true, 128}, patchLayout, &applyErr))
    {
        Logger::Log::Warning("InternBaseMaterialPipeline '{}': failed to apply shader meta: {}",
                             debugName, applyErr);
        return {};
    }

    // Runtime-contract push constants: V|F @ 128 bytes regardless of meta
    // reflection (see the historical note in CompileMaterialPipeline).
    gd.PushConstants.Size      = 128;
    gd.PushConstants.StageMask = Rendering::kShaderStageVertex | Rendering::kShaderStageFragment;

    return device.InternGraphicsPipeline(std::move(gd));
}
} // namespace

MaterialSystem::MaterialSystem() = default;

MaterialSystem::~MaterialSystem()
{
    // Backstop the must-Shutdown-first invariant (was RenderServices::~RenderServices):
    // ShutdownPhaseA drains the prewarm jobs (which capture `this`), so by here the
    // count is normally already 0 and the drain below is a no-op. If Shutdown was
    // skipped, the assert flags it loudly in debug; setting the flag + draining then
    // keeps a still-tracked job from using freed members instead of racing teardown.
    {
        std::lock_guard<std::mutex> lk(m_PrewarmDrainMutex);
        m_PrewarmShutdown = true;
    }
    assert(m_PrewarmCounter.IsZero()
           && "MaterialSystem::ShutdownPhaseA() must run before ~MaterialSystem()");
    DrainPrewarmJobs();
}

void MaterialSystem::Initialize(Rendering::IDevice* device, PerFrameWritePool& framePool,
                                TextureService& textures, RenderServices& rs,
                                std::function<Rendering::MaterialKeyword()> resolveWorldPassKeywords)
{
    // The async base-compile submit + publish paths mutate main-thread-only state
    // (m_BaseCompileInFlight, the publish drain's Material writes) without locks.
    // Record the constructing thread so their debug asserts can catch a future
    // off-main caller. Initialize runs on the main/render thread.
    m_OwnerThreadId = std::this_thread::get_id();
    m_Device = device;
    m_FramePool = &framePool;
    m_Textures = &textures;
    m_RenderServices = &rs;
    m_ResolveWorldPassKeywords = std::move(resolveWorldPassKeywords);
    m_ProjectPrewarm = std::make_unique<ProjectMaterialPrewarmService>();

    // Global shader-compile admission gate. Bounds concurrent shaderc work so a
    // cold-load prewarm burst leaves cores + worker lanes for extraction and the
    // main thread. The dispatch closure captures nothing with lifetime concerns
    // (it reaches the engine job system through the singleton) and is only
    // invoked while the pool is up (SubmitTrackedPrewarm is guarded on that).
    const uint32_t compileCap = ComputeShaderCompileConcurrencyCap();
    m_CompileGate = std::make_unique<CompileConcurrencyGate>(
        compileCap,
        [](std::function<void()> work, JobSystem::JobPriority priority)
        {
            GameEngine::EngineCore::GetInstance().GetJobSystem().EnqueueWork(std::move(work),
                                                                            priority);
        });
    Logger::Log::Info("MaterialSystem: shader-compile concurrency cap = {}", compileCap);

    // The device's asynchronous pipeline builds (Request*Pipeline) run on the
    // tracked prewarm workers: capped by the compile gate, drained at shutdown.
    device->SetPipelineBuildDispatcher([this](std::function<void()> build) { DispatchPipelineBuild(std::move(build)); });
    // The binder binds references only (empty ctor); bind-time view/keyword reads
    // are its documented job (§0a-A3 edge 1).
    m_MaterialBinder = std::make_unique<MaterialBinder>(rs, *device);
    m_PipelineVariants = std::make_unique<PipelineVariantCache>(*this);
    // Compile outcomes feed the editor-facing error log (Shader Errors panel).
    m_ShaderCompilationCache.SetErrorLog(&m_ShaderErrors);

    // Runtime material registry: owns all runtime Material instances.
    m_RuntimeMaterialRegistry.Initialize(device);
    m_RuntimeMaterialRegistry.SetPreUnregisterCallback([this](const Material* mat) {
        m_ShaderCompilationCache.ClearVariantsForMaterial(mat);

        // Defer variant-pipeline cache erasure to BeginFrame on the render
        // thread. The unregister callback can fire from any thread (asset
        // reloader, GC worker); the four caches are written from the render
        // thread alone, so we hand off via a mutex-protected queue and drain at
        // frame boundary. Material* is captured by address — never dereferenced
        // after enqueue, since the Material may be freed before the drain runs.
        m_PipelineVariants->EnqueueEviction(mat);

        // Also drop any queued/in-flight publish-gate variant compile for this
        // material — the serial drain derefs the Material*, so a pending request
        // for a material about to be freed would UAF (defense for a future off-main
        // Unregister; today Unregister is test-only + main-thread).
        m_PipelineVariants->PurgePendingVariantCompiles(mat);

        uint32_t idx = mat->GetGpuSceneMaterialIndex();
        if (idx != Material::kInvalidSSBOIndex)
        {
            m_FreeMaterialSSBOIndices.push_back(idx);
            if (idx < m_MaterialsBySSBOIndex.size())
                m_MaterialsBySSBOIndex[idx] = nullptr;
            // Reset the freed slot to material-dependent so a shadow table built
            // before the index is reused can't merge a dead material into a
            // shared-depth class sentinel.
            if (idx < m_MaterialDepthClass.size())
                m_MaterialDepthClass[idx] =
                    static_cast<uint8_t>(Rendering::MaterialDepthClass::MaterialDependent);
            // A freed slot deforms nothing: a deformer-lane membership left
            // behind would put the next tenant of the index in the lane, and
            // out of the mover lane, on the frame it is registered.
            if (idx < m_MaterialDeformationMotion.size())
                m_MaterialDeformationMotion[idx] = 0u;
            ++m_MaterialSSBOGeneration;
        }
        m_Textures->OnMaterialUnregistered(mat->GetGuid());
    });

    // Material compiler/cache (renderer-owned, uses Engine AssetManager).
    // Guard: Engine may not be fully initialized in test environments.
    auto& engine = GameEngine::EngineCore::GetInstance();
    if (engine.IsInitialized())
        m_MaterialCompiler = std::make_unique<MaterialCompiler>(engine.GetAssetManager());
}

void MaterialSystem::ShutdownPhaseA()
{
    // The warm-up's discovery task reads the asset registry: join it while the
    // engine's AssetManager still exists (the engine tears assets down after the
    // renderer).
    m_ProjectPrewarm.reset();

    // Detach the .material reload subscriber before anything it touches tears
    // down (was RenderServices.cpp:1028).
    m_MaterialReloadInvalidator.Reset();
    m_ShaderSourceInvalidator.Reset();
    // No further drain will run, so carried recompiles are abandoned rather than
    // left holding GUIDs into a registry that is about to tear down.
    m_CarriedShaderEdits.clear();

    // No new device pipeline builds reach this system; the ones it holds end in
    // the drain below, before the device is destroyed.
    if (m_Device)
        m_Device->SetPipelineBuildDispatcher({});

    // Stop accepting new shader-variant prewarm jobs and wait for in-flight ones
    // to finish before anything they touch is torn down. They hold a raw pointer
    // to m_ShaderCompilationCache and would lock/use it after free otherwise
    // (ASan-confirmed shutdown heap-use-after-free). (was :1030-1038)
    {
        std::lock_guard<std::mutex> lk(m_PrewarmDrainMutex);
        m_PrewarmShutdown = true;
    }
    DrainPrewarmJobs();

    // Drop the descriptor binder before features/registries tear down: its
    // caches only hold transient descriptor handles, but destroying it here
    // keeps the tear-down order consistent with construction. (was :1052)
    m_MaterialBinder.reset();
}

void MaterialSystem::ShutdownPhaseB()
{
    // Clear the shader compilation cache before the material registry — the
    // per-material variant map holds Material pointers that become invalid after
    // registry shutdown. (was :1064)
    m_ShaderCompilationCache.Clear();
    m_ShaderErrors.Clear();

    // Suppress the pre-unregister callback during shutdown: the shader cache is
    // already bulk-cleared above, and the SSBO free-list / reverse lookup are
    // about to be cleared below. Only after the callback is disarmed reset the
    // variant cache the callback dereferences (EnqueueEviction), then shut the
    // registry down — feature teardown (RS-side, between the phases) can
    // unregister materials, so this order is load-bearing. (was :1069-1073)
    m_RuntimeMaterialRegistry.SetPreUnregisterCallback(nullptr);
    m_PipelineVariants.reset();
    m_RuntimeMaterialRegistry.Shutdown();

    m_MaterialCompiler.reset(); // was :1094

    // SSBO index + fallback state reset (was :1075-1081 and :1106-1109). The
    // physical MaterialParams fallback buffer is destroyed RS-side via
    // m_FallbackBuffers (§0a-A5); here we clear only the handles + bookkeeping.
    m_FreeMaterialSSBOIndices.clear();
    m_MaterialsBySSBOIndex.clear();
    m_MaterialDepthClass.clear();
    m_MaterialDeformationMotion.clear();
    m_MaterialColorClass.clear();
    m_MaterialColorClassGeneration = kInvalidColorClassGeneration;
    m_NextMaterialSSBOIndex = 0;
    ++m_MaterialSSBOGeneration;
    m_MaterialParamsSSBOFallback = {};
    m_MaterialParamsSSBOBuffer = {};
    m_MaterialParamsSSBOOffset = 0;
    m_MaterialParamsSSBOSize = 0;
    // RS recreates the physical fallback buffer after a device rebuild, so the
    // seed does not survive this reset.
    m_FallbackRowSeeded = false;

    // ShutdownPhaseA already drained the prewarm workers, so no more publish
    // records can arrive. Drop any that landed but were never applied (their
    // materials are tearing down) — no Material is dereferenced here.
    {
        std::lock_guard<std::mutex> lk(m_PendingPublishMutex);
        m_PendingPipelinePublishes.clear();
    }
    m_BaseCompileInFlight.clear();
}

void MaterialSystem::BeginFrame()
{
    // Apply shader-source edits queued by the watcher thread (or direct
    // callers) before the eviction drain below, so the pass-variant evictions
    // the recompiles enqueue are applied this frame, not next.
    DrainPendingShaderSourceEdits();

    // Drain pending variant-cache evictions enqueued by the material unregister
    // callback. Must run before any draw work so stale rows keyed on freed
    // Material* addresses don't survive into this frame's pass record. (was
    // RenderServicesFrameGraph.cpp:945)
    m_PipelineVariants->DrainPendingEvictions();

    // Apply base pipelines produced off-thread by async material registration.
    // Runs here on the serial frame-begin thread (before any parallel record
    // window opens) so the Material field writes never race a draw-time read.
    ApplyPendingPipelinePublishes();

    // A2.4-D6/P0-R: warm the shared record-path state on this serial thread BEFORE
    // any parallel record window opens, so no worker triggers a lazy, non-thread-safe
    // populate mid-record. Idempotent after the first ready frame.
    //   - EnsureMaterialBuildContextReady populates m_MaterialBuildContext (workers
    //     then only read it via IsMaterialBuildContextReady).
    //   - PreloadSharedDepthShaders force-loads both shared-depth variants.
    EnsureMaterialBuildContextReady();
    m_PipelineVariants->PreloadSharedDepthShaders();

    // Publish-gate: submit the variant compiles a record worker enqueued on a cold
    // miss last frame (option c). Runs here on the serial thread — after the build
    // context is warm and before any parallel record window opens — so it can deref
    // the Material to snapshot compile inputs, and the off-thread compiles land in
    // the cache a few frames later (the missed draw stays skipped until then).
    m_PipelineVariants->SubmitPendingVariantCompiles();

    // Apply the variant units the compile workers queued since last frame (and
    // any the submit above ran inline, headless). This serial apply is the ONLY
    // writer of live variant-cache nodes — record threads read entry fields and
    // hold stale-serve pointers without the lock, so node writes must never
    // overlap a record window (same envelope as ApplyPendingPipelinePublishes).
    m_PipelineVariants->ApplyPendingVariantPublishes();
    m_PipelineVariants->PruneSettledConcreteWarmWaits();

    // Open this frame's served-generation record, sized to the material index
    // domain the SSBO table spans, before any record worker can serve a unit.
    m_PipelineVariants->BeginServeFrame(static_cast<uint32_t>(m_MaterialsBySSBOIndex.size()));

    // Frame-boundary invalidation for the descriptor binder's transient caches.
    // Must run before any draw work uses the binder so set-0/set-1 handles from
    // the prior frame's transient pool are not handed out. (was :950-951)
    if (m_MaterialBinder)
        m_MaterialBinder->OnBeginFrame();

    PumpProjectPrewarm();
}

void MaterialSystem::PumpProjectPrewarm()
{
    auto& engine = GameEngine::EngineCore::GetInstance();
    if (!m_ProjectPrewarm || engine.GetRenderServices() != m_RenderServices)
        return;
    AssetManager* assets = engine.TryGetAssetManager();
    if (!assets)
        return;
    const std::filesystem::path projectRoot =
        engine.IsWorkspaceRootFallback() ? std::filesystem::path{} : engine.GetWorkspaceRoot();
    m_ProjectPrewarm->Update(*assets, *this, engine.GetJobSystem(), projectRoot,
                             std::chrono::steady_clock::now(), m_ResolveWorldPassKeywords);
}

void MaterialSystem::OnActiveRenderPipelineChanged()
{
    // Stale per-material variant entries must not persist across pipeline changes.
    m_ShaderCompilationCache.ClearAllMaterialVariants();
}

void MaterialSystem::OnProjectSwitched()
{
    // Cached material build paths: AdapterShaderDir / CacheRoot / IncludeDirs are
    // derived from the workspace root and the editor asset source, both of which
    // the switch has just repointed. The next EnsureMaterialBuildContextReady
    // re-derives them; without this reset a compile would use the old project's.
    m_MaterialBuildContext = Rendering::MaterialBuildContext{};
    m_AdapterShaderDirVerified = false;
    m_MaterialBuildContextSourceVersion = 0;

    // Compiled SPIR-V: entries are keyed by PATH, not by content. An identity
    // with a material asset path cannot collide across projects, so for those
    // this is unbounded retention rather than a hazard — but an identity with an
    // EMPTY asset path (primitives, graph previews, headless registrations) is
    // identical in both projects while the roots its references resolve against
    // have just moved. Keeping the old entry serves the old project's SPIR-V for
    // a reference that now names a different file. The include-closure rows
    // deliberately SURVIVE the clear (ShaderCompilationCache::m_SurfaceClosures):
    // materials registered before the switch are not recompiled, and without
    // their rows an include edit could never reach them again.
    //
    // Clear() advances the invalidation epoch, which bars a compile that had
    // already ELECTED before this call from inserting its SPIR-V afterwards:
    // publish compares the epoch snapshotted at election. It does NOT bar one
    // merely QUEUED before this call that elects after it — that snapshots the
    // POST-clear epoch, compares equal, and publishes SPIR-V built from the
    // build context it captured by value, i.e. the old project's. Bounded by how
    // many prewarm jobs are queued at the switch: a key carrying a material
    // asset path republishes an entry the new project can never probe with
    // (retention, as above), while an EMPTY-path key republishes exactly the
    // stale serve above until a later sweep drops it. Closure rows are not
    // epoch-gated at all, so a late publish also unions old-project filenames
    // into a surviving row — over-match, costing a redundant recompile.
    //
    // No DrainPrewarmJobs() here: it waits on the prewarm counter from the MAIN
    // thread, and this runs on ordinary startup (auto-load of the last project),
    // so the drain trades a bounded redundant compile for a multi-second stall.
    m_ShaderCompilationCache.Clear();

    // The inspector's per-material compile cache captured include paths and a
    // MaterialBuildContext from the old project; a cached `success` would skip
    // the recompile the new project needs. Only if one exists — Compiler() would
    // construct a compiler here purely to reset it, and asserts without a live
    // engine.
    if (m_MaterialCompiler)
        m_MaterialCompiler->Reset();
}

void MaterialSystem::SeedMaterialParamsFallback(Rendering::BufferHandle buffer, size_t bytes)
{
    m_MaterialParamsSSBOFallback = buffer;
    m_MaterialParamsSSBOBuffer = buffer;
    m_MaterialParamsSSBOOffset = 0;
    m_MaterialParamsSSBOSize = bytes;
}

void MaterialSystem::InstallReloadInvalidator(AssetEventDispatcher& dispatcher)
{
    // .material DISK edits (external tools, git pulls): re-register the fresh
    // document so texture bindings and properties update, not just the pipeline
    // caches (RenderingHotReloadBridge already invalidates those). Same path the
    // inspector save uses. ReloadedOnly: AssetReloaded is the only event
    // dispatched from the main thread (AssetManager::Update -> CheckForReloads);
    // Unloaded/Destroyed arrive synchronously on the file-watcher thread, and
    // material registration (MaterialRegistry, pipeline compile, UBO creation)
    // is strictly main-thread. A deleted material needs no refresh anyway.
    m_MaterialReloadInvalidator = AssetReloadInvalidator(
        dispatcher, AssetType::Material,
        [this](const GUID& guid)
        {
            // Only refresh materials something already renders; an unregistered
            // .material picks up the fresh document on first registration anyway.
            if (!m_RuntimeMaterialRegistry.Find(guid))
                return;
            auto asset = GameEngine::EngineCore::GetInstance().GetAssetManager().GetAsset(guid);
            if (!asset || asset->GetType() != AssetType::Material)
                return; // asset not resident — nothing to re-read from
            const auto* matAsset = static_cast<const MaterialAsset*>(asset.get());
            Compiler().Clear(guid);
            // Back through the RenderServices public forwarder so the world-pass
            // keyword resolution stays RS-side (§0.3) — same entry point the
            // pre-move lambda used.
            m_RenderServices->RegisterAndPrewarmMaterial(guid, matAsset->GetDocument());
        },
        AssetReloadInvalidator::EventSet::ReloadedOnly);

    // Shader SOURCE edits (.glsl surface / vertex modifier). A plain file edit
    // dispatches AssetModified on the watcher thread — and never AssetReloaded
    // unless the source happens to be resident in the AssetManager — so this
    // must ride ContentEvents with an enqueue-only handler. Without this lane a
    // saved surface edit changed nothing on screen: the pipeline-cache clear in
    // RenderingHotReloadBridge only rebuilds VkPipelines from the interned
    // descs, and the in-memory ShaderCompilationCache kept serving the old
    // SPIR-V (the walk evidence behind the 2026-07-25 authoring-loop fix).
    m_ShaderSourceInvalidator = AssetReloadInvalidator(
        dispatcher, AssetType::Shader,
        [this](const GUID& guid)
        {
            std::lock_guard<std::mutex> lk(m_PendingShaderEditsMutex);
            m_PendingShaderEditGuids.push_back(guid);
        },
        AssetReloadInvalidator::EventSet::ContentEvents);
}

void MaterialSystem::NotifyShaderSourceEdited(const std::filesystem::path& sourcePath)
{
    if (sourcePath.empty())
        return;
    std::lock_guard<std::mutex> lk(m_PendingShaderEditsMutex);
    m_PendingShaderEditPaths.push_back(sourcePath);
}

void MaterialSystem::DrainPendingShaderSourceEdits()
{
    std::vector<GUID> guids;
    std::vector<std::filesystem::path> paths;
    {
        std::lock_guard<std::mutex> lk(m_PendingShaderEditsMutex);
        if (!m_PendingShaderEditGuids.empty() || !m_PendingShaderEditPaths.empty())
        {
            guids.swap(m_PendingShaderEditGuids);
            paths.swap(m_PendingShaderEditPaths);
        }
    }
    // Idle fast-out: nothing new arrived AND nothing is carried over from a
    // previous frame's budget. One uncontended lock, no allocation (swapping
    // empty vectors moves no storage) — the same cost the pre-budget drain paid.
    if (guids.empty() && paths.empty() && m_CarriedShaderEdits.empty())
        return;

    // Resolve the GUID lane (asset events) to paths. Registry metadata is the
    // authority; unresolvable GUIDs (bulk-destroy races) are dropped.
    if (!guids.empty())
    {
        auto& engine = GameEngine::EngineCore::GetInstance();
        if (engine.IsInitialized())
        {
            auto& am = engine.GetAssetManager();
            for (const GUID& guid : guids)
            {
                AssetMetadata metadata;
                if (!am.GetRegistry().TryGetAssetMetadata(guid, metadata) || metadata.Path.empty())
                    continue;
                std::filesystem::path p = metadata.Path;
                if (p.is_relative())
                    p = am.ResolveAssetPath(p);
                if (!p.empty())
                    paths.push_back(std::move(p));
            }
        }
    }
    // No new edits this frame — fall through to spend the budget on carried work.
    if (paths.empty())
    {
        SpendShaderEditRecompileBudget();
        return;
    }

    // Authored material references are relative strings ("my_surface.glsl",
    // "Materials/water.glsl") while the notifications carry absolute paths;
    // the filename is the only stable common key. Over-matching (same filename
    // in two roots) costs a redundant recompile that the content-keyed disk
    // cache absorbs; under-matching would silently keep stale pixels.
    auto toLower = [](std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    std::vector<std::string> fileNames;
    for (const auto& p : paths)
    {
        std::string name = toLower(p.filename().string());
        if (!name.empty()
            && std::find(fileNames.begin(), fileNames.end(), name) == fileNames.end())
            fileNames.push_back(std::move(name));
    }

    for (const std::string& fileName : fileNames)
    {
        // One closure matcher for the whole lane. The sweep drops the dependent
        // cache entries AND names the dependent source identities in the same
        // locked pass; the registry scan below only asks which registered
        // materials carry one of those identities. A second closure query per
        // material would be a second matcher, free to answer differently as rows
        // and entries drift apart — and it would take the cache's lock once per
        // registered material.
        const ShaderFileDependents deps =
            m_ShaderCompilationCache.InvalidateEntriesForShaderFile(fileName);
        const std::unordered_set<ShaderSourceKey> matched(deps.Identities.begin(),
                                                          deps.Identities.end());

        std::vector<GUID> affected;
        m_RuntimeMaterialRegistry.ForEach(
            [&](const GUID& guid, const Material& mat)
            {
                const MaterialCompileSpec& spec = mat.GetCompileSpec();
                auto references = [&](const std::string& authored)
                {
                    return !authored.empty()
                        && toLower(std::filesystem::path(authored).filename().string()) == fileName;
                };
                // Direct references catch an authored surface / vertex-modifier
                // edit even for a material nothing has built this session — no
                // closure row exists for those, so no identity can name them.
                // The identity match catches everything a build pulled in:
                // adapters, engine includes, transitive helpers. It also carries
                // implicit-default-surface materials (empty surfaceShaderPath ->
                // Surfaces/standard_surface.glsl), whose identity is the empty
                // spec strings and whose row records the default surface chain
                // their build actually read.
                // Generated graph-preview materials are excluded: their owner
                // (the graph preview host) writes the source AND drives the
                // recompile in the same breath, so the watcher echo would only
                // repeat the compile — synchronously, on the main thread, one
                // ~750ms frame per material (felt as drag hitches seconds after
                // a graph edit).
                if (Engine::Renderer::IsGraphLivePreviewMaterialPath(mat.GetMaterialAssetPath()))
                    return;
                if (references(spec.surfaceShaderPath) || references(spec.vertexModifierPath)
                    || matched.count(ShaderSourceKey{spec.surfaceShaderPath,
                                                     spec.vertexModifierPath,
                                                     mat.GetMaterialAssetPath()})
                           != 0)
                    affected.push_back(guid);
            });

        if (deps.DroppedEntries != 0 || !affected.empty())
        {
            Logger::Log::Info(
                "MaterialSystem: shader source '{}' changed -> dropped {} cached shader(s), "
                "{} material(s) queued for recompile",
                fileName, deps.DroppedEntries, affected.size());
        }
        // A file edited again while its previous batch is still carried is
        // rebuilt from scratch: the old entry is dropped unconditionally, so the
        // NEWEST save decides the affected set and every material sharing the
        // file recompiles against it. Materials already recompiled from the
        // older content simply recompile again; a set that has since emptied
        // (materials unregistered) drops without leaving stale GUIDs queued.
        std::erase_if(m_CarriedShaderEdits,
                      [&fileName](const CarriedShaderEdit& e) { return e.FileName == fileName; });
        if (affected.empty())
            continue;
        m_CarriedShaderEdits.push_back({fileName, std::move(affected), 0});
    }

    // Logged only on drains that received new edits — once per save burst rather
    // than once per frame for as long as the carry lasts.
    if (const size_t carried = SpendShaderEditRecompileBudget(); carried != 0)
    {
        Logger::Log::Info(
            "MaterialSystem: shader-edit recompile budget ({}/frame) spent -> {} material(s) "
            "carried to later frames",
            kShaderEditRecompileBudgetPerFrame, carried);
    }
}

size_t MaterialSystem::SpendShaderEditRecompileBudget()
{
    uint32_t remaining = kShaderEditRecompileBudgetPerFrame;
    for (CarriedShaderEdit& edit : m_CarriedShaderEdits)
    {
        while (remaining != 0 && edit.NextIndex < edit.Affected.size())
        {
            RecompileMaterialForShaderEdit(edit.Affected[edit.NextIndex++]);
            --remaining;
        }
        if (remaining == 0)
            break;
    }
    std::erase_if(m_CarriedShaderEdits, [](const CarriedShaderEdit& e)
                  { return e.NextIndex >= e.Affected.size(); });

    size_t carried = 0;
    for (const CarriedShaderEdit& edit : m_CarriedShaderEdits)
        carried += edit.Affected.size() - edit.NextIndex;
    return carried;
}

void MaterialSystem::RecompileMaterialForShaderEdit(const GUID& guid)
{
    Material* mat = m_RuntimeMaterialRegistry.Find(guid);
    if (!mat)
        return;
    // Drop the inspector-lane result so the diagnostics recompile
    // against the edited source next time the material is viewed.
    if (m_MaterialCompiler)
        m_MaterialCompiler->Clear(guid);
    // A shader CONTENT edit leaves the material document unchanged, so
    // the live compile spec is the document for this purpose — same
    // synthesis GetOrCompile uses. This also covers materials without a
    // backing asset (graph previews, headless registration).
    const MaterialCompileSpec& spec = mat->GetCompileSpec();
    MaterialDocument doc{};
    doc.surfaceShader = spec.surfaceShaderPath;
    doc.vertexModifier = spec.vertexModifierPath;
    doc.lightingModel = spec.lightingModel;
    doc.customVertexShader = spec.customVertexShader;
    doc.keywords = spec.userKeywords;
    // Async recompile: the shaderc base compile runs on a worker and the
    // result publishes on a later BeginFrame, so a save burst never runs
    // compiles on the frame loop. The submit bumps the material version,
    // which retires the pass-variant PSO rows; PipelineVariantCache serves a
    // version-stale row while workers rebuild it, so record-path draws keep
    // the last-good pipeline instead of leaving a hole. The submit also
    // retires the base pipeline id (the publish guard requires it), and the
    // extraction gate skips the material while that id is invalid — the
    // object is out of the draw stream for the base compile's frames. A
    // FAILED compile restores the retired id at publish (fail-visible: the
    // mesh keeps rendering the last good shader; errors are logged by
    // ShaderCompilationCache and surface in the Shader Errors panel).
    // Eviction stays reserved for a material actually going away. The
    // declared-property table follows the publish the same way: it is re-laid
    // where the new pipeline binds, so a failed compile keeps the lanes the
    // still-running shader was laid out for.
    RecompileMaterialPipeline(guid, doc);
}

MaterialCompiler& MaterialSystem::Compiler()
{
    // MaterialCompiler is created during Initialize(). If someone calls this too
    // early, fall back to creating it on-demand (requires a fully initialized Engine).
    if (!m_MaterialCompiler)
    {
        auto& engine = GameEngine::EngineCore::GetInstance();
        if (engine.IsInitialized())
            m_MaterialCompiler = std::make_unique<MaterialCompiler>(engine.GetAssetManager());
    }
    assert(m_MaterialCompiler && "Compiler() called before Initialize() or engine not ready");
    return *m_MaterialCompiler;
}

MaterialBinder& MaterialSystem::Binder()
{
    return *m_MaterialBinder;
}

const MaterialBinder& MaterialSystem::Binder() const
{
    return *m_MaterialBinder;
}

Rendering::CullModeFlags MaterialSystem::DebugCullMode() const
{
    // §0a-A3 edge 2: the rasterizer state lives on RenderServices; the facade
    // reads it synchronously at PSO build.
    return m_RenderServices->m_CullMode;
}

Rendering::FrontFace MaterialSystem::DebugFrontFace() const
{
    return m_RenderServices->m_FrontFace;
}

void MaterialSystem::FinalizeFrameBuffers()
{
    // Both indexing modes read the MaterialParams SSBO: it carries the material
    // param block and TextureST. Only its texture-index lane is bindless-only,
    // and Classic simply never reads that lane.
    PackMaterialSSBO();
}

void MaterialSystem::EnsureFallbackRowSeeded()
{
    if (m_FallbackRowSeeded || !m_Device || !m_MaterialParamsSSBOFallback.IsValid())
        return;

    // The row an overflow frame binds must be an UNASSIGNED row, not a zeroed
    // one: every draw in the scene collapses onto it, and zeroed texture
    // indices would point the whole scene at the never-written bindless
    // descriptor 0 (WriteUnassignedRowDefaults states the consequence).
    const auto textureIndices = UnassignedSlotTextureIndices(*m_Textures);

    // The TextureService publishes these indices when its default textures
    // register, which can be later than the first pack. Seeding before that
    // would latch the very zeros this seeding exists to avoid, so wait.
    for (const uint32_t index : textureIndices)
    {
        if (index == 0u)
            return;
    }

    uint8_t row[kMaterialEntryStride]{};
    WriteUnassignedRowDefaults(row, textureIndices);
    if (void* ptr = m_Device->MapBuffer(m_MaterialParamsSSBOFallback))
    {
        std::memcpy(ptr, row, sizeof(row));
        m_Device->UnmapBuffer(m_MaterialParamsSSBOFallback);
        m_FallbackRowSeeded = true;
    }
}

void MaterialSystem::HandleMaterialParamsOverflow(uint32_t materialCount, size_t requestedBytes)
{
    ++m_MaterialParamsOverflowCount;

    // Bind the app-lifetime fallback row rather than leaving last frame's
    // binding in place. The retained handle would be a ring slot's buffer, and
    // the ring destroys and recreates that buffer when the slot grows — so the
    // very overflow that triggers growth is what can turn the retained handle
    // stale. The fallback is a single row and all three readers of this binding
    // clamp the row index to the bound length (material_row_index.glsl), so
    // every draw reads that one row this frame: a scene shaded flat in the
    // TextureService defaults is a visible, safe failure, and a dangling buffer
    // handle in binding 13 is neither.
    m_MaterialParamsSSBOBuffer = m_MaterialParamsSSBOFallback;
    m_MaterialParamsSSBOOffset = 0;
    m_MaterialParamsSSBOSize = m_MaterialParamsSSBOFallback.IsValid() ? kMaterialEntryStride : 0;

    // Rate limit: first occurrence, then every power of two. The pack grows the
    // ring before it allocates (Reserve), so an overflow means a demand
    // past the configured cap or a failed device allocation.
    const uint64_t n = m_MaterialParamsOverflowCount;
    if ((n & (n - 1)) != 0)
        return;

    const size_t slotCapacity = m_FramePool->GetCapacity(FrameWriteUsage::MaterialParams);
    const size_t growCap = m_FramePool->GetMaxCapacity(FrameWriteUsage::MaterialParams);

    // Past the cap, growth is not a matter of waiting a frame — it can never
    // cover this demand, so say that instead of advising patience.
    if (requestedBytes > growCap)
    {
        Logger::Log::Error(
            "MaterialParams ring overflow ({}x): {} materials need {} bytes, which is past the "
            "{} byte grow cap — the ring CANNOT grow to fit this scene. Every frame shades from "
            "the fallback row until the material count drops or PerFrameWritePoolConfig "
            "MaterialParams maxCapacityBytes is raised.",
            n, materialCount, requestedBytes, growCap);
        return;
    }

    Logger::Log::Error(
        "MaterialParams ring overflow ({}x): {} materials need {} bytes but the ring slot holds "
        "{} bytes (grow cap {} bytes): the device could not allocate the larger ring. Shading "
        "from the fallback row this frame.",
        n, materialCount, requestedBytes, slotCapacity, growCap);
}

void MaterialSystem::PackMaterialSSBO()
{
    // Pack all registered materials into the MaterialParams SSBO.
    // Each entry: material CPU cache (padded to kMaterialParamBytes) + texture indices = kMaterialEntryStride.
    // materialIndex on GPUInstance indexes into this array.

    // The overflow path below binds the fallback row for the whole scene, so it
    // has to hold unassigned-slot defaults before it can ever be bound.
    EnsureFallbackRowSeeded();

    // Always allocate at least 1 entry so the MaterialParamsSSBO buffer is valid
    // for descriptor binding even when no materials have been registered yet.
    const uint32_t materialCount = std::max(m_NextMaterialSSBOIndex, 1u);

    const size_t totalBytes = static_cast<size_t>(materialCount) * kMaterialEntryStride;
    // Growing: a material count past the ring's size (a folder of models
    // registering theirs) must not shade one frame per ring slot from the
    // fallback row before the ring catches up.
    m_FramePool->Reserve(FrameWriteUsage::MaterialParams, totalBytes);
    auto alloc = m_FramePool->Allocate(FrameWriteUsage::MaterialParams, totalBytes, 16);
    if (!alloc.IsValid())
    {
        HandleMaterialParamsOverflow(materialCount, totalBytes);
        return;
    }

    // O(1) content stamp over everything this function packs: the slot
    // mapping (m_MaterialSSBOGeneration, bumped when that mapping moves — an
    // index assigned, an index freed, or a shutdown clear; a re-registration
    // keeps its index and does NOT bump it),
    // the process-wide material content epoch (bumped by every MarkDirty —
    // properties, bindless indices, UV transforms, sampler preset), and the
    // default bindless indices seeded into unvisited rows. Equal stamp
    // implies a byte-identical pack. Whole-array granularity matches the
    // pack itself: any material change repacks everything.
    uint64_t stamp = Rendering::HashUtils::HashValue(kMaterialStampSeed, m_MaterialSSBOGeneration);
    stamp = Rendering::HashUtils::HashValue(stamp, materialCount);
    stamp = Rendering::HashUtils::HashValue(stamp, Material::GetGlobalContentEpoch());
    stamp = Rendering::HashUtils::HashValue(stamp, m_Textures->DefaultWhiteBindlessIndex());
    stamp = Rendering::HashUtils::HashValue(stamp, m_Textures->DefaultFlatNormalBindlessIndex());
    stamp = Rendering::HashUtils::HashValue(stamp, m_Textures->DefaultBlackBindlessIndex());

    // Publish the SSBO binding every frame — buffer/offset differ per slot.
    m_MaterialParamsSSBOBuffer = alloc.buffer;
    m_MaterialParamsSSBOOffset = alloc.offset;
    m_MaterialParamsSSBOSize = totalBytes;

    // Skip the repack when this frame slot's ring memory already holds this
    // exact pack. Valid because MaterialParams is this function's exclusive
    // usage bucket and it runs once per pool BeginFrame — the ring hands back
    // the same {buffer, offset} every cycle while sizes are stable (placement
    // verified below; growth or buffer recreation fails the check and
    // repacks). A second call in one frame would allocate at a shifted offset
    // and permanently disengage the skip — visible as m_MaterialSSBOPackCount
    // climbing while idle.
    auto& packedSlot =
        m_PackedMaterialSlots[m_FramePool->GetCurrentSlot(FrameWriteUsage::MaterialParams)];
    if (packedSlot.Stamp == stamp && packedSlot.Buffer == alloc.buffer
        && packedSlot.Offset == alloc.offset)
    {
        return;
    }

    ++m_MaterialSSBOPackCount;

    // Zero-fill the entire allocation (default values for unvisited slots).
    std::memset(alloc.ptr, 0, totalBytes);

    // Seed every row with valid defaults so an unvisited/recycled row is never
    // sampled as zero (see WriteUnassignedRowDefaults for why zero is unsafe).
    // Populated rows overwrite these below.
    const auto defaultTexIdx = UnassignedSlotTextureIndices(*m_Textures);
    for (uint32_t i = 0; i < materialCount; ++i)
        WriteUnassignedRowDefaults(static_cast<uint8_t*>(alloc.ptr) + i * kMaterialEntryStride,
                                   defaultTexIdx);

    // Sequential flat-vector scan replaces unordered_map ForEach for cache-friendly iteration.
    const uint32_t lookupSize = static_cast<uint32_t>(m_MaterialsBySSBOIndex.size());
    for (uint32_t idx = 0; idx < materialCount && idx < lookupSize; ++idx)
    {
        const Material* mat = m_MaterialsBySSBOIndex[idx];
        if (!mat)
            continue;

        uint8_t* entryPtr = static_cast<uint8_t*>(alloc.ptr) + idx * kMaterialEntryStride;

        const uint32_t cacheSize = mat->GetCacheSize();
        const uint32_t copyBytes = std::min(cacheSize, kMaterialParamBytes);
        if (copyBytes > 0)
            std::memcpy(entryPtr, mat->GetCacheData(), copyBytes);

        std::memcpy(entryPtr + kMaterialParamBytes, mat->GetBindlessTextureIndices(), kTextureIndexBytes);

        // Material stores texture transforms interleaved by slot:
        //   slot0 row0, slot0 row1, slot1 row0, slot1 row1, ...
        // The shader SSBO stores them as two arrays:
        //   TextureST[8] row0s, then TextureST2[8] row1s.
        // Repack instead of bulk-copying, otherwise default identity transforms
        // become (u, u) for albedo in bindless draws.
        const float* srcRows = mat->GetTextureTransforms();
        float* dstRows = reinterpret_cast<float*>(entryPtr + kMaterialParamBytes + kTextureIndexBytes);
        for (uint32_t slot = 0; slot < kTextureSlotArraySize; ++slot)
        {
            std::memcpy(dstRows + slot * 4u, srcRows + slot * 8u, 4u * sizeof(float));
            std::memcpy(dstRows + (kTextureSlotArraySize + slot) * 4u,
                        srcRows + slot * 8u + 4u,
                        4u * sizeof(float));
        }

        const uint32_t packedSamplers = mat->GetPackedSamplerIndices();
        std::memcpy(entryPtr + kSamplerIndicesOffset, &packedSamplers, sizeof(uint32_t));
    }

    packedSlot = PackedMaterialSlot{alloc.buffer, alloc.offset, stamp};
}

Material* MaterialSystem::RegisterMaterialFromDocument(
    const GUID& guid,
    const MaterialDocument& doc,
    Rendering::MaterialKeyword additionalKeywords,
    BaseCompileMode baseCompileMode)
{
    using Clock = std::chrono::high_resolution_clock;
    const auto tStart = Clock::now();
    auto msElapsed = [](Clock::time_point t) {
        return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
    };

    // Reconcile GUID↔path for shader references so that both the runtime
    // (which needs paths) and the editor (which needs GUIDs) work correctly.
    // Guard: m_MaterialCompiler is null when no Engine instance exists (e.g. tests).
    MaterialDocument resolved = doc;
    // Mask demotion mutates only this registration-local copy: the authored
    // document (and the MaterialAsset behind it) keeps its Mask, so editor
    // saves re-emit the authored mode and a texture that gains alpha
    // re-evaluates on the next (re)registration. Runs BEFORE the reconcile
    // below, which back-fills surfaceShaderGuid for registered builtin
    // surfaces — the gate must see the authored reference shape.
    DemoteMaskWithoutAlphaSource(resolved, guid);
    // Shares the reconcile ordering above for the same reason: the gate reads the
    // authored surface-reference shape, which the back-fill would overwrite. Runs
    // after the demotion so a material proven unable to discard — the exact
    // opposite finding — never also reports that it cannot draw.
    WarnIfMaterialCannotDraw(guid, resolved);
    if (m_MaterialCompiler)
        m_MaterialCompiler->ReconcileShaderReferences(resolved);

    Material* previousMat = m_RuntimeMaterialRegistry.Find(guid);
    const bool hadCompiledPipeline = previousMat
        && previousMat->GetGraphicsPipelineId().IsValid();
    const bool previousDoubleSided = previousMat ? previousMat->m_DoubleSided : false;
    const MaterialAlphaMode previousAlphaMode = previousMat
        ? previousMat->m_AlphaMode
        : MaterialAlphaMode::Opaque;
    const Rendering::ShaderVariantKey previousVariantKey = previousMat
        ? previousMat->GetVariantKey()
        : Rendering::ShaderVariantKey{};
    const MaterialCompileSpec previousCompileSpec = previousMat
        ? previousMat->GetCompileSpec()
        : MaterialCompileSpec{};
    // The fixed-function half of the PSO. Captured as the DERIVED state, not the
    // authored `blend`/`zWrite`/`zTest`, because the derivation is what the
    // pipeline consumes: an Opaque material's blend block never reaches it, so
    // comparing the raw fields would rebuild for an edit with no PSO effect.
    const Rendering::DerivedBlendState previousDerivedBlend = previousMat
        ? Rendering::DeriveMaterialBlendState(previousAlphaMode, previousMat->GetBlendState(),
                                              previousMat->GetDepthWriteOverride(),
                                              previousMat->GetDepthTestOverride())
        : Rendering::DerivedBlendState{};
    // Color-class inputs captured BEFORE Register() overwrites them in place, so
    // a re-registration that edits the PSO signature can be detected below.
    const bool previousColorMergeEligible = previousMat && IsColorMergeEligible(*previousMat);
    const ColorClassSignature previousColorClassSignature =
        previousMat ? ComputeColorClassSignature(*previousMat) : ColorClassSignature{};

    const auto tReg = Clock::now();
    Material* mat = m_RuntimeMaterialRegistry.Register(guid, resolved);
    const double regMs = msElapsed(tReg);

    bool materialAssetPathChanged = false;
    // The surface's declared texture slots, read once per registration: the Parallax keyword and
    // the material's slot map below both come from this table.
    std::optional<std::vector<std::pair<std::string, uint8_t>>> surfaceSlots;
    if (mat)
    {
        std::filesystem::path materialAssetPath;
        // Headless guard: without an initialized EngineCore (unit tests, tooling)
        // the AssetManager/registry are not constructed, so GetAsset would deref a
        // null registry. The compiler fallback below supplies the path instead.
        if (GameEngine::EngineCore::GetInstance().IsInitialized())
        {
            auto& am = GameEngine::EngineCore::GetInstance().GetAssetManager();
            if (auto asset = am.GetAsset(guid))
            {
                if (auto* matAsset = dynamic_cast<MaterialAsset*>(asset.get()))
                    materialAssetPath = matAsset->GetPath();
            }
            // Loaded-asset lookup can miss (load still in flight, asset ejected)
            // while the registry's startup scan already knows the material. The
            // registry path is absolute under the LIVE project mount, so this
            // keeps materialDir — and with it project-side surface-shader
            // resolution — anchored to the open project root instead of
            // collapsing to the synthetic-path fallback (the copied-project
            // portability bug: surfaces next to their .material stopped
            // resolving in a relocated project copy). First registration only:
            // runtime materials (particle emitters) re-register per frame, and
            // a metadata miss ends in a redirect-enumeration store probe that
            // must stay off that path. m_MaterialAssetPath is sticky, so a
            // re-registration keeps the recovered value.
            if (materialAssetPath.empty() && !previousMat)
            {
                AssetMetadata meta;
                if (am.GetRegistry().TryGetAssetMetadata(guid, meta))
                    materialAssetPath = meta.Path;
            }
        }
        if (materialAssetPath.empty() && m_MaterialCompiler)
        {
            if (auto compiled = m_MaterialCompiler->Get(guid))
                materialAssetPath = compiled->materialPath;
        }
        if (!materialAssetPath.empty())
        {
            materialAssetPathChanged = mat->m_MaterialAssetPath != materialAssetPath;
            mat->m_MaterialAssetPath = materialAssetPath;
        }
        surfaceSlots = ResolveSurfaceTextureSlots(resolved, mat->GetMaterialAssetPath());
        std::string parallaxRefusal;
        mat->m_VariantKey = DeriveRegistrationVariantKey(resolved, materialAssetPath, surfaceSlots, parallaxRefusal);
        // Every later compile rebuilds its document from the spec, which carries no texture
        // bindings; the derived decision rides the spec to reach it.
        mat->m_CompileSpec.parallax =
            Rendering::HasKeyword(mat->m_VariantKey.materialKeywords, Rendering::MaterialKeyword::Parallax);
        ReportParallaxRefusal(*mat, std::move(parallaxRefusal));
    }

    // Inject caller-provided keywords (e.g. ForwardPlus | Shadows for
    // forward contributors that always render in the Forward+ pass).
    if (mat && additionalKeywords != Rendering::MaterialKeyword::None)
        mat->m_VariantKey.materialKeywords |= additionalKeywords;

    double compileMs = 0.0;
    if (mat)
    {
        if (!mat->GetGraphicsPipelineId().IsValid())
        {
            // First registration — pipeline not built yet. In Async mode the
            // base-shader compile is handed to a worker so a cold scene load
            // doesn't block the main thread; the material stays skipped by the
            // draw stream until its pipeline publishes (BeginFrame). SubmitAsync-
            // BaseCompile self-falls-back to the inline compile when async can't
            // run (headless/test paths), so the branch stays simple.
            if (baseCompileMode == BaseCompileMode::Async)
            {
                SubmitAsyncBaseCompile(guid, *mat, resolved);
            }
            else
            {
                const auto tCompile = Clock::now();
                CompileMaterialPipeline(*mat, resolved);
                compileMs = msElapsed(tCompile);
            }
        }
        else
        {
            // Re-registration: rebuild only if pipeline-affecting state changed.
            const MaterialAlphaMode effectiveAlphaMode = GetEffectiveMaterialAlphaMode(resolved);
            // Register() has already written the authored overrides onto the
            // Material, and m_AlphaMode is assigned effectiveAlphaMode below, so
            // this is the state CompileMaterialPipeline is about to hand the PSO.
            const Rendering::DerivedBlendState derivedBlend = Rendering::DeriveMaterialBlendState(
                effectiveAlphaMode, mat->GetBlendState(), mat->GetDepthWriteOverride(),
                mat->GetDepthTestOverride());
            const bool pipelineAffectingChanged =
                !hadCompiledPipeline
                || (previousDoubleSided != resolved.doubleSided)
                || (previousAlphaMode != effectiveAlphaMode)
                || (previousDerivedBlend != derivedBlend)
                || !(previousVariantKey == mat->GetVariantKey())
                || (previousCompileSpec.surfaceShaderPath != mat->GetCompileSpec().surfaceShaderPath)
                || (previousCompileSpec.vertexModifierPath != mat->GetCompileSpec().vertexModifierPath)
                || (previousCompileSpec.lightingModel != mat->GetCompileSpec().lightingModel);
            mat->m_DoubleSided = resolved.doubleSided;
            mat->m_AlphaMode = effectiveAlphaMode;
            if (pipelineAffectingChanged)
            {
                const auto tCompile = Clock::now();
                CompileMaterialPipeline(*mat, resolved);
                compileMs = msElapsed(tCompile);
            }
        }
    }

    // Assign a stable MaterialParams SSBO index if this material is new.
    if (mat && mat->GetGpuSceneMaterialIndex() == Material::kInvalidSSBOIndex)
    {
        uint32_t idx;
        if (!m_FreeMaterialSSBOIndices.empty())
        {
            idx = m_FreeMaterialSSBOIndices.back();
            m_FreeMaterialSSBOIndices.pop_back();
        }
        else
        {
            idx = m_NextMaterialSSBOIndex++;
        }
        mat->m_GpuSceneMaterialIndex = idx;

        if (idx >= m_MaterialsBySSBOIndex.size())
            m_MaterialsBySSBOIndex.resize(idx + 1, nullptr);
        m_MaterialsBySSBOIndex[idx] = mat;
        ++m_MaterialSSBOGeneration;
        // Bindless defaults are seeded by MaterialRegistry::Register the
        // moment the Material is constructed, so every observable Material
        // has valid bindless indices for all 8 slots — no need to re-seed here.

        // A real materialIndex must never reach the shadow table's class
        // sentinels (top of the 24-bit sort domain) or an eligible caster
        // would collide with a class group. maxMaterials is 10k, so this is a
        // guard, not a live limit.
        assert(idx < Rendering::GPUDrawStreamBuilder::kSharedDepthDoubleSidedSentinel
               && "materialIndex collides with a shadow-depth class sentinel");
    }

    // Refresh the depth-batch class on every register AND re-register: material
    // edits that flip alpha mode / double-sided / vertex-modifier / transmission
    // change how the caster batches in shadow, and extraction's flags bits ride
    // the R1.1 skip gate (LastFlags) so the change re-uploads the instance.
    if (mat)
    {
        const uint32_t idx = mat->GetGpuSceneMaterialIndex();
        if (idx != Material::kInvalidSSBOIndex)
        {
            if (idx >= m_MaterialDepthClass.size())
                m_MaterialDepthClass.resize(
                    idx + 1, static_cast<uint8_t>(Rendering::MaterialDepthClass::MaterialDependent));
            m_MaterialDepthClass[idx] = static_cast<uint8_t>(ClassifyMaterialDepthClass(*mat));
            // Same edge, same reasons: a modifier reference or an alpha mode
            // added or removed by a re-registration moves the material into or
            // out of the deformer lane.
            if (idx >= m_MaterialDeformationMotion.size())
                m_MaterialDeformationMotion.resize(idx + 1, 0u);
            m_MaterialDeformationMotion[idx] = SupportsDeformationMotion(*mat) ? 1u : 0u;
        }
    }

    // The color class needs the same "on register AND re-register" treatment, but
    // it is a cached whole-array build rather than a per-material byte, and its
    // cache is keyed on m_MaterialSSBOGeneration — which moves only when an index
    // is assigned or freed. A re-registration keeps its index, so an in-place edit
    // to alphaMode / doubleSided / ignoreVertexColor / a keyword / the compile spec
    // would leave the cached array describing the material's OLD PSO signature and
    // merge it into a batch that binds the wrong pipeline. A first registration is
    // already covered by the generation bump, so only the edit case is tested here.
    // Comparing the signature itself (rather than reusing pipelineAffectingChanged,
    // which does not track ignoreVertexColor) keeps the rebuild exact: no stale
    // array, and no rebuild for the per-frame re-registrations that change nothing.
    if (mat && previousMat
        && (IsColorMergeEligible(*mat) != previousColorMergeEligible
            || !(ComputeColorClassSignature(*mat) == previousColorClassSignature)))
    {
        InvalidateMaterialColorClassMap();
    }

    // Resolve texture GUIDs to GPU handles.
    double texResolveMs = 0.0;
    if (mat)
    {
        // Named texture slots: hand the Material the surface's resolved
        // name->slot map ONCE, before the clear/bind loops below, so user texture
        // names route to the ordinal the shader compiled against. Legacy surfaces
        // (no @texture tags) get an empty map and keep the fixed ladder; a failed
        // resolution leaves the existing map for the still-live pipeline.
        if (surfaceSlots)
            mat->SetTextureSlotMap(std::move(*surfaceSlots));

        // Declared properties: the packed lanes the composed shader reads. Installed
        // here, once the material's directory is known, from the same cached parse
        // the composer uses; the document re-apply below writes through them.
        //
        // The resolve is filesystem work (an exists() probe per shader root, then a
        // stat per source file) and registration is a per-frame path for a runtime
        // material — a smoke emitter re-registers its document every frame. The
        // table can only change with the surface or vertex-modifier reference, the
        // material's directory, or a shader-source edit, and a recompile re-applies
        // it as its pipeline binds (CompileMaterialPipeline, the publish drain); a
        // re-registration that changes none of them keeps the table it has. A
        // material whose first registration ran before the build context was ready
        // has no pipeline yet, and resolves again.
        const bool declaredInputsUnchanged =
            hadCompiledPipeline && !materialAssetPathChanged
            && previousCompileSpec.surfaceShaderPath == mat->GetCompileSpec().surfaceShaderPath
            && previousCompileSpec.vertexModifierPath == mat->GetCompileSpec().vertexModifierPath;
        if (!declaredInputsUnchanged)
        {
            m_RuntimeMaterialRegistry.ApplyPropertyTable(
                *mat, ResolveDeclaredProperties(resolved, mat->GetMaterialAssetPath()));
        }

        // Route through MarkDirty (same-value guarded): the sampler nibble is
        // packed into the MaterialParams SSBO, so a change must invalidate the
        // pack skip stamp.
        if (const auto preset = SamplerPresetFromMaterialFilter(resolved.textureFilter);
            mat->m_SamplerPreset != preset)
        {
            mat->m_SamplerPreset = preset;
            mat->MarkDirty();
        }

        // Drop bindings for slots no longer present in the new document. The
        // texture-resolve loop below only ADDS bindings; without this, a
        // hot-reload that removes a slot (e.g. dropping `normalMap`) leaves
        // the old TextureHandle and bindless index in place, so the material
        // keeps sampling the stale texture instead of the default fallback.
        // (For a name whose @texture declaration vanished, only the handle and
        // ref clear — the by-name default repaint no-ops since the name no
        // longer resolves; the stale ordinal is unreachable by name, and user
        // ordinals are re-seeded white by SetTextureSlotMap.)
        // A texGuidStr of "" or one that fails to parse counts as "removed"
        // — same handling as UpdateMaterialTextures (line ~3777).
        {
            std::unordered_set<StringId> liveSlots;
            liveSlots.reserve(resolved.textures.size());
            for (const auto& [texName, texGuidStr] : resolved.textures)
            {
                if (texGuidStr.empty())
                    continue;
                const StringId nameId = HashStringId(texName);
                // A key that resolves to no slot is not live — an unknown
                // document key (skipped with a warning in the bind loop
                // below) or a binding whose @texture declaration was removed;
                // either way an existing binding under it must clear.
                if (!mat->HasTextureSlot(nameId))
                    continue;
                // Treat embedded refs (non-empty texGuidStr that doesn't
                // parse as a GUID) as live — ResolveEmbeddedTextures binds
                // them after this function returns.
                liveSlots.insert(nameId);
            }

            std::vector<StringId> slotsToClear;
            for (const auto& [slotName, _] : mat->GetTextureBindings())
            {
                if (liveSlots.find(slotName) == liveSlots.end())
                    slotsToClear.push_back(slotName);
            }

            for (StringId slotName : slotsToClear)
            {
                mat->SetTexture(slotName, Rendering::TextureHandle{});
                mat->SetBindlessTextureIndex(slotName, m_Textures->ResolveDefaultBindlessIndex(slotName));
                m_Textures->UntrackMaterialTextureRef(guid, slotName);
            }
        }

        const auto tTex = Clock::now();
        // A key repaired by document edit is never visited by validation
        // again; prune its warned entry to the current document so a
        // same-session relapse reports again (empty-warned early-out keeps
        // the per-frame emitter re-registration free).
        mat->PruneTextureKeyWarnings(resolved.textures);
        for (const auto& [texName, texGuidStr] : resolved.textures)
        {
            // Where the document meets the slot tables: a key that names no
            // slot is skipped here, before any ref is tracked — a tracked bad
            // name would replay into the bindless slot table when the texture
            // decode completes.
            if (!mat->ValidateDocumentTextureKey(texName))
                continue;
            if (texGuidStr.empty())
                continue;
            auto texPathIt = resolved.texturePaths.find(texName);
            GUID texGuid = m_Textures->ResolveTextureRefGuid(
                texGuidStr, texPathIt != resolved.texturePaths.end() ? texPathIt->second : std::string());
            StringId nameId = HashStringId(texName);
            if (texGuid.IsNull())
            {
                // An embedded image binds in ResolveEmbeddedTextures after this
                // returns; any other reference that resolves to no asset failed.
                if (!IsEmbeddedTextureRef(texGuidStr))
                    m_Textures->BindAwaitedTexture(mat, nameId);
                continue;
            }

            m_Textures->BindMaterialTextureRef(mat, guid, nameId, texGuid);
        }
        texResolveMs = msElapsed(tTex);

        // Re-apply mutable document state (properties + UV transforms) on every
        // call. MaterialRegistry::Register populates these on first creation but
        // returns early on re-registration, so without this re-apply, opening a
        // scene that uses an already-registered material would render with stale
        // values until something forced a state refresh (e.g. toggling
        // DoubleSided, which rebuilds the PSO and re-binds material data).
        ApplyDocumentPropertiesToMaterial(*mat, resolved);
        SyncMaterialOpacityFromDocument(*mat, resolved);

        // Routed like the bindings, now that the surface's slot map is installed: a surface's
        // own texture name (heightMap on the standard surface) carries its tiling on the ordinal
        // it binds to.
        for (const auto& [texName, st] : resolved.textureTransforms)
            mat->SetTextureTransform(HashStringId(texName), st);
    }

    const double totalMs = msElapsed(tStart);
    if (totalMs > 1.0)
    {
        Logger::Log::Info(
            "[ModelLoad]   RegisterMaterial '{}': {:.1f}ms (registry {:.1f}ms, compile {:.1f}ms, texResolve {:.1f}ms)",
            mat ? mat->GetName() : "null", totalMs, regMs, compileMs, texResolveMs);
    }

    return mat;
}

// The effective `uBaseColor.a` the fragment stage multiplies into opacity.
// `opacity` and `baseColor.a` are two spellings of one quantity:
// SyncMaterialOpacityFromDocument folds the property into baseColor.a at
// registration (flooring it at 0), so when the key is present it IS the
// multiplier. nullopt means the document shape is unexpected, so nothing about
// the alpha can be proven either way.
static std::optional<float> DocumentEffectiveBaseAlpha(const MaterialDocument& doc)
{
    float baseAlpha = 1.0f;
    if (auto it = doc.properties.find("baseColor"); it != doc.properties.end())
    {
        const auto* v = std::get_if<std::vector<float>>(&it->second);
        if (!v || v->size() < 4)
            return std::nullopt;
        baseAlpha = (*v)[3];
    }
    if (auto it = doc.properties.find("opacity"); it != doc.properties.end())
    {
        if (const float* o = std::get_if<float>(&it->second))
            baseAlpha = std::max(0.0f, *o);
        else if (const int32_t* oi = std::get_if<int32_t>(&it->second))
            baseAlpha = std::max(0.0f, static_cast<float>(*oi));
        else
            return std::nullopt;
    }
    return baseAlpha;
}

// The authored cutoff, verbatim — callers that need the value the shader tests
// against must clamp it to [0,1] themselves. nullopt means the property is
// present but not a float, so the authored value cannot be read.
static std::optional<float> DocumentAuthoredAlphaCutoff(const MaterialDocument& doc)
{
    if (auto it = doc.properties.find("alphaCutoff"); it != doc.properties.end())
    {
        const auto* v = std::get_if<float>(&it->second);
        if (!v)
            return std::nullopt;
        return *v;
    }
    return kDefaultAlphaCutoff;
}

// Mask that provably cannot discard is Opaque by definition: the alpha test
// compares opacity = albedoAlpha * uBaseColor.a * vertexColor.a against the
// cutoff, so when the albedo texture encodes no alpha channel (or no texture
// is bound) and the multipliers sit at 1, no fragment can ever fail the test.
// Converters that blanket-set Mask (observed: Unity scene imports over Synty
// RGB atlases) otherwise force discard-mode depth rasterization and
// material-dependent shadow batching on fully opaque geometry. Only provable
// cases demote; any doubt (unknown format, unresolvable texture, attenuating
// multipliers, custom surfaces or graphs, vertex-color variants) keeps the
// authored Mask.
void MaterialSystem::DemoteMaskWithoutAlphaSource(MaterialDocument& doc, const GUID& logAs)
{
    constexpr float kOpaqueBaseAlphaMin = 0.999f;
    constexpr float kMaxDemotableAlphaCutoff = 0.99f;

    if (doc.alphaMode != MaterialAlphaMode::Mask)
        return;

    // A surface graph compiles as THE surface regardless of what surfaceShader
    // currently says (ResolveSurfaceShaderFromGraph overwrites it downstream),
    // and graph opacity can be procedural — its presence disqualifies outright.
    if (!doc.surfaceGraph.empty() || !doc.surfaceGraphGuid.empty())
        return;
    // Only surfaces whose opacity is provably albedo.a * uBaseColor.a *
    // vertexColor.a — custom surface shaders may derive opacity elsewhere.
    const bool analyzableSurface =
        doc.surfaceShader == "Surfaces/standard_pbr.glsl" ||
        doc.surfaceShader == "Surfaces/standard_pbr_extended.glsl";
    if (!analyzableSurface || !doc.surfaceShaderGuid.empty())
        return;
    // Without ignoreVertexColor the variant samples vertex alpha, which can
    // attenuate opacity below the cutoff on meshes with COLOR_0.
    if (!doc.ignoreVertexColor)
        return;

    const std::optional<float> baseAlphaOpt = DocumentEffectiveBaseAlpha(doc);
    if (!baseAlphaOpt || *baseAlphaOpt < kOpaqueBaseAlphaMin)
        return;
    const float baseAlpha = *baseAlphaOpt;

    const std::optional<float> cutoffOpt = DocumentAuthoredAlphaCutoff(doc);
    if (!cutoffOpt || *cutoffOpt > kMaxDemotableAlphaCutoff)
        return;
    const float cutoff = *cutoffOpt;

    if (const auto texIt = doc.textures.find("albedoMap");
        texIt != doc.textures.end() && !texIt->second.empty())
    {
        // Resolve exactly as the texture binder will (ResolveTextureRefGuid:
        // redirect-chase, then source-relative path re-derive), so the probed
        // file and the bound texture can never diverge. Headless (no engine /
        // no registry) keeps Mask.
        if (!m_Textures || !GameEngine::EngineCore::GetInstance().IsInitialized())
            return;
        const auto pathIt = doc.texturePaths.find("albedoMap");
        const GUID texGuid = m_Textures->ResolveTextureRefGuid(
            texIt->second,
            pathIt != doc.texturePaths.end() ? pathIt->second : std::string());
        if (texGuid.IsNull())
            return; // embedded ref or unparseable — keep Mask
        auto& am = GameEngine::EngineCore::GetInstance().GetAssetManager();
        AssetMetadata metadata;
        if (!am.GetRegistry().TryGetAssetMetadata(texGuid, metadata) || metadata.Path.empty())
        {
            Logger::Log::Debug("Material '{}': Mask kept — albedo guid '{}' unresolved",
                               doc.materialName, texIt->second);
            return;
        }
        std::filesystem::path texturePath = metadata.Path;
        if (texturePath.is_relative())
            texturePath = am.ResolveAssetPath(texturePath);
        if (texturePath.empty() || !ProbedFileCannotDiscard(texturePath, cutoff))
        {
            Logger::Log::Debug("Material '{}': Mask kept — albedo '{}' has (or may have) alpha "
                               "below cutoff {:.3f}",
                               doc.materialName, texturePath.string(), cutoff);
            return;
        }
    }
    // else: no albedo texture — the default 1x1 white texture samples alpha 1.

    doc.alphaMode = MaterialAlphaMode::Opaque;
    if (logAs.IsNull() || !m_LoggedMaskDemotions.insert(logAs).second)
        return;
    // The gate inputs ride the line so a demotion can be audited from the log
    // alone (registration-time demotion has no persisted artifact to inspect).
    const auto albedoIt = doc.textures.find("albedoMap");
    Logger::Log::Info(
        "Material '{}': demoted Mask to Opaque (alpha source cannot discard; "
        "baseAlpha={:.4f} cutoff={:.3f} albedo='{}')",
        doc.materialName, baseAlpha, cutoff,
        albedoIt != doc.textures.end() ? albedoIt->second : std::string("<none>"));
}

namespace
{
// The inputs behind a "cannot draw" report, kept so the log line can quote the
// two numbers the author has to reconcile.
struct AlwaysDiscardingMask
{
    float BaseAlpha = 0.0f;
    float Cutoff    = 0.0f; // clamped to [0,1] exactly as the shader clamps it
};

// A Mask document whose opacity provably never reaches its own cutoff, so the
// alpha test discards every fragment it is ever asked about.
//
// The proof needs no texel decode: opacity = albedo.a * uBaseColor.a *
// vertexColor.a, and with albedo.a bounded by 1 (UNORM) and vertexColor.a
// pinned to 1, uBaseColor.a is the upper bound of the whole product. A bound
// below the cutoff means the strict `<` in the fragment stage is always true.
//
// Every gate below is load-bearing for that bound, not caution:
//  - a surface graph or a custom surface shader may derive opacity from
//    anything, so albedo.a * uBaseColor.a stops being the product;
//  - vertex colour is R32G32B32A32_FLOAT and therefore UNBOUNDED, so a mesh
//    with COLOR_0 can multiply opacity back above the cutoff. Only
//    ignoreVertexColor (which strips HAS_COLOR, leaving surface_io.glsl's
//    vec4(1) default) pins that factor to 1.
std::optional<AlwaysDiscardingMask>
ProvenAlwaysDiscardingMask(const MaterialDocument& doc)
{
    // Gate on the EFFECTIVE mode: the AlphaTest keyword (and with it the
    // discard) is derived from it, and ShadowOnly forces Blend.
    if (GetEffectiveMaterialAlphaMode(doc) != MaterialAlphaMode::Mask)
        return std::nullopt;
    if (!doc.surfaceGraph.empty() || !doc.surfaceGraphGuid.empty())
        return std::nullopt;
    const bool analyzableSurface =
        doc.surfaceShader == "Surfaces/standard_pbr.glsl" ||
        doc.surfaceShader == "Surfaces/standard_pbr_extended.glsl";
    if (!analyzableSurface || !doc.surfaceShaderGuid.empty())
        return std::nullopt;
    if (!doc.ignoreVertexColor)
        return std::nullopt;

    const std::optional<float> baseAlpha = DocumentEffectiveBaseAlpha(doc);
    const std::optional<float> authoredCutoff = DocumentAuthoredAlphaCutoff(doc);
    if (!baseAlpha || !authoredCutoff)
        return std::nullopt;
    // Authored floats reach here unvalidated, NaN included. Every ordered
    // comparison against NaN is false, so a non-finite input would silently
    // read as "draws fine"; rejecting it explicitly keeps that honest.
    if (!std::isfinite(*baseAlpha) || !std::isfinite(*authoredCutoff))
        return std::nullopt;

    const float cutoff = std::clamp(*authoredCutoff, 0.0f, 1.0f);
    if (*baseAlpha >= cutoff)
        return std::nullopt;
    return AlwaysDiscardingMask{*baseAlpha, cutoff};
}
} // namespace

void MaterialSystem::WarnIfMaterialCannotDraw(const GUID& guid, const MaterialDocument& doc)
{
    const std::optional<AlwaysDiscardingMask> failing = ProvenAlwaysDiscardingMask(doc);
    if (!failing)
    {
        // Cleared on recovery so a material edited back into the failing state
        // reports again instead of being suppressed by the stale entry.
        m_CannotDrawWarnedMaterials.erase(guid);
        return;
    }
    // Runtime materials (particle emitters) re-register every frame, so the
    // report is once per material per entry into the failing state.
    if (!m_CannotDrawWarnedMaterials.insert(guid).second)
        return;

    Logger::Log::Warning(
        "Material '{}' can never draw: alphaMode=Mask discards every fragment because its "
        "opacity ({:.4f}) is below alphaCutoff ({:.3f}), and opacity cannot exceed it "
        "(opacity = baseColor.a * albedoMap.a, and albedoMap.a <= 1). Fix by one of: raise "
        "baseColor.a/opacity above {:.3f}, lower alphaCutoff below {:.4f}, or set alphaMode "
        "to Opaque or Blend.",
        doc.materialName, failing->BaseAlpha, failing->Cutoff, failing->Cutoff,
        failing->BaseAlpha);
}

bool MaterialSystem::ProbedFileCannotDiscard(const std::filesystem::path& path, float cutoff)
{
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (ec)
        return false;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec)
        return false;

    const auto verdictFor = [cutoff](const AlphaProbeCacheEntry& entry) {
        // Tier 1: no alpha channel encoded at all, so the decoder fills alpha
        // with 255 and there is no authored cutout for the cook to erase. The
        // cooked value is NOT bit-exactly 255 — BC7 reconstructs even a
        // constant alpha channel to within a step or two, and adversarial RGB
        // has cost as much as 57 (measured) — but the failure
        // direction is benign here in a way it is not for tier 2: there is no
        // transparency to lose, so Opaque is the correct mode at any cutoff and
        // no margin applies. This is the pre-existing verdict and must keep
        // demoting at any cutoff.
        if (entry.NoAlphaSource)
            return true;
        // Tier 2: the channel exists, but every texel provably clears the
        // cutoff by more than the cooked upload can fall (the probe only
        // answers at all for containers whose cooked delta is bounded — see
        // kCookedAlphaDropBound). A cutoff that leaves no room for the bound
        // yields no threshold, and no threshold means no proof.
        if (!entry.MinAlpha)
            return false;
        const std::optional<uint32_t> threshold =
            AlphaCutoffOpaqueThreshold(cutoff, kCookedAlphaDropBound);
        return threshold && static_cast<uint32_t>(*entry.MinAlpha) >= *threshold;
    };

    const std::string key = path.string();
    {
        std::lock_guard<std::mutex> lock(m_AlphaProbeCacheMutex);
        if (auto it = m_AlphaProbeCache.find(key);
            it != m_AlphaProbeCache.end() && it->second.MTime == mtime && it->second.Size == size)
            return verdictFor(it->second);
    }

    AlphaProbeCacheEntry entry{mtime, size, false, std::nullopt};
    entry.NoAlphaSource = ProbeTextureFileAlpha(path) == TextureAlphaContent::NoAlphaSource;
    // The decode tier only earns its cost where the header tier is blind: a
    // declared alpha channel whose content may still be uniformly opaque.
    if (!entry.NoAlphaSource)
    {
        const TextureAlphaProbeResult probe = ProbeTextureMinAlphaFromFile(path);
        // Deferred says nothing about the file — the decode concurrency cap was
        // full at this instant. Caching it would freeze a moment of contention
        // into a permanent "keeps Mask" for a texture that will probe fine, so
        // this registration keeps Mask and leaves the cache alone; the next
        // registration of any material sharing this texture re-probes.
        if (probe.Status == TextureAlphaProbeStatus::Deferred)
        {
            Logger::Log::Debug("Alpha decode probe '{}': deferred (decode cap full) — keeps Mask, "
                               "not cached", path.string());
            return false;
        }
        if (probe.Status == TextureAlphaProbeStatus::Answered)
            entry.MinAlpha = probe.MinAlpha;
        // Once per texture per mtime (this is the cache-miss path), so the
        // decode verdict behind every tier-2 demotion is auditable from the log.
        if (entry.MinAlpha)
            Logger::Log::Info("Alpha decode probe '{}': min alpha {}/255", path.string(),
                              static_cast<uint32_t>(*entry.MinAlpha));
        else
            Logger::Log::Debug("Alpha decode probe '{}': undecodable within bounds — keeps Mask",
                               path.string());
    }

    std::lock_guard<std::mutex> lock(m_AlphaProbeCacheMutex);
    m_AlphaProbeCache[key] = entry;
    return verdictFor(entry);
}

void MaterialSystem::EnsureMaterialColorClassMap()
{
    // The color-class merge is always on (P2b, verified byte-identical +
    // pixel-equivalent with real transmissive content). An empty map is still
    // the degenerate OFF signal downstream, reached only when there are zero
    // materials (nothing to draw); every real material set builds the merge.
    // The generation covers the index mapping; RegisterMaterialFromDocument
    // additionally invalidates when a re-registration edits a live material's
    // signature, which the generation alone cannot see.
    if (m_MaterialColorClassGeneration == m_MaterialSSBOGeneration
        && m_MaterialColorClass.size() == m_MaterialsBySSBOIndex.size())
        return; // material set unchanged since the last build

    m_MaterialColorClassGeneration = m_MaterialSSBOGeneration;

    const size_t count = m_MaterialsBySSBOIndex.size();
    m_MaterialColorClass.assign(count, 0u);

    // Assign a dense, downward-allocated colorClassId per distinct PSO
    // signature. Iterating in ascending materialIndex makes both the id set and
    // the lowest-index-per-class representative deterministic. Eligible ==
    // shared-depth eligible so the merge is shadow-safe (MaterialColorClassify.h).
    std::unordered_map<ColorClassSignature, uint32_t> signatureToId;
    uint32_t nextOrdinal = 0u;
    for (uint32_t idx = 0; idx < static_cast<uint32_t>(count); ++idx)
    {
        const Material* mat = m_MaterialsBySSBOIndex[idx];
        if (!mat || !IsColorMergeEligible(*mat))
        {
            m_MaterialColorClass[idx] = idx; // identity: keeps its (mat, mesh) row
            continue;
        }
        const ColorClassSignature sig = ComputeColorClassSignature(*mat);
        auto [it, inserted] = signatureToId.try_emplace(sig, 0u);
        if (inserted)
            it->second = Rendering::GPUDrawStreamBuilder::kColorClassBase - nextOrdinal++;
        m_MaterialColorClass[idx] = it->second;
    }

    // Domain-disjointness guard (P2-a): the lowest class id must stay above
    // every real materialIndex (identity keys are <= count-1), or a class group
    // would alias an identity row in the shared cascade=None table.
    // BuildColorBatchTable re-checks per built table; maxMaterials ~10k vs a
    // handful of PSO classes, so this is a guard, not a live limit.
    assert((count == 0u || nextOrdinal == 0u
            || static_cast<uint32_t>(count - 1)
                   < Rendering::GPUDrawStreamBuilder::kColorClassBase - (nextOrdinal - 1u))
           && "color class ids collide with the real materialIndex domain (P2-a)");
}

std::span<const uint32_t> MaterialSystem::MaterialColorClassSpan()
{
    EnsureMaterialColorClassMap();
    return m_MaterialColorClass;
}

void MaterialSystem::RecompileMaterialPipeline(const GUID& guid, const MaterialDocument& doc)
{
    assert(std::this_thread::get_id() == m_OwnerThreadId
           && "RecompileMaterialPipeline is main-thread-only (retires the pipeline id unlocked)");
    Material* mat = m_RuntimeMaterialRegistry.Find(guid);
    if (!mat || !m_Device)
        return;

    MaterialDocument resolved = doc;
    if (m_MaterialCompiler)
        m_MaterialCompiler->ReconcileShaderReferences(resolved);

    ShaderCacheKey cacheKey{};
    cacheKey.VariantKey = mat->GetVariantKey();
    cacheKey.Source.SurfaceShaderPath = resolved.surfaceShader;
    cacheKey.Source.VertexModifierPath = resolved.vertexModifier;
    cacheKey.Source.MaterialAssetPath = mat->GetMaterialAssetPath();

    // The in-memory cache is keyed by paths, not bytes: a content edit behind an
    // unchanged path must invalidate here or the worker would re-serve the old
    // compile. (The on-disk layer hashes content and needs no help.)
    m_ShaderCompilationCache.InvalidateGlobalEntry(cacheKey);
    m_ShaderCompilationCache.ClearVariantsForMaterial(mat);
    if (auto slots = ResolveSurfaceTextureSlots(resolved, mat->GetMaterialAssetPath()))
        mat->SetTextureSlotMap(std::move(*slots));

    // Retire the current pipeline: the draw stream skips the material until the
    // publish, and the publish guard itself requires an invalid id to apply.
    // Same write-safety envelope as ApplyPendingPipelinePublishes (F4): main
    // thread, before the parallel record window opens. Keep the retired id: a
    // FAILED compile restores it (fail-visible — the mesh keeps rendering the
    // last good shader). The stash is safe across a device rebuild because
    // interned ids survive it: OnDeviceRebuilt clears only concrete pipelines
    // (ClearConcreteOnly keeps the intern tables), and the warm probe
    // liveness-checks concretes via IsPipelineAlive, so a restored id lazily
    // rebuilds its backend state on the healthy device.
    const Rendering::GraphicsPipelineId lastGoodId = mat->m_GraphicsPipelineId;
    mat->m_GraphicsPipelineId = {};
    {
        std::lock_guard lock(*mat->m_PerFlagsIdMutex);
        mat->m_PerFlagsId.clear();
    }
    ++mat->m_Version;

    if (const auto it = m_BaseCompileInFlight.find(guid); it != m_BaseCompileInFlight.end())
    {
        // An earlier submit is still compiling — possibly against the bytes this
        // edit just replaced, which the cache key cannot distinguish. Let it
        // finish, then discard its result and re-drive at publish.
        it->second.RedriveAfterPublish = true;
        // A synchronous compile may have published since that submit; an id
        // that was valid at entry is the freshest last-good.
        if (lastGoodId.IsValid())
            it->second.LastGoodPipelineId = lastGoodId;
        return;
    }
    SubmitAsyncBaseCompile(guid, *mat, resolved);
    if (const auto it = m_BaseCompileInFlight.find(guid); it != m_BaseCompileInFlight.end())
    {
        // Async submit accepted — stash the retired id for the publish drain's
        // failed-compile restore.
        it->second.LastGoodPipelineId = lastGoodId;
    }
    else if (!mat->GetGraphicsPipelineId().IsValid())
    {
        // The submit fell back to the inline compile (headless/tests) and that
        // compile FAILED — restore the retired id here; there is no publish
        // record to restore it from.
        mat->m_GraphicsPipelineId = lastGoodId;
    }
}

bool MaterialSystem::EnsureMaterialBuildContextReady()
{
    // Lazy-init from the asset mount table. The asset sources can be registered
    // after Engine init (e.g. on macOS the user-copy seeding stage runs before
    // the asset manager mounts), so any path that hands m_MaterialBuildContext
    // to ShaderCompilationCache must call this first.
    if (m_MaterialBuildContext.AdapterShaderDir.empty())
    {
        auto& engine = GameEngine::EngineCore::GetInstance();
        if (!engine.IsInitialized())
            return false; // No Engine instance (e.g. unit-test harness).
        auto& am = engine.GetAssetManager();
        m_MaterialBuildContext.AdapterShaderDir = ResolveAdapterShaderDir(am);
        // The bundle cache root wins when one is set: a compiler-less runtime
        // (web) ships its cooked variants beside the binary, not under the
        // workspace.
        const std::filesystem::path& shaderCacheRoot = ShaderProgramAsset::GetShaderCacheRoot();
        m_MaterialBuildContext.CacheRoot = shaderCacheRoot.empty()
            ? engine.GetWorkspaceRoot() / ".Cache" / "Shaders"
            : shaderCacheRoot;
        m_MaterialBuildContext.IncludeDirs = {am.GetAssetRoot()};
        m_MaterialBuildContext.ProjectRoots = CollectProjectRoots(am);
        m_MaterialBuildContext.AssetSourceRoots = CollectAssetSourceRoots(am);
        m_MaterialBuildContext.PackageShaderDirs = CollectPackageShaderDirs(am);
        AppendStagedModuleShaderDirs(m_MaterialBuildContext.AdapterShaderDir,
                                     m_MaterialBuildContext.PackageShaderDirs);
        m_MaterialBuildContextSourceVersion = am.GetSourceSetVersion();
    }
    else if (m_MaterialBuildContextSourceVersion != 0
             && GameEngine::EngineCore::GetInstance().IsInitialized())
    {
        // The mount table moved since the snapshot (a package mounted or
        // unmounted after the first compile — normally project open completes
        // mounts first, but the order is not a contract). Re-derive the
        // mount-derived state; project switches reset the whole context.
        // version==0 means the context was injected via SetMaterialBuildContext
        // (test seam) rather than self-derived — never touch those.
        auto& am = GameEngine::EngineCore::GetInstance().GetAssetManager();
        const uint64_t version = am.GetSourceSetVersion();
        if (version != m_MaterialBuildContextSourceVersion)
        {
            m_MaterialBuildContext.ProjectRoots = CollectProjectRoots(am);
            m_MaterialBuildContext.AssetSourceRoots = CollectAssetSourceRoots(am);
            m_MaterialBuildContext.PackageShaderDirs = CollectPackageShaderDirs(am);

            // AdapterShaderDir is mount-derived too, and it is NEVER empty:
            // ResolveAdapterShaderDir falls back to the implicit overload, which
            // returns the project candidate whether or not it exists. So a first
            // derive that runs before the 'editor' source registers latches a
            // placeholder that no later call would revisit — the editor mounts
            // its project source well before its editor source. Re-derive here,
            // and when the root actually MOVES drop everything compiled against
            // the old one: Clear() also advances the invalidation epoch, which
            // bars an already-elected compile from publishing old-root SPIR-V.
            std::filesystem::path adapterDir = ResolveAdapterShaderDir(am);
            if (adapterDir != m_MaterialBuildContext.AdapterShaderDir)
            {
                m_MaterialBuildContext.AdapterShaderDir = std::move(adapterDir);
                m_AdapterShaderDirVerified = false;
                m_ShaderCompilationCache.Clear();
            }

            // After the re-derive, never before: the staged module roots hang off
            // whatever AdapterShaderDir now is.
            AppendStagedModuleShaderDirs(m_MaterialBuildContext.AdapterShaderDir,
                                         m_MaterialBuildContext.PackageShaderDirs);

            m_MaterialBuildContextSourceVersion = version;
            // The lane roots just moved, so the coverage report from the first
            // warm no longer describes them. Re-report — this fires only when
            // mounts actually change (project open/close, package mount), so a
            // session that completes its mounts before the first compile still
            // logs exactly once.
            if (m_AdapterShaderDirVerified)
                LogShaderEditWatchCoverage();
        }
    }

    // Readiness is the directory EXISTING, not the path being non-empty:
    // ResolveAdapterShaderDir always yields a path, so a wrong-but-resolvable
    // root must fail here rather than reach a compile. Verified once per root —
    // fs::exists() is ~0.5-1ms in Debug and previously dominated material-compile
    // overhead — and re-armed above whenever the root moves.
    if (!m_AdapterShaderDirVerified)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::exists(m_MaterialBuildContext.AdapterShaderDir, ec))
        {
            Logger::Log::Warning(
                "MaterialBuildContext: adapter shaders not found at '{}'. In the editor the "
                "'editor' asset source may not be mounted yet (this resolves itself once it "
                "is); in a packaged game the build did not stage Assets/Shaders/.",
                m_MaterialBuildContext.AdapterShaderDir.string());
            return false;
        }
        m_AdapterShaderDirVerified = true;
        // First moment the mount table is derived, so the first honest moment to
        // say which shader roots the save->recompile lane can see. This is a
        // coverage MANIFEST, not a failure report: an edit in an unwatched root
        // fires no ContentEvent at all, so no miss can be reported when it
        // happens — the manifest is what lets someone diagnose it afterwards.
        LogShaderEditWatchCoverage();
    }
    return true;
}

void MaterialSystem::LogShaderEditWatchCoverage() const
{
    auto& engine = GameEngine::EngineCore::GetInstance();
    if (!engine.IsInitialized())
        return; // injected-context paths (tests) have no mount table to report on

    // A lane root is hot-editable iff it sits under a mount whose file watcher
    // is subscribed (AssetManager::RegisterSource). Lexical prefix check on
    // normalized generic paths, case-insensitive (Windows mounts).
    auto normalize = [](const std::filesystem::path& p)
    {
        std::string s = p.lexically_normal().generic_string();
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    struct WatchRoot
    {
        std::string Alias;
        std::string NormalizedRoot;
    };
    std::vector<WatchRoot> watched;
    for (const AssetSourceDesc& source : engine.GetAssetManager().GetRegisteredSources())
    {
        if (source.RegisterFileWatcher)
            watched.push_back({source.Alias, normalize(source.Root)});
    }
    auto watchedBy = [&](const std::filesystem::path& root) -> const WatchRoot*
    {
        const std::string r = normalize(root);
        for (const WatchRoot& w : watched)
        {
            if (r.size() >= w.NormalizedRoot.size()
                && r.compare(0, w.NormalizedRoot.size(), w.NormalizedRoot) == 0
                && (r.size() == w.NormalizedRoot.size() || r[w.NormalizedRoot.size()] == '/'))
                return &w;
        }
        return nullptr;
    };

    std::vector<std::filesystem::path> laneRoots;
    laneRoots.push_back(m_MaterialBuildContext.AdapterShaderDir);
    laneRoots.insert(laneRoots.end(), m_MaterialBuildContext.ProjectRoots.begin(),
                     m_MaterialBuildContext.ProjectRoots.end());
    laneRoots.insert(laneRoots.end(), m_MaterialBuildContext.PackageShaderDirs.begin(),
                     m_MaterialBuildContext.PackageShaderDirs.end());

    std::string covered;
    std::string uncovered;
    for (const auto& root : laneRoots)
    {
        if (root.empty())
            continue;
        const WatchRoot* w = watchedBy(root);
        std::string& bucket = w ? covered : uncovered;
        if (!bucket.empty())
            bucket += ", ";
        bucket += "'" + root.generic_string() + "'";
        if (w)
            bucket += " (via " + w->Alias + ")";
    }
    Logger::Log::Info("MaterialSystem: shader edit->recompile lane watches: {}",
                      covered.empty() ? std::string("<none>") : covered);
    if (!uncovered.empty())
    {
        Logger::Log::Warning(
            "MaterialSystem: shader roots NOT covered by a file watcher (edits there will NOT "
            "hot-recompile; typically read-only package mounts): {}",
            uncovered);
    }
}

std::filesystem::path MaterialSystem::ResolveSurfacePath(const MaterialDocument& doc,
                                                         const std::filesystem::path& materialAssetPath) const
{
    return Rendering::ShaderComposer::ResolveSurfaceShaderPath(doc.surfaceShader, MaterialDirOf(materialAssetPath),
                                                               m_MaterialBuildContext);
}

std::shared_ptr<const Rendering::ShaderPropertyTable> MaterialSystem::ResolveDeclaredProperties(
    const MaterialDocument& doc, const std::filesystem::path& materialAssetPath)
{
    if (!EnsureMaterialBuildContextReady())
        return nullptr;

    Rendering::ShaderPropertyTableInputs inputs{};
    inputs.AdapterPath = m_MaterialBuildContext.AdapterShaderDir / "Adapters" / "adapter_forward.glsl";
    inputs.SurfacePath = ResolveSurfacePath(doc, materialAssetPath);
    if (!doc.vertexModifier.empty())
        inputs.VertexModifierPath = Rendering::ShaderComposer::ResolveShaderReference(
            doc.vertexModifier, MaterialDirOf(materialAssetPath), m_MaterialBuildContext);
    return Rendering::ShaderPropertyTableCache::Instance().Resolve(inputs);
}

bool MaterialSystem::IsSurfaceProjectOwned(const MaterialDocument& doc,
                                           const std::filesystem::path& materialAssetPath)
{
    if (!EnsureMaterialBuildContextReady())
        return false;
    const std::filesystem::path resolved = ResolveSurfacePath(doc, materialAssetPath);

    auto isUnder = [&](const std::filesystem::path& root)
    { return !RelativePathUnderRoot(resolved, root).empty(); };
    if (isUnder(m_MaterialBuildContext.AdapterShaderDir))
        return false;
    for (const std::filesystem::path& packageDir : m_MaterialBuildContext.PackageShaderDirs)
        if (isUnder(packageDir))
            return false;
    return true;
}

std::optional<std::vector<std::pair<std::string, uint8_t>>> MaterialSystem::ResolveSurfaceTextureSlots(
    const MaterialDocument& doc, const std::filesystem::path& materialAssetPath)
{
    if (!EnsureMaterialBuildContextReady())
        return std::nullopt; // headless / context not yet warm — leave the map as-is

    const std::filesystem::path surfacePath = ResolveSurfacePath(doc, materialAssetPath);

    std::string source;
    if (!ReadFileTextShared(surfacePath, source))
        return std::nullopt; // unresolved/unreadable — compose reports it; keep the old map

    Rendering::TextureSlotResolution slots =
        Rendering::ShaderComposer::ResolveTextureSlots(source);
    // A rejected surface fails the compose with a hard error (logged there). The
    // failed compile leaves the material's PREVIOUS pipeline running, so the old
    // routing map must survive with it — report no-result rather than empty.
    if (slots.Rejected)
        return std::nullopt;
    return std::move(slots.DeclaredSlots);
}

void MaterialSystem::CompileMaterialPipeline(Material& mat, const MaterialDocument& doc)
{
    if (!m_Device)
        return;

    m_ShaderCompilationCache.ClearVariantsForMaterial(&mat);

    if (!EnsureMaterialBuildContextReady())
        return;

    // Compile the base variant with the material's intrinsic keywords, then
    // pre-warm common pass variants (ForwardPlus, depth) to avoid first-frame
    // stalls from draw-time shaderc invocations.
    Rendering::ShaderVariantKey mergedKey = mat.GetVariantKey();

    ShaderCacheKey cacheKey{};
    cacheKey.VariantKey = mergedKey;
    cacheKey.Source.SurfaceShaderPath = doc.surfaceShader;
    cacheKey.Source.VertexModifierPath = doc.vertexModifier;
    cacheKey.Source.MaterialAssetPath = mat.GetMaterialAssetPath();

    MaterialCompileSpec spec{};
    spec.surfaceShaderPath = doc.surfaceShader;
    spec.vertexModifierPath = doc.vertexModifier;
    spec.lightingModel = doc.lightingModel;
    spec.customVertexShader = doc.customVertexShader;
    spec.userKeywords = doc.keywords;
    spec.alphaTest = GetEffectiveMaterialAlphaMode(doc) == MaterialAlphaMode::Mask;
    spec.parallax = HasKeyword(mergedKey.materialKeywords, MaterialKeyword::Parallax);

    using Clock = std::chrono::high_resolution_clock;
    const auto tShader = Clock::now();
    auto variant = m_ShaderCompilationCache.GetOrCompile(cacheKey, spec, m_MaterialBuildContext,
                                                         mat.GetName(),
                                                         m_Device->PreferredShaderSource());
    const double shaderMs = std::chrono::duration<double, std::milli>(Clock::now() - tShader).count();
    if (!variant)
        return;

    const auto tPso = Clock::now();

    // Blend/depth-write derive from the material's alpha mode plus its optional
    // explicit blend authoring (T1). With no authored `blend`/`zWrite` this
    // reproduces the pre-T1 hardwired equation byte-for-byte. Note: T1 sets the
    // PSO state only — additive/multiply are order-independent and correct even
    // unsorted, but straight-alpha/premultiplied are order-dependent and only
    // correct over opaque until the sorted transparent pass (T2) peels Blend
    // draws onto a back-to-front direct path.
    const Rendering::DerivedBlendState derivedBlend = Rendering::DeriveMaterialBlendState(
        mat.GetAlphaMode(), mat.GetBlendState(), mat.GetDepthWriteOverride(),
        mat.GetDepthTestOverride());
    const Rendering::CullModeFlags cullMode = mat.IsDoubleSided()
        ? Rendering::CullModeFlagBits::None
        : DebugCullMode();

    const Rendering::GraphicsPipelineId pipelineId = InternBaseMaterialPipeline(
        *m_Device, variant, mat.GetVariantKey().vertexFlags, cullMode, DebugFrontFace(),
        derivedBlend, mat.GetName());
    if (!pipelineId.IsValid())
        return; // meta-apply failed (already logged)

    mat.m_GraphicsPipelineId = pipelineId;
    mat.m_ShaderMeta = variant->meta;
    // The packed lanes this program reads, re-laid by name from the same parse
    // the composer used, so the CPU cache and the shader now bound agree on
    // every lane. Bound HERE, with the pipeline: a compile that failed above
    // left the previous shader running and its lanes untouched.
    m_RuntimeMaterialRegistry.ApplyPropertyTable(mat, ResolveDeclaredProperties(doc, mat.GetMaterialAssetPath()));

    {
        // Per-flags interned ids reference the previous m_GraphicsPipelineId.
        // Drop them here so the next draw re-derives against the new base.
        std::lock_guard lock(*mat.m_PerFlagsIdMutex);
        mat.m_PerFlagsId.clear();
    }
    ++mat.m_Version;

    const double psoMs = std::chrono::duration<double, std::milli>(Clock::now() - tPso).count();

    Logger::Log::Trace(
        "[ModelLoad]     CompilePipeline '{}': shader {:.1f}ms, psoDesc {:.1f}ms",
        mat.GetName(), shaderMs, psoMs);
}

bool MaterialSystem::CanSubmitAsyncBaseCompile() const
{
    // The worker calls SubmitTrackedPrewarm -> the engine job system, and interns
    // on the device. Both must exist. Headless/tests without an initialized engine
    // fall back to the inline compile in RegisterMaterialFromDocument. A device
    // that cannot create resources off its owning thread (WebGPU) compiles inline
    // too: a worker-side intern there kills the worker and never publishes.
    return m_Device != nullptr
        && m_Device->GetCapabilities().supportsMultithreadedResourceCreation
        && GameEngine::EngineCore::GetInstance().IsInitialized();
}

bool MaterialSystem::IsBaseCompileInFlight(const GUID& guid) const
{
    assert(std::this_thread::get_id() == m_OwnerThreadId &&
           "IsBaseCompileInFlight is main-thread-only (m_BaseCompileInFlight is unlocked)");
    return m_BaseCompileInFlight.find(guid) != m_BaseCompileInFlight.end();
}

void MaterialSystem::SubmitAsyncBaseCompile(const GUID& guid, Material& mat,
                                            const MaterialDocument& doc)
{
    assert(std::this_thread::get_id() == m_OwnerThreadId
           && "SubmitAsyncBaseCompile is main-thread-only (mutates m_BaseCompileInFlight unlocked)");
    if (!m_Device)
        return;

    // Async can't run without the engine job system (SubmitTrackedPrewarm) — fall
    // back to the inline synchronous compile. This keeps headless/test paths (and
    // the re-drive path below) deterministic, and covers the pre-engine window.
    if (!CanSubmitAsyncBaseCompile())
    {
        CompileMaterialPipeline(mat, doc);
        return;
    }

    // The worker snapshots the build context by value; populate it here on the
    // main thread first (not thread-safe to populate). If it is still cold, fall
    // back to the inline compile — which applies its own readiness guard — rather
    // than dispatching a worker that would capture an empty AdapterShaderDir.
    if (!EnsureMaterialBuildContextReady())
    {
        CompileMaterialPipeline(mat, doc);
        return;
    }

    // Capture every compile input by value on this (main) thread — the Material
    // may be re-registered or mutated before the worker runs. This mirrors what
    // CompileMaterialPipeline reads inline.
    const Rendering::ShaderVariantKey variantKey = mat.GetVariantKey();
    ShaderCacheKey cacheKey{};
    cacheKey.VariantKey = variantKey;
    cacheKey.Source.SurfaceShaderPath = doc.surfaceShader;
    cacheKey.Source.VertexModifierPath = doc.vertexModifier;
    cacheKey.Source.MaterialAssetPath = mat.GetMaterialAssetPath();

    // Coalesce duplicate submits: one base compile per GUID at a time. A re-
    // registration before the first publish still sees an invalid pipeline id and
    // would re-enter this path; the in-flight entry drops the duplicate, and a
    // key change is reconciled at publish (F1: stale-key re-drive). Main-thread-
    // only, so the map needs no lock.
    if (!m_BaseCompileInFlight.try_emplace(guid, BaseCompileState{cacheKey}).second)
        return;

    MaterialCompileSpec spec{};
    spec.surfaceShaderPath = doc.surfaceShader;
    spec.vertexModifierPath = doc.vertexModifier;
    spec.lightingModel = doc.lightingModel;
    spec.customVertexShader = doc.customVertexShader;
    spec.userKeywords = doc.keywords;
    spec.alphaTest = GetEffectiveMaterialAlphaMode(doc) == MaterialAlphaMode::Mask;
    spec.parallax = HasKeyword(variantKey.materialKeywords, MaterialKeyword::Parallax);

    const Rendering::DerivedBlendState derivedBlend = Rendering::DeriveMaterialBlendState(
        mat.GetAlphaMode(), mat.GetBlendState(), mat.GetDepthWriteOverride(),
        mat.GetDepthTestOverride());
    const Rendering::CullModeFlags cullMode = mat.IsDoubleSided()
        ? Rendering::CullModeFlagBits::None
        : DebugCullMode();
    const Rendering::FrontFace frontFace = DebugFrontFace();
    const Rendering::VertexAttributeFlags vertexFlags = variantKey.vertexFlags;
    const std::string materialName = mat.GetName();
    const Rendering::MaterialBuildContext buildContext = m_MaterialBuildContext;
    // Q6 RebuildDevice: the worker captures this raw IDevice*, which is safe
    // because the IDevice POINTER survives RebuildDevice by design (the rebuild
    // is in place). A publish that straddles a rebuild is benign for the same
    // reason the failed-compile restore is: interned pipeline ids survive —
    // OnDeviceRebuilt clears only concrete pipelines (ClearConcreteOnly keeps
    // the intern tables) and the warm probe liveness-checks concretes via
    // IsPipelineAlive — so nothing rebuild-side needs to drain
    // m_PendingPipelinePublishes or m_BaseCompileInFlight.
    Rendering::IDevice* device = m_Device;
    auto* cache = &m_ShaderCompilationCache;

    SubmitTrackedPrewarm(
        [this, guid, cacheKey, spec, buildContext, derivedBlend, cullMode, frontFace,
         vertexFlags, materialName, device, cache]() mutable
        {
            // Compile the SPIR-V (mutex-guarded, single-flight) and intern the base
            // pipeline (thread-safe device cache) entirely off the main thread.
            auto variant = cache->GetOrCompile(cacheKey, spec, buildContext, materialName,
                                               device->PreferredShaderSource());
            Rendering::GraphicsPipelineId pipelineId{};
            if (variant)
                pipelineId = InternBaseMaterialPipeline(*device, variant, vertexFlags, cullMode,
                                                        frontFace, derivedBlend, materialName);
            // Hand the result (tagged with the key it was compiled against) to the
            // serial frame-begin thread. A failed compile still enqueues (invalid
            // id) so the drain clears the in-flight guard.
            std::lock_guard<std::mutex> lk(m_PendingPublishMutex);
            m_PendingPipelinePublishes.push_back(
                {guid, pipelineId, variant ? variant->meta : nullptr, cacheKey});
        });
}

void MaterialSystem::RedriveAsyncBaseCompile(const GUID& guid, Material& mat)
{
    // Rebuild the compile inputs from the material's CURRENT compile spec, which
    // ApplyDocumentToMaterial keeps in lockstep with the latest registered
    // document, so the re-drive targets the live pipeline-affecting state rather
    // than the superseded document that produced the stale result.
    const MaterialCompileSpec& spec = mat.GetCompileSpec();
    MaterialDocument doc{};
    doc.surfaceShader = spec.surfaceShaderPath;
    doc.vertexModifier = spec.vertexModifierPath;
    doc.lightingModel = spec.lightingModel;
    doc.customVertexShader = spec.customVertexShader;
    doc.keywords = spec.userKeywords;
    if (spec.alphaTest)
        doc.alphaMode = MaterialAlphaMode::Mask;
    SubmitAsyncBaseCompile(guid, mat, doc);
}

void MaterialSystem::ApplyPendingPipelinePublishes()
{
    assert(std::this_thread::get_id() == m_OwnerThreadId
           && "ApplyPendingPipelinePublishes is main-thread-only (writes Material fields unlocked)");
    std::vector<PendingPipelinePublish> pending;
    {
        std::lock_guard<std::mutex> lk(m_PendingPublishMutex);
        if (m_PendingPipelinePublishes.empty())
            return;
        pending.swap(m_PendingPipelinePublishes);
    }

    for (auto& rec : pending)
    {
        // Clear the in-flight guard whatever the outcome (success, failed compile,
        // a material that vanished, or a stale key) so the GUID can be resubmitted.
        bool redrive = false;
        Rendering::GraphicsPipelineId lastGoodId{};
        if (const auto it = m_BaseCompileInFlight.find(rec.Guid);
            it != m_BaseCompileInFlight.end())
        {
            redrive = it->second.RedriveAfterPublish;
            lastGoodId = it->second.LastGoodPipelineId;
            m_BaseCompileInFlight.erase(it);
        }

        // Superseded by an async recompile while in flight: the result may be
        // built from pre-edit bytes under an identical key, so the F1 guard
        // below cannot catch it. Discard and re-drive against current sources
        // (a failed superseded compile re-drives too — the edit may fix it).
        if (redrive)
        {
            if (Material* live = m_RuntimeMaterialRegistry.Find(rec.Guid))
            {
                RedriveAsyncBaseCompile(rec.Guid, *live);
                // Carry the last-good stash across the re-drive — it can fail
                // too. An inline-fallback re-drive leaves no in-flight entry:
                // restore directly if it failed.
                if (const auto it = m_BaseCompileInFlight.find(rec.Guid);
                    it != m_BaseCompileInFlight.end())
                {
                    it->second.LastGoodPipelineId = lastGoodId;
                }
                else if (!live->GetGraphicsPipelineId().IsValid() && lastGoodId.IsValid())
                {
                    live->m_GraphicsPipelineId = lastGoodId;
                }
            }
            continue;
        }

        if (!rec.PipelineId.IsValid())
        {
            // Compile failed — restore the pipeline the submit retired
            // (fail-visible: the mesh keeps rendering the last good shader
            // until an edit compiles), unless something newer already
            // published. Errors were logged by ShaderCompilationCache.
            if (Material* mat = m_RuntimeMaterialRegistry.Find(rec.Guid);
                mat && lastGoodId.IsValid() && !mat->GetGraphicsPipelineId().IsValid())
            {
                mat->m_GraphicsPipelineId = lastGoodId;
            }
            continue;
        }

        // Look the material up by GUID (never a captured Material*). R3-verified:
        // MaterialRegistry::Unregister has no off-main production callers (tests
        // only; asserted at RenderExtractionSystem.cpp) and the .material hot-reload
        // re-registers in place on the main thread, so no concurrent free races this
        // main-thread drain. A same-thread unregister-before-drain yields a null
        // Find and a safe skip. (If a future off-main Unregister is added, this
        // Find-then-write needs a deferred handoff instead.)
        Material* mat = m_RuntimeMaterialRegistry.Find(rec.Guid);
        if (!mat)
            continue;
        // A synchronous (re)compile may have published a pipeline while ours was in
        // flight — don't clobber the newer state with this stale async result.
        if (mat->GetGraphicsPipelineId().IsValid())
            continue;

        // F1: the material may have been re-registered with different pipeline-
        // affecting state (e.g. a .material hot-reload during a cold load) after
        // this compile started. Our result was built against rec.CompiledKey; if
        // that no longer matches the material's live key, publishing it would run
        // the wrong shader with no self-heal (the material has no valid pipeline to
        // recompile from). Discard and re-drive for the current key instead.
        ShaderCacheKey liveKey{};
        liveKey.VariantKey = mat->GetVariantKey();
        liveKey.Source.SurfaceShaderPath = mat->GetCompileSpec().surfaceShaderPath;
        liveKey.Source.VertexModifierPath = mat->GetCompileSpec().vertexModifierPath;
        liveKey.Source.MaterialAssetPath = mat->GetMaterialAssetPath();
        if (!(rec.CompiledKey == liveKey))
        {
            RedriveAsyncBaseCompile(rec.Guid, *mat);
            continue;
        }

        // Drop stale per-material shader variants so a draw re-derives against the
        // published base — the synchronous CompileMaterialPipeline does the same
        // before it interns (they are keyed by Material*, not MaterialVersion, so
        // the version bump below does not evict them).
        m_ShaderCompilationCache.ClearVariantsForMaterial(mat);

        // F4: these are plain (unsynchronized) writes to fields a draw-time record
        // reads. They are safe ONLY because there is no render-ahead thread — the
        // engine runs a single frame spine, and this drain executes in BeginFrame
        // before the parallel record window opens, so no reader overlaps the write.
        mat->m_GraphicsPipelineId = rec.PipelineId;
        mat->m_ShaderMeta = std::move(rec.Meta);
        // The packed lanes the published program reads, from the key it was
        // compiled against (F1 above proved it is still the material's live key):
        // re-laid by name from the same parse the composer used. A failed compile
        // never reaches this point, so the last-good shader keeps the lanes it was
        // laid out for; a `// @property` edit that moves lanes reaches the CPU
        // cache in the frame the new shader binds, never at submit.
        MaterialDocument compiled{};
        compiled.surfaceShader = rec.CompiledKey.Source.SurfaceShaderPath;
        compiled.vertexModifier = rec.CompiledKey.Source.VertexModifierPath;
        m_RuntimeMaterialRegistry.ApplyPropertyTable(
            *mat, ResolveDeclaredProperties(compiled, mat->GetMaterialAssetPath()));
        {
            // Per-flags interned ids reference the previous (invalid) base — drop
            // them so the next draw re-derives against the published base.
            std::lock_guard lock(*mat->m_PerFlagsIdMutex);
            mat->m_PerFlagsId.clear();
        }
        // Version bump retires any stale variant-cache entries keyed on the old
        // MaterialVersion; the material re-enters the draw stream next frame.
        ++mat->m_Version;
    }
}

void MaterialSystem::EnqueueBasePipelinePublishForTesting(
    const GUID& guid, Rendering::GraphicsPipelineId pipelineId,
    std::shared_ptr<Rendering::ShaderMeta> meta, const ShaderCacheKey& compiledKey,
    Rendering::GraphicsPipelineId lastGoodPipelineId)
{
    // Mimic a real async submit + worker publish so a test can exercise the drain
    // guards through FlushAsyncMaterialCompiles without a live job system.
    m_BaseCompileInFlight.insert_or_assign(
        guid, BaseCompileState{compiledKey, /*RedriveAfterPublish=*/false, lastGoodPipelineId});
    std::lock_guard<std::mutex> lk(m_PendingPublishMutex);
    m_PendingPipelinePublishes.push_back({guid, pipelineId, std::move(meta), compiledKey});
}

void MaterialSystem::FlushAsyncMaterialCompiles()
{
    // Wait for every submitted base compile (and prewarm) worker to finish, then
    // apply the results on this (calling) thread so the observable material state
    // matches the synchronous path. Non-worker-thread only (Wait contract).
    DrainPrewarmJobs();
    ApplyPendingPipelinePublishes();
    m_PipelineVariants->ApplyPendingVariantPublishes();
}

void MaterialSystem::PrewarmMaterialBaseShader(const MaterialDocument& doc,
                                               const std::filesystem::path& materialAssetPath)
{
    if (!m_Device)
        return;

    auto& engine = GameEngine::EngineCore::GetInstance();
    if (!engine.IsInitialized())
        return;

    // Worker captures buildContext by value below — populate before the snapshot
    // so the async compile sees a valid AdapterShaderDir.
    if (!EnsureMaterialBuildContextReady())
        return;

    // The key and compile inputs registration will build, so the warmed entry is
    // the one the base pipeline looks up.
    const MaterialShaderIdentity identity = DeriveShaderIdentity(doc, materialAssetPath);
    const MaterialCompileSpec& spec = identity.Spec;
    if (spec.surfaceShaderPath.empty())
        return;

    ShaderCacheKey cacheKey{};
    cacheKey.VariantKey = identity.Key;
    cacheKey.Source.SurfaceShaderPath = spec.surfaceShaderPath;
    cacheKey.Source.VertexModifierPath = spec.vertexModifierPath;
    // Key identity must match the eventual draw-time key (which carries the
    // asset path), and material-relative surface references need the anchor.
    cacheKey.Source.MaterialAssetPath = materialAssetPath;

    const Rendering::MaterialBuildContext buildContext = m_MaterialBuildContext;
    auto* cache = &m_ShaderCompilationCache;
    const std::string materialName = doc.materialName.empty() ? std::string("<prewarm>") : doc.materialName;
    const Rendering::ShaderSourceKind sourceKind = m_Device->PreferredShaderSource();

    SubmitTrackedPrewarm(
        [cache, cacheKey, spec, buildContext, materialName, sourceKind]() mutable
        {
            cache->GetOrCompile(cacheKey, spec, buildContext, materialName, sourceKind);
        });
}

void MaterialSystem::SubmitTrackedPrewarm(std::function<void()> work,
                                          JobSystem::JobPriority priority, PrewarmOrigin origin)
{
    PrewarmCounters& counters = origin == PrewarmOrigin::ProjectWarmUp ? m_ProjectWarmUp : m_OnDemandPrewarm;
    // Slice 5: tracked through a JobCounter instead of the hand-rolled
    // in-flight/mutex/cv barrier. Audit resolution: NARROWED critical
    // section (the alternative — gate held across the publish — would need a
    // standing re-entrancy constraint on every prewarm closure, see below).
    //
    // The COUNT happens under the gate lock: a shutdown that sets the flag
    // under this mutex afterwards always sees the job in its drain (F16
    // add-at-submit; publishing outside the gate WITHOUT counting inside it
    // would let a drain observe zero and tear down while this submit is
    // about to count a job in). The PUBLISH happens OUTSIDE the lock: a
    // publish racing pool shutdown self-drains inline (F13c), executing
    // arbitrary queued bare tasks on THIS thread — under m_PrewarmDrainMutex
    // that was a self-deadlock whenever a drained task re-entered this gate
    // (non-recursive mutex). With the publish outside, a drained task may
    // re-enter SubmitTrackedPrewarm freely.
    {
        std::lock_guard<std::mutex> lk(m_PrewarmDrainMutex);
        if (m_PrewarmShutdown)
            return;  // shutting down — don't start work that captures soon-to-be-freed state
        m_PrewarmEverSubmitted = true;
        m_PrewarmCounter.Add(1);
        // Progress accounting (the origin's counters): count the task in under the
        // same gate as the JobCounter Add so submitted is published before the
        // task can run and complete.
        counters.Submitted.fetch_add(1, std::memory_order_relaxed);
    }
    // Counter balance is intrinsic to the task's lifetime: this guard's shared
    // deleter bumps completed and decrements the drain barrier EXACTLY ONCE, when
    // the last copy of the tracked closure is destroyed — whether the closure ran
    // to completion, was dropped by the gate (a promoted job whose pool dispatch
    // failed), or was dropped by the pool. That removes the drop-sensitivity a
    // manual catch-rebalance would carry at each of those three sites. The guard
    // is a shared_ptr (copyable), so the closure stays copy-constructible as the
    // pool envelope requires. `this` outlives the deleter: DrainPrewarmJobs waits
    // on m_PrewarmCounter, which the deleter releases, so every deleter has fired
    // before ~MaterialSystem proceeds past its drain.
    auto counterGuard = std::shared_ptr<void>(nullptr, [this, &counters](void*) {
        // Bump completed before releasing the barrier so a drained-queue observer
        // (pending == 0) sees the count.
        counters.Completed.fetch_add(1, std::memory_order_release);
        m_PrewarmCounter.Decrement();
    });
    auto tracked = [work = std::move(work), counterGuard = std::move(counterGuard)]() mutable
    {
        work();
    };

    // Route through the global compile-admission gate so total concurrent shaderc
    // stays bounded: it dispatches admitted work to the pool on the requested lane
    // and holds the remainder in per-lane pending queues drained as running
    // compiles finish — no worker sleeps, and this (main) thread never blocks. The
    // dispatch happens OUTSIDE m_PrewarmDrainMutex so a task that self-drains the
    // pool inline (F13c) can re-enter this method freely. A throwing dispatch
    // (task-envelope allocation failure) destroys the closure, firing the guard —
    // no rebalance needed here. Absent gate (default-constructed, never
    // Initialized) => enqueue directly, preserving the pre-engine/test fallback.
    if (m_CompileGate)
        m_CompileGate->Submit(std::move(tracked), priority);
    else
        GameEngine::EngineCore::GetInstance().GetJobSystem().EnqueueWork(std::move(tracked),
                                                                         priority);
}

void MaterialSystem::DispatchPipelineBuild(std::function<void()> build)
{
    // A pipeline build is something a frame waits on (a cold variant, a feature
    // that declares nothing until its pipelines land), so it rides the
    // Background lane, which the compile gate admits ahead of the Normal-lane
    // prewarm backlog. Without the job system it runs inline.
    if (!CanSubmitAsyncBaseCompile())
    {
        build();
        return;
    }
    SubmitTrackedPrewarm(std::move(build), JobSystem::JobPriority::Background);
}

void MaterialSystem::DrainPrewarmJobs()
{
    // Never touch the engine when nothing was ever submitted: the destructor
    // backstop must stay engine-free for facades that never prewarmed (tests,
    // headless). Once anything was submitted the pool provably exists and
    // outlives this system (engine teardown destroys the pool last).
    bool everSubmitted = false;
    {
        std::lock_guard<std::mutex> lk(m_PrewarmDrainMutex);
        everSubmitted = m_PrewarmEverSubmitted;
    }
    if (!everSubmitted)
        return;
    GameEngine::EngineCore::GetInstance().GetJobSystem().Wait(m_PrewarmCounter);
}

void MaterialSystem::PrewarmMaterialVariants(Material& mat,
                                             Rendering::MaterialKeyword colorPassKeywords)
{
    // The registered key and compile inputs are exactly what the draw path
    // compiles its variants from (ShaderCompilationCache::GetOrCompileVariant).
    PrewarmMaterialVariantSet(mat.GetVariantKey(), mat.GetCompileSpec(), mat.GetAlphaMode(),
                              mat.IgnoresVertexColor(), mat.GetName(), mat.GetMaterialAssetPath(), colorPassKeywords,
                              PrewarmOrigin::OnDemand);
}

void MaterialSystem::PrewarmMaterialShaders(const MaterialDocument& doc,
                                           const std::filesystem::path& materialAssetPath,
                                           Rendering::MaterialKeyword colorPassKeywords)
{
    if (!m_Device || !GameEngine::EngineCore::GetInstance().IsInitialized() ||
        !EnsureMaterialBuildContextReady())
        return;
    const MaterialShaderIdentity identity = DeriveShaderIdentity(doc, materialAssetPath);
    if (identity.Spec.surfaceShaderPath.empty())
        return;
    PrewarmMaterialVariantSet(identity.Key, identity.Spec, identity.AlphaMode, doc.ignoreVertexColor,
                              doc.materialName, materialAssetPath, colorPassKeywords, PrewarmOrigin::ProjectWarmUp);
}

MaterialSystem::MaterialShaderIdentity MaterialSystem::DeriveShaderIdentity(
    const MaterialDocument& doc, const std::filesystem::path& materialAssetPath)
{
    // RegisterMaterialFromDocument's document resolution, in its order: the
    // demotion reads the authored surface-reference shape that the reconcile
    // back-fill overwrites.
    MaterialDocument resolved = doc;
    DemoteMaskWithoutAlphaSource(resolved, GUID{});
    if (m_MaterialCompiler)
        m_MaterialCompiler->ReconcileShaderReferences(resolved);
    MaterialShaderIdentity identity;
    // A refusal is registration's to report: it builds the material this identity warms.
    std::string parallaxRefusal;
    identity.Key = DeriveRegistrationVariantKey(resolved, materialAssetPath,
                                                ResolveSurfaceTextureSlots(resolved, materialAssetPath), parallaxRefusal);
    identity.Spec = DeriveCompileSpec(resolved);
    identity.Spec.parallax = HasKeyword(identity.Key.materialKeywords, MaterialKeyword::Parallax);
    identity.AlphaMode = DeriveRenderedAlphaMode(resolved);
    return identity;
}

Rendering::ShaderVariantKey MaterialSystem::DeriveRegistrationVariantKey(
    const MaterialDocument& resolvedDoc, const std::filesystem::path& materialAssetPath,
    const std::optional<std::vector<std::pair<std::string, uint8_t>>>& surfaceSlots,
    std::string& outParallaxRefusal)
{
    Rendering::ShaderVariantKey key = DeriveBaseVariantKey(resolvedDoc);
    // Vertex flags and the modifier form are derived here rather than in
    // MaterialRegistry: both are decided by resolved files, which needs the asset
    // path and the shader-root chain. With no asset path the flags stay at the
    // registry's StandardMesh.
    if (!materialAssetPath.empty())
        key.vertexFlags = InferVertexAttributeFlagsFromDocument(resolvedDoc, materialAssetPath);
    // The modifier form runs on the empty-path case too — engine-authored
    // materials (grass, ocean, CBT) have no .material asset and reference their
    // modifier from a shader root, which ResolveShaderReference still finds.
    //
    // Called for its populating side effect, NOT as a gate: a context that stays cold
    // (no Engine instance — a unit-test harness) must still leave a modifier document
    // carrying a modifier keyword. Resolution then misses and the derivation falls back
    // to the simple form, which is what keeps "a modifier implies one of the two bits"
    // true for the depth classifiers everywhere.
    EnsureMaterialBuildContextReady();
    Rendering::ApplyVertexModifierKeyword(key, resolvedDoc.vertexModifier,
                                          materialAssetPath.parent_path(),
                                          m_MaterialBuildContext);
    outParallaxRefusal = ApplyParallaxKeywordDescribingRefusal(key, resolvedDoc, materialAssetPath, surfaceSlots);
    // Both derivations above are unaware of fully-procedural materials: the vertexFlags
    // inference would resurrect a phantom vertex binding the SPIR-V never consumes, and
    // a modifier that failed to resolve would fall back to the simple form. Re-apply
    // the clamp last so vertexFlags stays None and the form stays extended.
    if (resolvedDoc.customVertexShader)
        Rendering::ApplyCustomVertexShaderClamp(key);
    return key;
}

std::string MaterialSystem::ApplyParallaxKeywordDescribingRefusal(
    Rendering::ShaderVariantKey& key, const MaterialDocument& resolvedDoc,
    const std::filesystem::path& materialAssetPath,
    const std::optional<std::vector<std::pair<std::string, uint8_t>>>& surfaceSlots)
{
    // Parallax follows the height-map binding AND the resolved surface's declared slots, so it is
    // derived with the surface, never from the document alone. The surface's name is looked up
    // only for a refusal's message.
    const Rendering::ParallaxRefusal refusal =
        Rendering::ApplyParallaxKeyword(key, resolvedDoc, surfaceSlots ? &*surfaceSlots : nullptr);
    if (refusal == Rendering::ParallaxRefusal::None)
        return {};
    return Rendering::DescribeParallaxRefusal(
        refusal, SurfaceDisplayName(ResolveSurfacePath(resolvedDoc, materialAssetPath),
                                    MaterialDirOf(materialAssetPath), m_MaterialBuildContext));
}

MaterialSystem::SurfaceTextureUse MaterialSystem::ResolveSurfaceTextureUse(
    const MaterialDocument& doc, const std::filesystem::path& materialAssetPath)
{
    // Registration's shader-reference resolution, the step of it that decides which surface the
    // document composes with (a surface referenced by GUID resolves to its path).
    MaterialDocument resolved = doc;
    if (m_MaterialCompiler)
        m_MaterialCompiler->ReconcileShaderReferences(resolved);

    SurfaceTextureUse use;
    const auto surfaceSlots = ResolveSurfaceTextureSlots(resolved, materialAssetPath);
    if (surfaceSlots)
    {
        use.DeclaredNames.reserve(surfaceSlots->size());
        for (const auto& [name, ordinal] : *surfaceSlots)
            use.DeclaredNames.push_back(name);
    }
    Rendering::ShaderVariantKey decidedKey{};
    use.ParallaxRefusal =
        ApplyParallaxKeywordDescribingRefusal(decidedKey, resolved, materialAssetPath, surfaceSlots);
    return use;
}

void MaterialSystem::ReportParallaxRefusal(Material& mat, std::string refusal)
{
    if (refusal == mat.m_ParallaxRefusal)
        return;
    mat.m_ParallaxRefusal = std::move(refusal);
    if (mat.m_ParallaxRefusal.empty())
        return;
    Logger::Log::Warning("Material '{}' ({}): {}. It renders without parallax.", mat.GetName(),
                         mat.GetMaterialAssetPath().empty() ? mat.GetGuid().ToString()
                                                            : mat.GetMaterialAssetPath().string(),
                         mat.m_ParallaxRefusal);
}

void MaterialSystem::PrewarmMaterialVariantSet(const Rendering::ShaderVariantKey& variantKey,
                                               const MaterialCompileSpec& spec,
                                               MaterialAlphaMode alphaMode,
                                               bool ignoresVertexColor,
                                               const std::string& materialName,
                                               const std::filesystem::path& materialAssetPath,
                                               Rendering::MaterialKeyword colorPassKeywords,
                                               PrewarmOrigin origin)
{
    if (!m_Device)
        return;

    auto& engine = GameEngine::EngineCore::GetInstance();
    if (!engine.IsInitialized())
        return;

    // Populate the build context before snapshotting it for the worker. Without
    // this, on first-frame prewarm the editor asset source may not be mounted
    // yet, the worker would capture an empty AdapterShaderDir, and the compile
    // would silently fail (the cache layer at commit 1cdfe9621 prevents the
    // poisoning, but pre-population means the work isn't wasted at all).
    if (!EnsureMaterialBuildContextReady())
        return;

    // Snapshot the build context by value — the worker reads its copy freely.
    // m_MaterialBuildContext is NOT immutable: EnsureMaterialBuildContextReady
    // re-derives the mount-derived fields (including AdapterShaderDir) whenever
    // the source set moves, and a project switch resets it outright. Copying
    // here is what keeps the worker off that mutation.
    const Rendering::MaterialBuildContext buildContext = m_MaterialBuildContext;
    auto* cache = &m_ShaderCompilationCache;

    // The color/world pass keyword set is resolved on the RenderServices side
    // (ResolveWorldPassKeywordsForPrewarm reads blueprint/view state) and passed
    // in, so prewarm matches whatever pipeline is loaded (IBL in the editor's
    // ForwardPlus.rendergraph, non-IBL in WASDDemo) without the
    // facade reaching into blueprint/view state (§0.3). GetOrCompileColorVariantImpl's
    // self-heal remains the safety net if the blueprint changes after prewarm.
    const MaterialKeyword colorPassKw = colorPassKeywords;
    const Rendering::ShaderSourceKind sourceKind = m_Device->PreferredShaderSource();

    SubmitTrackedPrewarm(
        [cache, variantKey, spec, alphaMode, ignoresVertexColor, buildContext, materialName, materialAssetPath,
         colorPassKw, sourceKind]() mutable
        {
            using Clock = std::chrono::high_resolution_clock;
            const auto t0 = Clock::now();
            const auto keys = MaterialPrewarmVariantKeys(variantKey, colorPassKw, spec.customVertexShader,
                                                         alphaMode, ignoresVertexColor);
            for (const auto& key : keys)
            {
                ShaderCacheKey ck{};
                ck.Source.SurfaceShaderPath = spec.surfaceShaderPath;
                ck.Source.VertexModifierPath = spec.vertexModifierPath;
                ck.Source.MaterialAssetPath = materialAssetPath;
                ck.VariantKey = key;
                cache->GetOrCompile(ck, spec, buildContext, materialName, sourceKind);
            }

            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            Logger::Log::Trace("[Prewarm] '{}' {} variants: {:.1f}ms (background)", materialName, keys.size(), ms);
        },
        origin == PrewarmOrigin::ProjectWarmUp ? JobSystem::JobPriority::Background
                                               : JobSystem::JobPriority::Normal,
        origin);
}

Material* MaterialSystem::RegisterAndPrewarmMaterial(
    const GUID& guid, const MaterialDocument& doc,
    Rendering::MaterialKeyword additionalKeywords,
    Rendering::MaterialKeyword colorPassKeywords)
{
    // Async base compile: the model/scene-load path drives this wrapper on the
    // main thread for every material. Deferring the base-shader compile to a
    // worker keeps frames pumping through a cold load; the material is skipped by
    // the draw stream (dark) until its pipeline publishes on a later BeginFrame.
    Material* mat = RegisterMaterialFromDocument(guid, doc, additionalKeywords,
                                                 BaseCompileMode::Async);
    if (mat)
        PrewarmMaterialVariants(*mat, colorPassKeywords);
    return mat;
}

} // namespace Engine::Renderer
} // namespace GameEngine
