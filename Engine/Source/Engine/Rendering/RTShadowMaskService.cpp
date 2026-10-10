#include "Engine/Rendering/RTShadowMaskService.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "Engine/Rendering/Pipeline/Nodes/PipelineNodeUtils.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/TerrainShadowMap.h"
#include "Logger/Logger.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

namespace GameEngine::Engine::Renderer
{

namespace
{

// GPUInstance.flags bit 0 — the cast-shadows bit, the same predicate the
// shadow cull applies (frustum_culling.comp "shadow dispatches drop
// non-casters").
constexpr uint32_t kInstanceFlagCastShadows = 1u;

// Degenerate-transform bound (world units) for the TLAS ingestion tripwire:
// anything positioned/scaled beyond this is sky-dome/fx-scale content whose
// BVH extents can wedge driver builders. Raster ignores such casters too
// (they fall outside every cascade), so skipping them is quality-neutral.
constexpr float kMaxSaneWorldUnits = 1.0e6f;

// Mask-pass ray parameters (world units, metre-scale content). tMin plus the
// normal-offset origin bias suppress self-intersection with the receiver's own
// triangles (the receiver IS in the TLAS; positions come from depth, normals
// from depth derivatives). tMax bounds directional occluders — anything past
// 10 km is outside every authored scene here; the max-shadow-distance receiver
// gate in the shader is what actually bounds the traced pixel set.
constexpr float kMaskRayTMin            = 0.01f;
constexpr float kMaskRayTMax            = 1.0e4f;
constexpr float kMaskNormalOffsetBias   = 0.03f;

// Soft-shadow cone jitter advances through this many frames, then repeats. The
// shader hashes the phase in float; a phase kept this small keeps every fract()
// stage at full precision (a raw frame index in the tens of thousands leaves
// the first stage with 16-32 distinct levels and neighbour pixels sharing
// samples). TAA's history weight (~0.95) integrates fewer frames than this.
constexpr uint64_t kSoftShadowJitterCycle = 64;

// Rays per pixel by RayTracedShadowQuality. Quality holds a fractional occlusion
// in the pixel itself (levels of 1/4), so its denoise footprint is halved: the
// kernel then removes residual noise without averaging a thin contact shadow
// or the physical sun's 1-2 texel penumbra away.
constexpr uint32_t kPerformanceRaysPerPixel = 1;
constexpr uint32_t kQualityRaysPerPixel     = 4;
constexpr int      kQualityDenoiseRadiusDivisor = 2;

// Spatial-denoise footprint. The mask's per-frame stipple must be averaged over a
// window no WIDER than the penumbra, or a narrow (small-sun) penumbra is fattened
// into a wide one. The penumbra's screen size scales with the sun's angular
// diameter, so the kernel radius does too — clamped to [1, kDenoiseMaxRadius].
// (~1 texel/degree: 0.53 deg physical sun -> radius 1, 3 deg -> 3, artistic
// 10 deg -> the cap.) At angular diameter 0 no denoise pass is declared, so a
// hard shadow stays byte-exact hard.
constexpr float kDenoiseRadiusPerDegree = 1.0f;
// Must equal kMaxRadius in rt_shadow_denoise.comp: the kernel stages a tile
// with exactly this border into shared memory.
constexpr int   kDenoiseMaxRadius       = 4;
// Bilateral weights. The plane tolerance is a multiple of the kernel's
// view-space footprint (texel size at the receiver's depth x kernel radius): a
// tap that far off the receiver's tangent plane is down-weighted to e^-0.5, and
// one 2.5x that far is rejected. Scaling with the footprint, not with depth,
// keeps a low step from leaking light into its contact shadow at any distance,
// while a curved receiver deviates from its tangent plane by a small fraction
// of the footprint and passes. The spatial Gaussian sigma is in texels relative
// to the radius (center-weighted).
constexpr float kDenoisePlaneToleranceTexels = 1.0f;
constexpr float kDenoiseSpatialSigmaScale    = 0.5f;

// Render-graph pool / pass names carry the view id; formatted into a fixed
// buffer so the per-frame declaration path allocates nothing.
constexpr size_t kViewNameCapacity = 64;
void FormatViewName(char (&out)[kViewNameCapacity], const char* prefix, uint32_t viewId,
                    const char* suffix)
{
    std::snprintf(out, kViewNameCapacity, "%s%u%s", prefix, viewId, suffix);
}

}  // namespace

RTShadowMaskService::RTShadowMaskService(SceneAccelerationStructureService* sceneAS,
                                         Rendering::GPUScene* gpuScene)
    : m_SceneAS(sceneAS), m_GpuScene(gpuScene)
{
    m_Backend = m_SceneAS ? m_SceneAS->GetBackend() : nullptr;
    m_Device  = m_SceneAS ? m_SceneAS->GetDevice() : nullptr;
}

RTShadowMaskService::~RTShadowMaskService()
{
    if (m_SceneAS && m_TlasSlot.IsValid())
        m_SceneAS->ReleaseTlasChannel(m_TlasSlot);
    for (const RetiredInstanceBuffer& r : m_RetiredInstanceBuffers)
    {
        if (r.Buffer.IsValid() && m_Device)
            m_Device->DestroyBuffer(r.Buffer);
    }
    // BLAS/TLAS objects are backend-owned; the device tears the backend down.
}

uint32_t RTShadowMaskService::GetBlasCount() const
{
    return m_SceneAS ? m_SceneAS->GetBlasCount() : 0u;
}

void RTShadowMaskService::TickDeferredReclaim()
{
    // Retire instance buffers older than the frames-in-flight margin (+1 for
    // the same conservative slack the AS backend uses). By retirement time
    // the device's timeline watermark is past the buffer's frame, so
    // DestroyBuffer's deferred path is safe.
    ++m_FrameClock;
    constexpr uint64_t kRetireMargin = Rendering::IDevice::kMaxSupportedFramesInFlight + 1;
    std::erase_if(m_RetiredInstanceBuffers,
                  [&](const RetiredInstanceBuffer& r)
                  {
                      if (m_FrameClock - r.FrameStamp <= kRetireMargin)
                          return false;
                      m_Device->DestroyBuffer(r.Buffer);
                      return true;
                  });
}

void RTShadowMaskService::TickInactive()
{
    if (m_TlasSlot.IsValid())
        ReleaseAccelerationStructures();

    // Only this service's retired instance buffers remain (the frame spine
    // drains the backend's queue). Keep their clock ticking until they drain,
    // then go fully quiescent.
    if (!m_RetiredInstanceBuffers.empty())
        TickDeferredReclaim();
}

void RTShadowMaskService::OnDeviceRebuilt()
{
    m_MaskSampler = {};
    m_TerrainSampler = {};
    m_RetiredInstanceBuffers.clear();
    m_TlasContentValid  = false;
    m_BuiltInstanceHash = 0;
    m_PendingTlasHash   = 0;
    m_LastInstanceCount = 0;
    m_TlasBuildExecuted.reset();
    m_HaveCasterEpoch = false;
    m_LastCasterEpoch = 0;
}

void RTShadowMaskService::ReleaseAccelerationStructures()
{
    // Release this consumer's claim; its own TLAS dies with the channel. The
    // shared BLAS pool stays with the scene AS service, which keeps it for a
    // quick re-enable and for any other consumer, and purges it once no
    // consumer has claimed it for a while.
    m_SceneAS->ReleaseTlasChannel(m_TlasSlot);
    m_TlasSlot = {};

    // Every service-side reference to the released TLAS dies with it.
    // Retired instance-staging buffers are NOT freed here — they keep their
    // frame-stamped margin and drain through TickDeferredReclaim, which
    // TickInactive keeps pumping.
    m_TlasContentValid  = false;  // closes CanDeclareMaskPass with immediate effect
    m_BuiltInstanceHash = 0;
    m_PendingTlasHash   = 0;
    m_LastInstanceCount = 0;
    m_TlasBuildExecuted.reset();
    // Belt over the validity braces above: even if the caster epoch sum did
    // not move while inactive, the next Schedule sees epochChanged and
    // refreshes — the rebuild never depends on external content changing.
    m_HaveCasterEpoch = false;
    m_LastCasterEpoch = 0;
    // New episode for the tripwire and the steady-state summary: the rebuild
    // re-warns and re-summarizes instead of inheriting stale counts.
    m_SkippedSector = 0;
    m_WarnedSector  = false;
    m_LoggedSummary = false;
    m_InstanceScratch.clear();
    m_InstanceScratch.shrink_to_fit();

    Logger::Log::Info("RTShadowMask: no world in RayTraced mode — TLAS channel released");
}

void RTShadowMaskService::Schedule(Rendering::RenderGraph::RGFrame& frame, uint64_t casterEpoch)
{
    if (!m_Backend || !m_SceneAS || !m_GpuScene)
        return;
    // Claim the pool while tracing; TickInactive released it.
    if (!m_TlasSlot.IsValid())
        m_TlasSlot = m_SceneAS->AcquireTlasChannel("RTShadowMask");
    if (!m_TlasSlot.IsValid())
        return;

    TickDeferredReclaim();

    // TLAS exec confirmation (epoch gate): a declared TLAS build becomes the
    // trusted content only after its exec provably ran — mirroring the shared
    // service's BuildConfirmToken discipline for BLAS. An unexecuted build
    // leaves validity false so the next frame re-declares it.
    if (m_TlasBuildExecuted)
    {
        if (m_TlasBuildExecuted->load(std::memory_order_acquire))
        {
            m_TlasContentValid  = true;
            m_BuiltInstanceHash = m_PendingTlasHash;
        }
        m_TlasBuildExecuted.reset();
    }

    // 1. The frame spine already ran the shared pool's BeginFrame (confirm,
    //    reload drop, sweep). A BLAS that became Ready there forces a TLAS
    //    refresh below even without a caster-epoch bump, so a just-readied
    //    mesh's instances join the TLAS without waiting for the next raster
    //    shadow-content change.
    const bool anyBecameReady = m_SceneAS->BlasBecameReadyThisFrame();
    if (anyBecameReady)
        m_LoggedSummary = false;

    // 2. Collect every BLAS build this service can claim this frame and
    //    reserve its scratch region (shared service; see CollectPendingBuilds
    //    doc for the once-per-frame claim semantics).
    std::shared_ptr<SceneAccelerationStructureService::BuildConfirmToken> blasConfirmToken;
    std::vector<SceneAccelerationStructureService::PendingBlasBuild> builds =
        m_SceneAS->CollectPendingBuilds(blasConfirmToken);
    if (!builds.empty())
        m_LoggedSummary = false;  // re-arm: one summary per settled build burst

    // 3. Epoch gate (G1). An unchanged caster epoch with no newly-ready BLAS
    //    and confirmed TLAS content means the current TLAS is exact — with no
    //    pending BLAS builds either, no pass is declared at all (idle ASBuild
    //    cost is zero, CPU and GPU).
    const bool epochChanged = !m_HaveCasterEpoch || casterEpoch != m_LastCasterEpoch;
    m_HaveCasterEpoch       = true;
    m_LastCasterEpoch       = casterEpoch;
    const bool needRefresh  = epochChanged || anyBecameReady || !m_TlasContentValid;
    if (!needRefresh && builds.empty())
        return;

    // 4. Refresh frames rebuild the filtered instance records into the CPU
    //    scratch (v0 CPU-written array; the compute writer over the instance
    //    SSBO arrives with G2 mover epochs) and hash them: the epoch signal
    //    over-invalidates (any vertex-mod shadow caster bumps it every frame
    //    — wind foliage), and the hash keeps those spurious bumps off the GPU.
    //    Filter mirrors the raster shadow lane: cast-shadows bit
    //    (frustum_culling.comp), skinned excluded (prototype),
    //    tombstones/no-BLAS/pending meshes drop via the Ready check.
    bool doTlasBuild     = false;
    uint32_t count       = 0;
    uint64_t refreshHash = 0;
    if (needRefresh)
    {
        const std::vector<Rendering::GPUInstance>& instances = m_GpuScene->GetInstances();
        m_InstanceScratch.clear();
        m_InstanceScratch.reserve(instances.size());
        uint64_t skippedSector     = 0;
        uint64_t skippedDegenerate = 0;
        for (uint32_t i = 0; i < instances.size(); ++i)
        {
            const Rendering::GPUInstance& inst = instances[i];
            if ((inst.flags & kInstanceFlagCastShadows) == 0u)
                continue;
            if (inst.skinPaletteOffset != 0u)
                continue;
            // GetBlasAddress already folds the Ready check (0 = not ready);
            // one shared-service lookup instead of an IsBlasReady + address
            // pair, since we need the address itself further down anyway.
            const uint64_t blasAddress = m_SceneAS->GetBlasAddress(inst.meshIndex);
            if (blasAddress == 0)
                continue;
            if ((inst.sectorPacked[0] | inst.sectorPacked[1]) != 0u)
            {
                ++skippedSector;
                continue;
            }

            // Degenerate-transform tripwire: non-finite or astronomically
            // scaled instances (sky-dome/fx quads at 1e5+ world scale) are
            // exactly the inputs that can wedge a driver-side BVH builder — a
            // hung build is a 2-second TDR, not a wrong shadow. Skip and
            // count, never ingest.
            const float* m = inst.transform.Data();
            bool sane = std::isfinite(inst.boundingRadius)
                        && inst.boundingRadius < kMaxSaneWorldUnits;
            for (int f = 0; f < 16 && sane; ++f)
            {
                sane = std::isfinite(m[f]) && std::fabs(m[f]) < kMaxSaneWorldUnits;
            }
            if (!sane)
            {
                ++skippedDegenerate;
                continue;
            }

            Rendering::TlasInstanceData dst{};
            // Column-major mat4 -> VkTransformMatrixKHR row-major 3x4.
            for (uint32_t r = 0; r < 3; ++r)
                for (uint32_t c = 0; c < 4; ++c)
                    dst.Transform[r * 4 + c] = m[c * 4 + r];
            dst.CustomIndexAndMask = (i & 0x00FFFFFFu) | (0xFFu << 24);
            // Facing cull disabled: a caster occludes regardless of winding,
            // which also makes the mirrored-winding bit (flags bit 4)
            // irrelevant here.
            dst.SbtOffsetAndFlags =
                Rendering::kTlasInstanceFlagTriangleFacingCullDisable << 24;
            dst.BlasAddress = blasAddress;
            m_InstanceScratch.push_back(dst);
        }
        count               = static_cast<uint32_t>(m_InstanceScratch.size());
        m_LastInstanceCount = count;
        m_SkippedSector += skippedSector;
        m_SkippedDegenerate += skippedDegenerate;
        if (skippedSector > 0 && !m_WarnedSector)
        {
            m_WarnedSector = true;
            Logger::Log::Warning(
                "RTShadowMask: {} instance(s) in nonzero render-origin sectors skipped "
                "(prototype builds the TLAS in world space; Earth-scale content joins "
                "when the mask pass picks its working space)",
                skippedSector);
        }
        if (skippedDegenerate > 0 && !m_WarnedDegenerate)
        {
            m_WarnedDegenerate = true;
            Logger::Log::Warning(
                "RTShadowMask: {} instance(s) with non-finite or >1e6-unit transforms "
                "skipped (BVH-builder safety tripwire; first occurrence, further "
                "skips counted silently)",
                skippedDegenerate);
        }

        // FNV-1a over the record bytes, seeded with the count so N zeroed
        // records can never alias N-1 of them.
        refreshHash = 1469598103934665603ull ^ count;
        const auto* bytes = reinterpret_cast<const uint8_t*>(m_InstanceScratch.data());
        const size_t byteCount = count * sizeof(Rendering::TlasInstanceData);
        for (size_t b = 0; b < byteCount; ++b)
        {
            refreshHash ^= bytes[b];
            refreshHash *= 1099511628211ull;
        }
        doTlasBuild = !m_TlasContentValid || refreshHash != m_BuiltInstanceHash;
    }

    if (!doTlasBuild && builds.empty())
        return;  // spurious epoch bump: TLAS content byte-identical, nothing to record

    // 5. Rebuild frames stage the records into a FRESH frame-lifetime buffer
    //    (see m_RetiredInstanceBuffers) and reserve TLAS scratch.
    Rendering::BufferHandle instanceBufferHandle{};
    if (doTlasBuild)
    {
        Rendering::BufferDesc instDesc{};
        instDesc.size = std::max<uint64_t>(count, 1u) * sizeof(Rendering::TlasInstanceData);
        instDesc.usage =
            static_cast<uint32_t>(Rendering::BufferUsage::AccelerationStructureBuildInput
                                  | Rendering::BufferUsage::ShaderDeviceAddress);
        instDesc.memoryUsage = Rendering::BufferMemoryUsage::Upload;
        instDesc.flags       = Rendering::BufferCreateFlags::PersistentlyMapped;
        instDesc.debugName   = "RTShadowMask.TlasInstances";
        instanceBufferHandle = m_Device->CreateBuffer(instDesc);
        void* instanceMapped =
            instanceBufferHandle.IsValid() ? m_Device->MapBuffer(instanceBufferHandle) : nullptr;
        if (!instanceMapped)
        {
            Logger::Log::Warning("RTShadowMask: instance staging allocation failed; pass skipped");
            if (instanceBufferHandle.IsValid())
                m_Device->DestroyBuffer(instanceBufferHandle);
            return;
        }
        m_RetiredInstanceBuffers.push_back(
            RetiredInstanceBuffer{instanceBufferHandle, m_FrameClock});
        if (count > 0)
            std::memcpy(instanceMapped, m_InstanceScratch.data(),
                        count * sizeof(Rendering::TlasInstanceData));

        if (!m_Backend->PrepareTlas(m_TlasSlot, count))
        {
            Logger::Log::Warning("RTShadowMask: TLAS preparation failed; pass skipped");
            return;
        }
        m_PendingTlasHash   = refreshHash;
        m_TlasContentValid  = false;  // until the exec below confirms
        m_TlasBuildExecuted = std::make_shared<std::atomic<bool>>(false);
    }

    if (!m_LoggedSummary && builds.empty() && m_SceneAS->GetBlasCount() > 0)
    {
        m_LoggedSummary = true;
        Logger::Log::Info(
            "RTShadowMask: steady state — {} BLAS ({:.1f} MiB) in the shared scene AS pool, "
            "TLAS {} instance(s) of {} scene instances (skipped so far: {} nonzero-sector, "
            "{} degenerate transform)",
            m_SceneAS->GetBlasCount(),
            static_cast<double>(m_SceneAS->GetLiveBlasMemoryBytes()) / (1024.0 * 1024.0),
            m_LastInstanceCount, m_GpuScene->GetInstances().size(), m_SkippedSector,
            m_SkippedDegenerate);
    }

    // Fault-bisection lane: GE_RT_SHADOW_MASK_BLAS_ONLY records BLAS builds
    // but skips the TLAS build entirely, splitting the two halves of the
    // pipeline for device-loss triage. (TLAS content never confirms in that
    // lane, so the mask pass gate stays closed — by design.)
    static const bool s_BlasOnly = []
    {
        const char* v = std::getenv("GE_RT_SHADOW_MASK_BLAS_ONLY");
        return v && std::strcmp(v, "0") != 0;
    }();

    // 6. The build pass, on the graphics queue: build-order hazards (scratch
    //    reuse, BLAS->TLAS, prior frames) are handled with AS-stage barriers on
    //    that one queue. The graph tracks the TLAS (RGAccelerationStructure):
    //    this pass writes it and the mask pass reads it, so the mask is
    //    scheduled after the build and, on the compute queue, waits for it.
    //    Pool VB/IB geometry reads are ordered by the same submission-order
    //    guarantees the draw path relies on, and the instance buffer is
    //    host-written before submit (host-write visibility at submission,
    //    Vulkan 7.9).
    Rendering::IAccelerationStructureBackend* backend = m_Backend;
    const Rendering::TlasSlotHandle tlasSlot           = m_TlasSlot;
    const Rendering::BufferHandle instanceBuffer       = instanceBufferHandle;
    const uint32_t instanceCount                       = count;
    const bool recordTlas                              = doTlasBuild && !s_BlasOnly;
    auto tlasExecuted                                  = m_TlasBuildExecuted;  // null unless doTlasBuild
    auto blasBuilds = std::make_shared<std::vector<SceneAccelerationStructureService::PendingBlasBuild>>(
        std::move(builds));

    const Rendering::RenderGraph::RGAccelerationStructure tlasRG =
        recordTlas ? frame.ImportAccelerationStructure("RTShadowMask.TLAS", tlasSlot)
                   : Rendering::RenderGraph::RGAccelerationStructure{};
    frame.AddPass(
        "RTShadowMask.ASBuild", Rendering::PassPhase::kEarlySetup,
        [tlasRG](Rendering::RenderGraph::RGPassBuilder& p)
        {
            // The TLAS is the one output the graph tracks; the BLAS storage,
            // scratch, instance staging and pool VB/IB live outside it, so a
            // BLAS-only frame has no output and must not be dead-stripped.
            p.PreventCulling();
            if (tlasRG.IsValid())
                p.Write(tlasRG);
        },
        [backend, blasBuilds, blasConfirmToken, tlasSlot, instanceBuffer, instanceCount,
         recordTlas, tlasExecuted](Rendering::RenderGraph::RGContext& ctx)
        {
            if (!ctx.Cmd)
                return;
            // Breadcrumbs feed the device-lost marker dump — a fault names
            // the last recorded phase instead of "<none recorded>".
            ctx.Cmd->SetMarker("RTShadowMask.PreBarrier");
            backend->RecordPreBuildBarrier(*ctx.Cmd);
            ctx.Cmd->SetMarker("RTShadowMask.BlasBatch.begin");
            for (const auto& b : *blasBuilds)
                backend->RecordBlasBuild(*ctx.Cmd, b.Handle, b.Geometry);
            ctx.Cmd->SetMarker("RTShadowMask.BlasBatch.end");
            if (recordTlas)
            {
                ctx.Cmd->SetMarker("RTShadowMask.Tlas.begin");
                backend->RecordTlasBuild(*ctx.Cmd, tlasSlot, instanceBuffer, 0, instanceCount);
                ctx.Cmd->SetMarker("RTShadowMask.Tlas.end");
                if (tlasExecuted)
                    tlasExecuted->store(true, std::memory_order_release);
            }
            if (blasConfirmToken)
                blasConfirmToken->MarkExecuted();
        });
}

bool RTShadowMaskService::CanDeclareMaskPass() const
{
    return m_Backend && m_TlasSlot.IsValid() && m_TlasContentValid
           && m_Backend->IsTlasBuilt(m_TlasSlot)
           && m_Backend->GetTlasDeviceAddress(m_TlasSlot) != 0 && m_LastInstanceCount > 0;
}

void RTShadowMaskService::LoadMaskShader()
{
    if (m_MaskLoadAttempted || !m_Device)
        return;
    m_MaskLoadAttempted = true;

    Rendering::ShaderPackage pkg{};
    std::string loadErr;
    if (!Rendering::LoadShaderPkg("Shaders/rt_shadow_mask.shaderpkg", m_Device->PreferredShaderSource(), pkg, &loadErr))
    {
        Logger::Log::Warning("RTShadowMask: failed to load rt_shadow_mask.shaderpkg: {}", loadErr);
        return;
    }
    auto itCs = pkg.stageBytes.find("cs");
    if (itCs == pkg.stageBytes.end() || itCs->second.empty())
    {
        Logger::Log::Warning("RTShadowMask: rt_shadow_mask.shaderpkg missing cs stage");
        return;
    }

    m_MaskMeta = std::make_unique<Rendering::ShaderMeta>(std::move(pkg.meta));

    Rendering::ComputePipelineDesc cd{};
    cd.ComputeShader =
        std::make_shared<const std::vector<uint8_t>>(std::move(itCs->second));
    cd.DebugName = "RTShadowMask.Mask";

    m_MaskSet0Layout = Rendering::DescriptorSetLayoutDesc{};
    auto patchLayout = [&](uint32_t setIndex, Rendering::DescriptorSetLayoutDesc& dsl)
    {
        if (setIndex == 0)
            m_MaskSet0Layout = dsl;
    };
    std::string err;
    Rendering::MaterialHelper::ApplyShaderMetaToComputeDesc(
        *m_Device, *m_MaskMeta, cd, Rendering::MaterialBuilder::MergeMode::Auto,
        {true, 128}, patchLayout, &err);

    m_MaskPipelineId = m_Device->InternComputePipeline(std::move(cd));
}

void RTShadowMaskService::LoadDenoiseShader()
{
    if (m_DenoiseLoadAttempted || !m_Device)
        return;
    m_DenoiseLoadAttempted = true;

    Rendering::ShaderPackage pkg{};
    std::string loadErr;
    if (!Rendering::LoadShaderPkg("Shaders/rt_shadow_denoise.shaderpkg", m_Device->PreferredShaderSource(), pkg,
                                  &loadErr))
    {
        Logger::Log::Warning("RTShadowMask: failed to load rt_shadow_denoise.shaderpkg: {}",
                             loadErr);
        return;
    }
    auto itCs = pkg.stageBytes.find("cs");
    if (itCs == pkg.stageBytes.end() || itCs->second.empty())
    {
        Logger::Log::Warning("RTShadowMask: rt_shadow_denoise.shaderpkg missing cs stage");
        return;
    }

    m_DenoiseMeta = std::make_unique<Rendering::ShaderMeta>(std::move(pkg.meta));

    Rendering::ComputePipelineDesc cd{};
    cd.ComputeShader =
        std::make_shared<const std::vector<uint8_t>>(std::move(itCs->second));
    cd.DebugName = "RTShadowMask.Denoise";

    m_DenoiseSet0Layout = Rendering::DescriptorSetLayoutDesc{};
    auto patchLayout = [&](uint32_t setIndex, Rendering::DescriptorSetLayoutDesc& dsl)
    {
        if (setIndex == 0)
            m_DenoiseSet0Layout = dsl;
    };
    std::string err;
    Rendering::MaterialHelper::ApplyShaderMetaToComputeDesc(
        *m_Device, *m_DenoiseMeta, cd, Rendering::MaterialBuilder::MergeMode::Auto,
        {true, 128}, patchLayout, &err);

    m_DenoisePipelineId = m_Device->InternComputePipeline(std::move(cd));
}

Rendering::MaterialKeyword RTShadowMaskService::ContributeWorldPass(
    RenderServices& services, Rendering::RenderGraph::RGFrame& frame, uint32_t viewId,
    Rendering::RenderGraph::RGTexture depth, const Rendering::CameraData* camera,
    bool canDeclare, bool transmissiveOnly, Rendering::RenderGraph::RGTexture& mask,
    Rendering::MaterialKeyword keywords)
{
    const auto* view = services.Views().FindViewDesc(viewId);
    if (!view)
        return keywords;
    // The mode is resolved per frame by extraction, so a mode change applies on the next frame
    // with no residual state: a Cascades frame declares no mask pass and never sets the keyword.
    // Fail-visible: any gate below that fails leaves the keyword unset and the cascade term in
    // place, never a dropped shadow term. Transmissive passes keep cascades: a glass fragment
    // writes no prepass depth, so a mask reconstructed from opaque depth would shadow it with
    // whatever is behind it.
    const auto& settings = services.GetWorldShadowSettings(view->worldId);
    if (settings.Mode != Components::DirectionalShadowMode::RayTraced || transmissiveOnly ||
        !Rendering::HasKeyword(keywords, Rendering::MaterialKeyword::Shadows))
        return keywords;
    if (canDeclare && !mask.IsValid() && depth.IsValid() && camera)
    {
        // The primary directional (SelectPrimaryDirectional, the light ShadowMapNode fits the
        // cascades to and WriteViewLightBuffer shades with), gated on its own caster flag: mask,
        // cascades and shading always agree on the light, and no mask is traced for a term the
        // shading never samples.
        const auto* sun = SelectPrimaryDirectional(services.GetWorldLights(view->worldId));
        if (sun && sun->castsShadows != 0)
        {
            float towardLight[3] = {-sun->directionWS[0], -sun->directionWS[1], -sun->directionWS[2]};
            const float length = std::sqrt(towardLight[0] * towardLight[0] +
                towardLight[1] * towardLight[1] + towardLight[2] * towardLight[2]);
            if (length > 1e-6f)
            {
                for (float& value : towardLight) value /= length;
                float maxShadowDistance = 100.0f;
                const TerrainShadowMap* terrainShadow = nullptr;
                if (auto* feature = services.GetFeature<ShadowMapRenderFeature>();
                    feature && feature->IsInitialized())
                {
                    // The view's own range, the one its GPU fade uses: the feature's config
                    // holds whichever view was fitted last (a pass key can fit each view's
                    // range to its own scene).
                    const CascadeFrameData* cascades = feature->GetCachedFrameData(viewId);
                    maxShadowDistance = cascades ? cascades->MaxShadowDistance
                                                 : feature->GetConfig().MaxShadowDistance;
                    terrainShadow = feature->FindTerrainShadowMap(viewId, frame.FrameIndex());
                }
                // Only TAA integrates enough frames to resolve a per-frame cone jitter;
                // TemporalFXAA blends two frames and every other mode none, so those views keep
                // a frame-stable jitter for the spatial denoise.
                const auto* aa = services.Views().FindViewAntiAliasing(viewId);
                const bool accumulatesHistory = aa && aa->Enabled && aa->Mode == AntiAliasingMode::TAA;
                mask = DeclareMaskPass(frame, viewId, depth, *camera, towardLight, maxShadowDistance,
                    settings.DistanceFadeFraction, sun->shadowAngularDiameter, accumulatesHistory,
                    settings.RayTracedQuality, terrainShadow);
            }
        }
    }
    return mask.IsValid() ? keywords | Rendering::MaterialKeyword::RTShadowMask : keywords;
}

Rendering::RenderGraph::RGTexture RTShadowMaskService::DeclareMaskPass(
    Rendering::RenderGraph::RGFrame& frame, uint32_t viewId,
    Rendering::RenderGraph::RGTexture depth, const Rendering::CameraData& cam,
    const float* lightDirTowardLightWS, float maxShadowDistance, float distanceFadeFraction,
    float lightAngularDiameterDegrees, bool accumulatesHistory,
    Components::RayTracedShadowQuality quality, const TerrainShadowMap* terrainShadow)
{
    namespace RG = Rendering::RenderGraph;
    const bool highQuality = quality == Components::RayTracedShadowQuality::Quality;

    if (!CanDeclareMaskPass() || !depth.IsValid() || !lightDirTowardLightWS)
        return {};
    // Thumbnail cameras before their first update: inverting a zero proj
    // produces NaN that propagates into every ray.
    if (cam.proj[0] == 0.0f && cam.proj[5] == 0.0f)
        return {};

    LoadMaskShader();
    if (!m_MaskPipelineId.IsValid() || !m_MaskMeta)
        return {};
    // Outside the shader's load-once latch: a device rebuild drops the samplers
    // (OnDeviceRebuilt) while the interned pipeline id survives.
    if (!m_MaskSampler.IsValid())
        m_MaskSampler = m_Device->CreateSampler(Rendering::SamplerDesc::PointClamp("RTShadowMask.Sampler"));
    if (!m_TerrainSampler.IsValid())
        m_TerrainSampler =
            m_Device->CreateSampler(Rendering::SamplerDesc::MaterialLinearClamp("RTShadowMask.TerrainSampler"));

    const auto& dd = frame.Graph().ResourceDesc(depth.Id);
    if (dd.Width == 0 || dd.Height == 0 || dd.SampleCount > 1)
        return {};

    // Persistent pool import (per view): PhysicalTexture must be resolvable at
    // declaration time for the world pass's by-name binding table — a
    // transient has no physical until compile.
    Rendering::TextureDesc md{};
    md.width       = dd.Width;
    md.height      = dd.Height;
    md.depth       = 1;
    md.mipLevels   = 1;
    md.arrayLayers = 1;
    md.sampleCount = 1;
    md.format      = static_cast<uint32_t>(Rendering::TextureFormat::R8_UNORM);
    md.usage       = static_cast<uint32_t>(Rendering::TextureUsage::UnorderedAccess
                                           | Rendering::TextureUsage::ShaderResource);
    char poolName[kViewNameCapacity];
    FormatViewName(poolName, "RTShadowMask.View", viewId, "");
    md.debugName             = poolName;
    const RG::RGTexture mask = frame.ImportPersistentTexture(poolName, md);
    if (!mask.IsValid())
        return {};

    // GLSL mirror: RTShadowMaskParams in rt_shadow_mask.comp (std140; mat4 and
    // vec4 members only, so the C++ mirror needs no hand padding). The TLAS is a
    // separate acceleration-structure descriptor (binding 3), not a member here.
    struct MaskParamsUBO
    {
        float InvProj[16];
        float InvView[16];
        float LightDirWS[4];  // xyz toward light, w = maxShadowDistance
        float RayParams[4];   // x = tMin, y = tMax, z = normal-offset bias,
                              // w = tan(light angular half-angle), 0 = hard
        float Temporal[4];    // x = cone-jitter phase (advances under TAA, else 0),
                              // y = rays per pixel, z = distance fade fraction
        // The terrain's clearance map (TerrainShadowMap) in the full world frame, the frame the
        // mask reconstructs positions in (uInvView); ShadowDataGPU's terrainShadowGrid* layout.
        float TerrainGrid0[4];
        float TerrainGrid1[4];
        float TerrainGrid2[4];
        float TerrainGrid3[4];
        uint32_t TerrainSource[4]; // x = 1 when present; y = map side (texels)
    };
    auto ub = frame.AllocUpload<MaskParamsUBO>();
    if (!ub.Valid())
        return {};

    GameEngine::Mathematics::Matrix4x4 projM;
    std::memcpy(projM.Data(), cam.proj, sizeof(cam.proj));
    const GameEngine::Mathematics::Matrix4x4 invProjM = GameEngine::Mathematics::Inverse(projM);
    GameEngine::Mathematics::Matrix4x4 viewM;
    std::memcpy(viewM.Data(), cam.view, sizeof(cam.view));
    const GameEngine::Mathematics::Matrix4x4 invViewM = GameEngine::Mathematics::Inverse(viewM);

    MaskParamsUBO ubo{};
    std::memcpy(ubo.InvProj, invProjM.Data(), sizeof(ubo.InvProj));
    std::memcpy(ubo.InvView, invViewM.Data(), sizeof(ubo.InvView));
    ubo.LightDirWS[0] = lightDirTowardLightWS[0];
    ubo.LightDirWS[1] = lightDirTowardLightWS[1];
    ubo.LightDirWS[2] = lightDirTowardLightWS[2];
    ubo.LightDirWS[3] = maxShadowDistance;
    ubo.RayParams[0]  = kMaskRayTMin;
    ubo.RayParams[1]  = kMaskRayTMax;
    ubo.RayParams[2]  = kMaskNormalOffsetBias;
    // The same slope the cascade PCSS path uses (penumbra = depthDelta * tan).
    const float tanHalfAngle = ResolveShadowTanHalfAngle(lightAngularDiameterDegrees);
    ubo.RayParams[3]  = tanHalfAngle;
    ubo.Temporal[0]   = accumulatesHistory
                            ? static_cast<float>(frame.FrameIndex() % kSoftShadowJitterCycle)
                            : 0.0f;
    ubo.Temporal[1]   = static_cast<float>(highQuality ? kQualityRaysPerPixel
                                                       : kPerformanceRaysPerPixel);
    ubo.Temporal[2]   = distanceFadeFraction;
    ubo.Temporal[3]   = 0.0f;
    RG::RGTexture terrainMap{};
    RG::RGTexture terrainHeight{};
    if (terrainShadow && terrainShadow->Map.IsValid() && terrainShadow->HeightTexture.IsValid() &&
        m_TerrainSampler.IsValid())
    {
        terrainMap = terrainShadow->Map;
        terrainHeight = frame.ImportExternalTexture("CBT.HeightSource", terrainShadow->HeightTexture,
                                                    Rendering::ResourceState::ShaderResource);
        ubo.TerrainGrid0[0] = static_cast<float>(terrainShadow->TerrainX + 0.5 * terrainShadow->TerrainSizeX);
        ubo.TerrainGrid0[1] = static_cast<float>(terrainShadow->TerrainZ + 0.5 * terrainShadow->TerrainSizeZ);
        ubo.TerrainGrid0[2] = static_cast<float>(terrainShadow->TerrainX);
        ubo.TerrainGrid0[3] = static_cast<float>(terrainShadow->TerrainZ);
        ubo.TerrainGrid1[0] = terrainShadow->TerrainSizeX;
        ubo.TerrainGrid1[1] = terrainShadow->TerrainSizeZ;
        ubo.TerrainGrid1[2] = terrainShadow->SunX;
        ubo.TerrainGrid1[3] = terrainShadow->SunZ;
        ubo.TerrainGrid2[0] = terrainShadow->TanElevation;
        ubo.TerrainGrid2[1] = terrainShadow->Texel;
        ubo.TerrainGrid2[2] = terrainShadow->UMin;
        ubo.TerrainGrid2[3] = terrainShadow->VMin;
        ubo.TerrainGrid3[0] = terrainShadow->SamplesU;
        ubo.TerrainGrid3[1] = terrainShadow->SamplesV;
        ubo.TerrainGrid3[2] = static_cast<float>(terrainShadow->BaseY);
        ubo.TerrainGrid3[3] = terrainShadow->HeightScale;
        ubo.TerrainSource[0] = terrainHeight.IsValid() ? 1u : 0u;
        ubo.TerrainSource[1] = terrainShadow->MapSide;
    }
    *ub.Ptr = ubo;

    const uint32_t gx = (dd.Width + 7) / 8;
    const uint32_t gy = (dd.Height + 7) / 8;
    char passName[kViewNameCapacity];
    FormatViewName(passName, "RTShadowMask.Mask[View#", viewId, "]");

    const RG::RGAccelerationStructure tlasRG = frame.ImportAccelerationStructure("RTShadowMask.TLAS", m_TlasSlot);
    frame.AddComputePass(
        passName, Rendering::PassPhase::kDefault,
        [&](RG::RGPassBuilder& p)
        {
            p.Read(depth, RG::RGTextureRead::Sampled);
            p.Read(tlasRG);
            p.Write(mask, RG::RGTextureWrite::Storage);
            if (terrainHeight.IsValid())
            {
                p.Read(terrainMap, RG::RGTextureRead::Sampled);
                p.Read(terrainHeight, RG::RGTextureRead::Sampled);
            }
        },
        [this, depth, mask, terrainMap, terrainHeight, ubBuffer = ub.Buffer, ubOffset = ub.Offset, gx, gy,
         tlasSlot = m_TlasSlot](RG::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl  = ctx.Cmd;
            if (!dev || !cl)
                return;
            const auto depthTex = ctx.GetTexture(depth);
            const auto maskTex  = ctx.GetTexture(mask);
            if (!depthTex.IsValid() || !maskTex.IsValid())
                return;

            Rendering::DescriptorSetDesc dsDesc{};
            dsDesc.layout    = m_MaskSet0Layout;
            dsDesc.transient = true;
            dsDesc.debugName = "RTShadowMask.Mask.Set0";
            auto ds = dev->CreateDescriptorSet(dsDesc);

            Rendering::NamedDescriptorWriter wd(dev, ds, *m_MaskMeta, 0);
            if (wd.Has("uDepth") && m_MaskSampler.IsValid())
                wd.AddCombinedImageSampler("uDepth", depthTex, m_MaskSampler);
            wd.AddUniformBuffer("RTShadowMaskParams", ubBuffer, ubOffset,
                                sizeof(MaskParamsUBO));
            // The terrain's clearance map and height texture; the depth stands in for both when no
            // map is published (the shader reads them only when uTerrainSource.x is set).
            const bool terrain = terrainHeight.IsValid();
            if (wd.Has("uTerrainClearance") && m_TerrainSampler.IsValid())
                wd.AddCombinedImageSampler("uTerrainClearance",
                                           terrain ? ctx.GetTexture(terrainMap) : depthTex, m_TerrainSampler);
            if (wd.Has("uTerrainHeight") && m_TerrainSampler.IsValid())
                wd.AddCombinedImageSampler("uTerrainHeight",
                                           terrain ? ctx.GetTexture(terrainHeight) : depthTex, m_TerrainSampler);
            // Bound TLAS (no address cast): the one AS-access form both Vulkan
            // and Metal can translate. A reflection miss is fatal to the query,
            // not a dimmer frame, so skip the dispatch rather than trace an
            // unwritten slot.
            if (!wd.TryAddAccelerationStructure("uTlas", tlasSlot))
            {
                Logger::Log::Error(
                    "RTShadowMask: rt_shadow_mask.comp reflects no 'uTlas' "
                    "acceleration-structure binding — mask pass skipped");
                return;
            }
            wd.Flush();
            Pipeline::Nodes::Detail::BindStorageImageByName(dev, ds, *m_MaskMeta, "uMask",
                                                            maskTex);

            const Rendering::PipelineHandle pipe =
                ctx.GetOrCreatePipelineVariant(m_MaskPipelineId);
            if (!pipe.IsValid())
                return;
            cl->SetMarker("RTShadowMask.Mask");
            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            cl->Dispatch(gx, gy, 1);
        });

    // Soft shadows only: the mask is a per-frame binary stipple that TAA resolves
    // in a temporal view but a non-TAA view leaves frozen. A plane-aware spatial
    // blur resolves that stipple into the smooth fractional penumbra without any
    // temporal history. At angular diameter 0 the mask is already a clean hard
    // edge and a bilateral blur would only soften it — so skip the denoise
    // entirely and hand back the raw mask.
    // A/B + debugging kill switch: GE_RT_SHADOW_DENOISE=0 returns the raw
    // (grainy) mask so the denoise can be compared against it from one binary.
    static const bool s_DenoiseEnabled = []
    {
        const char* v = std::getenv("GE_RT_SHADOW_DENOISE");
        return !(v && std::strcmp(v, "0") == 0);
    }();

    if (s_DenoiseEnabled && tanHalfAngle > 0.0f)
    {
        int kernelRadius = std::clamp(
            static_cast<int>(std::lround(lightAngularDiameterDegrees * kDenoiseRadiusPerDegree)),
            1, kDenoiseMaxRadius);
        if (highQuality)
            kernelRadius = std::max(1, kernelRadius / kQualityDenoiseRadiusDivisor);
        const RG::RGTexture denoised =
            DeclareDenoisePass(frame, viewId, mask, depth, cam, kernelRadius);
        if (denoised.IsValid())
            return denoised;
        // Denoise setup failed: fall back to the raw mask (grainy but correct)
        // rather than dropping the shadow term.
    }

    return mask;
}

Rendering::RenderGraph::RGTexture RTShadowMaskService::DeclareDenoisePass(
    Rendering::RenderGraph::RGFrame& frame, uint32_t viewId,
    Rendering::RenderGraph::RGTexture rawMask, Rendering::RenderGraph::RGTexture depth,
    const Rendering::CameraData& cam, int kernelRadius)
{
    namespace RG = Rendering::RenderGraph;

    if (!rawMask.IsValid() || !depth.IsValid())
        return {};

    LoadDenoiseShader();
    if (!m_DenoisePipelineId.IsValid() || !m_DenoiseMeta)
        return {};

    // The mask is full-res R8 built from this depth, so the depth desc gives the
    // denoise dimensions (a proven-queryable tracked resource).
    const auto& md = frame.Graph().ResourceDesc(depth.Id);
    if (md.Width == 0 || md.Height == 0 || md.SampleCount > 1)
        return {};

    // Per-view persistent import, matched to the raw mask (R8, full-res). Distinct
    // pool name so it aliases neither the raw mask nor another view's buffer.
    Rendering::TextureDesc td{};
    td.width       = md.Width;
    td.height      = md.Height;
    td.depth       = 1;
    td.mipLevels   = 1;
    td.arrayLayers = 1;
    td.sampleCount = 1;
    td.format      = static_cast<uint32_t>(Rendering::TextureFormat::R8_UNORM);
    td.usage       = static_cast<uint32_t>(Rendering::TextureUsage::UnorderedAccess
                                           | Rendering::TextureUsage::ShaderResource);
    char poolName[kViewNameCapacity];
    FormatViewName(poolName, "RTShadowMask.Denoise.View", viewId, "");
    td.debugName                 = poolName;
    const RG::RGTexture denoised = frame.ImportPersistentTexture(poolName, td);
    if (!denoised.IsValid())
        return {};

    // GLSL mirror: RTShadowDenoiseParams in rt_shadow_denoise.comp (std140).
    struct DenoiseParamsUBO
    {
        float InvProj[16];
        float Params[4];  // x = kernel radius, y = plane tolerance (kernel
                          // footprints), z = spatial sigma (texels), w reserved
    };
    auto ub = frame.AllocUpload<DenoiseParamsUBO>();
    if (!ub.Valid())
        return {};

    GameEngine::Mathematics::Matrix4x4 projM;
    std::memcpy(projM.Data(), cam.proj, sizeof(cam.proj));
    const GameEngine::Mathematics::Matrix4x4 invProjM = GameEngine::Mathematics::Inverse(projM);

    DenoiseParamsUBO ubo{};
    std::memcpy(ubo.InvProj, invProjM.Data(), sizeof(ubo.InvProj));
    ubo.Params[0] = static_cast<float>(kernelRadius);
    ubo.Params[1] = kDenoisePlaneToleranceTexels;
    ubo.Params[2] = static_cast<float>(kernelRadius) * kDenoiseSpatialSigmaScale;
    ubo.Params[3] = 0.0f;
    *ub.Ptr = ubo;

    const uint32_t gx = (md.Width + 7) / 8;
    const uint32_t gy = (md.Height + 7) / 8;
    char passName[kViewNameCapacity];
    FormatViewName(passName, "RTShadowMask.Denoise[View#", viewId, "]");

    frame.AddComputePass(
        passName, Rendering::PassPhase::kDefault,
        [&](RG::RGPassBuilder& p)
        {
            p.Read(rawMask, RG::RGTextureRead::Sampled);
            p.Read(depth, RG::RGTextureRead::Sampled);
            p.Write(denoised, RG::RGTextureWrite::Storage);
        },
        [this, rawMask, depth, denoised, ubBuffer = ub.Buffer, ubOffset = ub.Offset, gx,
         gy](RG::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl  = ctx.Cmd;
            if (!dev || !cl)
                return;
            const auto maskInTex  = ctx.GetTexture(rawMask);
            const auto depthTex   = ctx.GetTexture(depth);
            const auto maskOutTex = ctx.GetTexture(denoised);
            if (!maskInTex.IsValid() || !depthTex.IsValid() || !maskOutTex.IsValid())
                return;

            Rendering::DescriptorSetDesc dsDesc{};
            dsDesc.layout    = m_DenoiseSet0Layout;
            dsDesc.transient = true;
            dsDesc.debugName = "RTShadowMask.Denoise.Set0";
            auto ds = dev->CreateDescriptorSet(dsDesc);

            Rendering::NamedDescriptorWriter wd(dev, ds, *m_DenoiseMeta, 0);
            if (wd.Has("uMaskIn") && m_MaskSampler.IsValid())
                wd.AddCombinedImageSampler("uMaskIn", maskInTex, m_MaskSampler);
            if (wd.Has("uDepth") && m_MaskSampler.IsValid())
                wd.AddCombinedImageSampler("uDepth", depthTex, m_MaskSampler);
            wd.AddUniformBuffer("RTShadowDenoiseParams", ubBuffer, ubOffset,
                                sizeof(DenoiseParamsUBO));
            wd.Flush();
            Pipeline::Nodes::Detail::BindStorageImageByName(dev, ds, *m_DenoiseMeta, "uMaskOut",
                                                            maskOutTex);

            const Rendering::PipelineHandle pipe =
                ctx.GetOrCreatePipelineVariant(m_DenoisePipelineId);
            if (!pipe.IsValid())
                return;
            cl->SetMarker("RTShadowMask.Denoise");
            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            cl->Dispatch(gx, gy, 1);
        });

    return denoised;
}

}  // namespace GameEngine::Engine::Renderer
