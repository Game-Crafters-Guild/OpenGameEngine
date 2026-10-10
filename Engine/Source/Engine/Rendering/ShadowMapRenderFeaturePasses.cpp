// ShadowMapRenderFeaturePasses.cpp
// All five shadow-map pass DECLARATION bodies for ShadowMapRenderFeature —
// the directional cascade + glass-tint families (design A1.1 §0 tier 2, slice
// S1) and the punctual area/spot/point families (slice S2) — relocated here from
// RenderServices. The feature that owns the shadow textures now declares the
// passes that write them. It reaches the shared RenderServices plumbing through
// the RenderServices& that Declare receives: the passkey-gated
// ImportShadowMapArrayRG / ImportTransmittanceShadowArrayRG / BuildShadowPassResources
// / Publish{Area,Spot,Point}Shadow — never a friend grant. Each pass exec lambda
// records via the free RecordDepthOnlyPass over a bundle from rs.MakeDepthDrawServices().
// Recording order and the read/write sets are identical to the
// former RenderServices bodies (§2): each pass records into the same frame at the
// same sequence position, so the RGSchedule hazard edges are invariant under
// "which class owns the method".
#include "Engine/Rendering/ShadowMapRenderFeature.h"

#include "Engine/Rendering/CascadeCacheLogFormat.h"
#include "Engine/Rendering/DepthDrawRecorder.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Mathematics/Matrix4x4.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/RecomputeElision.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "RenderServicesDetail.h"

namespace GameEngine
{
namespace Engine::Renderer
{
using namespace ::GameEngine::Rendering;

namespace
{
// GE_SHADOW_STATIC_CACHE=0 forces the always-render baseline (the A/B lane
// and kill switch). Any other value (or unset) keeps static-scene cascade
// shadow caching ON — the standard GE_ flag idiom, flipped from review-gated
// default-OFF after the dual review's fixes landed and the runtime gates
// (bit-exact static parity, orbit/dynamic invalidation, tint family, zero
// validation errors) passed. Read once so the branch is stable for the
// process lifetime.
bool IsStaticShadowCacheEnabled()
{
    static const bool s_Enabled = []()
    {
        const char* env = std::getenv("GE_SHADOW_STATIC_CACHE");
        return !env || std::strcmp(env, "0") != 0;
    }();
    return s_Enabled;
}

// Cache-key view of the caster-reduction lane. Mirrors the file-local
// IsCasterReductionEnabled in ShadowMapRenderFeature.cpp (anonymous namespace
// there — same env var, same ON-unless-"0" semantics): the lane changes the
// cull planes and therefore the rendered caster set, so it is a content input.
bool CacheKeyCasterReductionLane()
{
    static const bool s_Enabled = []()
    {
        const char* env = std::getenv("GE_SHADOW_CASTER_REDUCTION");
        return !env || std::strcmp(env, "0") != 0;
    }();
    return s_Enabled;
}

// Assemble the full content-input record for one cascade (or tint) slot. Every
// field is something that changes the rasterized layer: the snapped fit, the
// caster footprint the cull tightens to, the main camera (GPU LOD selection
// consumes it at cull time), the caster content epoch, the pooled physical
// identity, and the config/LOD knobs. Contributor depth commands
// (terrain/ocean) are not epoch-tracked, so their presence pins the slot to
// always-render.
//
// Fit-freeze lane (`fitFreeze`): the camera and the footprint come from the
// cascade's FROZEN cull-content snapshot (CascadeFrameData::CullCameraViewProj
// and CasterFootprint — the values the LOD selection and the cull actually
// consume), so they only step when the cascade refits and camera-motion frames
// can resolve Cached. Freeze off: the live camera and this frame's footprint.
CascadeRenderInputs BuildCascadeCacheInputs(RenderServices& rs, ViewId viewId,
                                            const CascadeFrameData& fd, uint32_t cascadeIndex,
                                            uint64_t physicalId, uint32_t resolution,
                                            bool hasContributorCommands, bool fitFreeze,
                                            CascadeShadowCache& cache, uint32_t cacheSlot)
{
    CascadeRenderInputs in{};
    std::memcpy(in.LightVP, fd.LightVP[cascadeIndex].Data(), sizeof(in.LightVP));
    std::memcpy(in.LightVPRel, fd.LightVPRel[cascadeIndex].Data(), sizeof(in.LightVPRel));
    if (fitFreeze)
        std::memcpy(in.CameraViewProj, fd.CullCameraViewProj[cascadeIndex].Data(),
                    sizeof(in.CameraViewProj));
    if (fd.CasterFootprintApplies[cascadeIndex])
        std::memcpy(in.CasterFootprint, fd.CasterFootprint[cascadeIndex],
                    sizeof(in.CasterFootprint));
    in.RenderOriginSector[0] = fd.RenderOriginSector[0];
    in.RenderOriginSector[1] = fd.RenderOriginSector[1];
    in.RenderOriginSector[2] = fd.RenderOriginSector[2];
    if (const auto* viewDesc = rs.Views().FindViewDesc(viewId))
    {
        in.WorldId = viewDesc->worldId;
        in.CasterEpoch = cache.TrackCasterChanges(viewId, cacheSlot, in.WorldId,
            rs.GetShadowCasterChanges(in.WorldId), in.LightVP, resolution);
        in.RenderLayerMask = viewDesc->ActiveRenderLayerMask();
        if (!fitFreeze && viewDesc->cameraId != 0)
        {
            if (const auto* cam = rs.Views().FindCameraData(viewDesc->cameraId))
                std::memcpy(in.CameraViewProj, cam->viewProj, sizeof(in.CameraViewProj));
        }
    }
    in.PhysicalId = physicalId;
    in.Resolution = resolution;
    in.NumCascades = fd.NumCascades;
    // EXACTLY the values the bucketer consumes (RenderServicesGpuDriven
    // registerViewBatches): shadow slices select LODs with the SUM of the
    // global bias and the shadow bias, and the force level resolves the
    // per-view registry override before the global.
    in.ShadowLODBias = rs.GetLODGlobalBias() + rs.GetShadowLODBias();
    in.LODForceLevel = rs.ResolveViewForceLOD(viewId);
    {
        const RenderServices::ViewSseScales sse = rs.ComputeViewSseScales(viewId);
        in.SseThresholdToCoverage = sse.Default;
        in.SseThresholdToCoverageTight = sse.Tight;
    }
    // The mapping that seeded the rows those slices read. A switch rewrites
    // every row's switch points, so a retained cascade holds LOD picks the
    // camera pass no longer makes.
    in.SelectionMode =
        static_cast<uint32_t>(rs.GetMeshGPURegistry().GetLodSelectionMode());
    in.CasterReduction = CacheKeyCasterReductionLane();
    in.HasContributorDrawCommands = hasContributorCommands;
    return in;
}
} // namespace

void ShadowMapRenderFeature::Declare(Rendering::RenderGraph::RGFrame& frame, RenderServices& rs,
                                          const FeatureDeclareContext& ctx)
{
    m_CascadeDeclareFrameIndex = frame.FrameIndex();
    m_DeclaredCascadePassesThisFrame = 0;

    // Directional cascades + glass tint only when the view has a shadow-casting
    // directional light (the node's full branch). Depth cascades first, then the
    // glass-tint cascades: each tint reads the opaque cascade depth it follows
    // (read-only test), so the fixed cascade->tint order is what the schedule's
    // depth-write->depth-read edge relies on. Same order the node's two loops
    // produced pre-move.
    if (ctx.DirectionalLightDirWS)
    {
        // Motion round-robin plan first: every cascade pair's dirty class is
        // peeked (read-only) so the budget can be split across cascades BEFORE
        // any per-slot declare commits. The plan's DeferMask gates both loops
        // below — a deferred depth layer and its tint twin stay coherent.
        PlanCascadeMotionFrame(frame, rs, ctx);
        for (uint32_t c = 0; c < ctx.CascadeCount; ++c)
        {
            char suffix[16];
            std::snprintf(suffix, sizeof(suffix), "Cascade%u", c);
            const std::string name = ctx.PassName ? ctx.PassName(suffix) : std::string(suffix);
            DeclareCascadePass(frame, rs, ctx, c, name.c_str());
        }
        for (uint32_t c = 0; c < ctx.CascadeCount; ++c)
        {
            char suffix[24];
            std::snprintf(suffix, sizeof(suffix), "GlassTint%u", c);
            const std::string name = ctx.PassName ? ctx.PassName(suffix) : std::string(suffix);
            DeclareTransmittancePass(frame, rs, ctx, c, ctx.DirectionalLightDirWS, name.c_str());
        }
        // Min/max pyramid over the cascade depth the loops above just wrote —
        // only PCSS reads it, so only PCSS pays for it, and it is always built
        // under PCSS: its lit proof is the only way a PCSS fragment skips the
        // filter, and building it is faster than not on every measured scene.
        // The gate is structural: a declined Declare emits no passes at all. It
        // is still CALLED when declined, so the pyramid's level count drops to 0
        // with it instead of going stale on a switch away from PCSS.
        // EFFECTIVE quality, not the requested one: PCSS silently degrades to
        // Poisson without bindless, on a stable-filtering device, and when MSM4
        // is requested but its moments are missing it promotes INTO PCSS. Gating
        // on the request would build a pyramid the shader never queries in the
        // first cases and skip one it wants in the last. A declined pyramid
        // starves the shader's own pyramidIdx >= 0 guard automatically:
        // with no pyramid declared this frame, BuildShadowDataGPU finds nothing
        // to publish and leaves the negative sentinel in place.
        // The shader reads the pyramid through bindless views only; the compat
        // profile's PCSS runs unaccelerated, so it builds none.
        const bool buildPyramid = ctx.ViewNeedsCascades && IsInitialized() &&
                                  rs.Textures().IsBindlessEnabled() &&
                                  ResolveEffectiveFilterQuality(rs, ctx.View) ==
                                      ShadowFilterQuality::PCSS;
        RenderGraph::RGTexture pyramidSrc{};
        uint32_t pyramidCascades = 0;
        float pyramidKernelTexels = 0.0f;
        if (buildPyramid)
        {
            // Import (and adopt) BEFORE reading the cached frame data — the
            // ordering the cascade arms rely on, since a pooled-physical change
            // makes the adopt invalidate the cache a pointer taken earlier would
            // still be holding. The import also ADOPTS, so it stays inside this
            // gate: never a side effect of an off pyramid.
            pyramidSrc = rs.ImportShadowMapArrayRG(frame, ctx.View, ctx.Seam);
            // The RENDERED cascade count bounds the pyramid, not the pipeline's
            // configured maximum. A light asking for fewer cascades leaves the
            // upper array layers unwritten, and reducing those costs a full
            // level chain per layer over undefined depth — waste charged
            // entirely to the arm whose cost the A/B is measuring. Same clamp
            // the motion planner applies to the same quantity.
            if (const CascadeFrameData* frameData = GetCachedFrameData(ctx.View))
            {
                pyramidCascades =
                    std::min({frameData->NumCascades, ctx.CascadeCount, kMaxShadowCascades});
                pyramidKernelTexels =
                    PcssWidestKernelTexels(ctx.View, *frameData, pyramidCascades);
            }
        }
        // Publish whatever Declare returned, invalid handle included, so the id
        // the world arm reads and the passes declared here always describe the
        // same decision. The world arm's Read on it is the only memory
        // dependency between the reduction and the fragment fetches, which reach
        // the pyramid bindlessly.
        rs.PublishPcssPyramid(ctx.Seam, frame, ctx.View,
                              m_MinMaxPyramid.Declare(frame, rs, ctx.View, pyramidSrc,
                                                      GetConfig().Resolution, pyramidCascades,
                                                      pyramidKernelTexels, buildPyramid));
    }

    // Punctual families are independent of the directional light: always
    // attempted, each no-ops unless its ctx snapshot is present. Declared in the
    // fixed area->spot->point order the node's no-light branch and the S1 full
    // branch's rs->Add* calls both produced — so recording order is unchanged.
    DeclareAreaPass(frame, rs, ctx);
    DeclareSpotPass(frame, rs, ctx);
    DeclarePointPasses(frame, rs, ctx);
}

void ShadowMapRenderFeature::PlanCascadeMotionFrame(Rendering::RenderGraph::RGFrame& frame,
                                                    RenderServices& rs,
                                                    const FeatureDeclareContext& ctx)
{
    namespace RenderGraph = Rendering::RenderGraph;
    const ViewId viewId = ctx.View;

    MotionFramePlan plan{};
    plan.FrameStamp = frame.FrameIndex();

    // Deferral requires the static-cache lane: its records define the retained
    // content the deferred slots keep sampling, and its under-draw feedback is
    // what stops a silently-short layer from being frozen. GE_SHADOW_STATIC_CACHE=0
    // (the always-render kill switch) therefore also disables the motion cap.
    // This scheduling policy is independent of contact-shadow enablement.
    const uint32_t cap = m_MotionCap;
    if (cap == 0 || !IsStaticShadowCacheEnabled() || !ctx.ViewNeedsCascades || !IsInitialized())
    {
        m_MotionPlanByView[viewId] = plan; // empty plan: nothing defers
        return;
    }

    // Mirror the declare sites' gates so a slot they would bail on is never
    // classified. Import (and adopt) FIRST — a pooled-physical change
    // invalidates the cache + content fits, and both the peeks below and the
    // later declares must see the post-adopt state.
    const RenderGraph::RGTexture shadowArr = rs.ImportShadowMapArrayRG(frame, viewId, ctx.Seam);
    const CascadeFrameData* frameData = GetCachedFrameData(viewId);
    if (!shadowArr.IsValid() || !frameData)
    {
        m_MotionPlanByView[viewId] = plan;
        return;
    }
    RenderGraph::RGTexture tintArr{};
    if (ctx.HasTransmissiveCaster)
        tintArr = rs.ImportTransmittanceShadowArrayRG(frame, viewId, ctx.Seam);
    const bool tintDeclares = ctx.HasTransmissiveCaster && tintArr.IsValid();

    const uint32_t numCascades =
        std::min({frameData->NumCascades, ctx.CascadeCount, kMaxShadowCascades});
    const uint32_t res = GetConfig().Resolution;
    const bool cascadeContrib = rs.HasDepthCommands(viewId, DepthPassType::ShadowCascade);
    const bool tintContrib =
        cascadeContrib || rs.HasDepthCommands(viewId, DepthPassType::TransmittanceCascade);
    const uint64_t depthPhys = static_cast<uint64_t>(frame.PhysicalTexture(shadowArr));
    const uint64_t tintPhys =
        tintDeclares ? static_cast<uint64_t>(frame.PhysicalTexture(tintArr)) : 0;

    const std::array<CascadeContentFit, kMaxShadowCascades>* fits = nullptr;
    if (const auto it = m_CascadeContentFit.find(viewId); it != m_CascadeContentFit.end())
        fits = &it->second;
    const std::array<CascadeContentFit, kMaxShadowCascades>* tintFits = nullptr;
    if (const auto it = m_TintContentFit.find(viewId); it != m_TintContentFit.end())
        tintFits = &it->second;

    // Classify each cascade PAIR: the tint twin samples through the SAME
    // ge_shadowVP as its depth layer, so the pair defers or renders as a unit
    // — a correctness cause on EITHER family cancels the pair's deferral (all
    // correctness classes render immediately, every time), and a Motion pair
    // may only defer when both families' retained content shares ONE fit
    // (ClassifyCascadePair's lockstep gate — the depth stamp bounds nothing
    // about a tint layer retained across a transmission-visibility gap).
    std::array<CascadeUpdateClass, CascadeMotionScheduler::kMaxCascades> classes{};
    std::array<uint64_t, CascadeMotionScheduler::kMaxCascades> staleness{};
    staleness.fill(CascadeMotionScheduler::kStalenessUnknown);
    for (uint32_t c = 0; c < numCascades; ++c)
    {
        const CascadeRenderInputs depthIn = BuildCascadeCacheInputs(
            rs, viewId, *frameData, c, depthPhys, res, cascadeContrib, m_FitFreezeEnabled,
            m_CascadeShadowCache, CascadeShadowCache::kDepthSlotBase + c);
        const bool depthPending = rs.DepthUnderDraw().Peek(
            static_cast<uint32_t>(viewId), static_cast<uint8_t>(DepthPassType::ShadowCascade), c);
        const CascadeCacheDirtyCause depthCause = m_CascadeShadowCache.PeekCause(
            viewId, CascadeShadowCache::kDepthSlotBase + c, depthIn, depthPending);
        CascadeCacheDirtyCause tintCause{};
        bool lockstep = false;
        if (tintDeclares)
        {
            const CascadeRenderInputs tintIn = BuildCascadeCacheInputs(
                rs, viewId, *frameData, c, tintPhys, res, tintContrib, m_FitFreezeEnabled,
                m_CascadeShadowCache, CascadeShadowCache::kTintSlotBase + c);
            const bool tintPending = rs.DepthUnderDraw().Peek(
                static_cast<uint32_t>(viewId),
                static_cast<uint8_t>(DepthPassType::TransmittanceCascade), c);
            tintCause = m_CascadeShadowCache.PeekCause(
                viewId, CascadeShadowCache::kTintSlotBase + c, tintIn, tintPending);
            lockstep = fits && tintFits && (*fits)[c].Valid && (*tintFits)[c].Valid &&
                       std::memcmp((*fits)[c].LightVP.Data(), (*tintFits)[c].LightVP.Data(),
                                   16 * sizeof(float)) == 0;
        }
        classes[c] =
            ClassifyCascadePair(depthCause, tintDeclares ? &tintCause : nullptr, lockstep);
        if (fits && (*fits)[c].Valid)
            staleness[c] = frame.FrameIndex() - (*fits)[c].FrameStamp;
    }

    plan.DeferMask = m_MotionScheduler
                         .PlanFrame(static_cast<uint32_t>(viewId), numCascades, classes, staleness,
                                    cap, m_MotionMaxAge)
                         .DeferMask;
    m_MotionPlanByView[viewId] = plan;
}

bool ShadowMapRenderFeature::IsCascadeDeferredThisFrame(Rendering::ViewId viewId,
                                                        uint64_t frameStamp,
                                                        uint32_t cascadeIndex) const
{
    const auto it = m_MotionPlanByView.find(viewId);
    return it != m_MotionPlanByView.end() && it->second.FrameStamp == frameStamp &&
           cascadeIndex < 32 && ((it->second.DeferMask >> cascadeIndex) & 1u) != 0;
}

Rendering::RenderGraph::RGPass ShadowMapRenderFeature::DeclareCascadePass(
    Rendering::RenderGraph::RGFrame& frame, RenderServices& rs, const FeatureDeclareContext& ctx,
    uint32_t cascadeIndex, const char* passName)
{
    namespace RenderGraph = Rendering::RenderGraph;
    const ViewId viewId = ctx.View;

    // The old activation predicate, evaluated at declaration (snapshotted).
    if (!ctx.ViewNeedsCascades)
        return {};
    if (!IsInitialized())
        return {};

    if (ctx.WorldAlreadyDeclared)
        Logger::Log::Error(
            "[RenderGraph] AddShadowCascadePassForView(view {}, cascade {}): the world pass already "
            "declared — a later cascade derives WAR (the world samples frame N−1 shadows); "
            "declare producers BEFORE the world pass",
            viewId, cascadeIndex);

    // Import (and adopt) BEFORE fetching the cached frame data: a pooled-
    // physical change makes AdoptPooledShadowMap invalidate the feature's
    // cache, so a pointer taken earlier would dangle. A null cache after the
    // adopt skips the pass for one frame — matching the old recreate
    // semantics on config-change frames.
    const RenderGraph::RGTexture shadowArr = rs.ImportShadowMapArrayRG(frame, viewId, ctx.Seam);
    if (!shadowArr.IsValid())
        return {};

    const CascadeFrameData* frameData = GetCachedFrameData(viewId);
    if (!frameData || cascadeIndex >= frameData->NumCascades)
        return {};

    const uint32_t res = GetConfig().Resolution;

    // Static-scene cascade cache (GE_SHADOW_STATIC_CACHE, default ON; =0
    // forces the always-render baseline): when
    // every content input matches the last settled render, skip the whole pass
    // — the pooled persistent layer already holds exactly what a re-render
    // would produce, and the world pass keeps sampling it through the import
    // above (the point-shadow L1a contract, cascade edition). Evaluated even
    // when disabled so the periodic stats measure the achievable hit rate; a
    // miss falls through to the normal declare and commits via OnRendered.
    const uint32_t cacheSlot = CascadeShadowCache::kDepthSlotBase + cascadeIndex;
    const CascadeRenderInputs cacheIn = BuildCascadeCacheInputs(
        rs, viewId, *frameData, cascadeIndex,
        static_cast<uint64_t>(frame.PhysicalTexture(shadowArr)), res,
        rs.HasDepthCommands(viewId, DepthPassType::ShadowCascade), m_FitFreezeEnabled,
        m_CascadeShadowCache, cacheSlot);
    if (cascadeIndex == 0)
        LogCascadeCacheStatsPeriodic(viewId, IsStaticShadowCacheEnabled(),
                                     rs.ShadowCasterContentVersion(cacheIn.WorldId));
    // Exec→declare feedback: the last declared render for this slice reported a
    // silent under-draw (async publish gate / stale-survivor walk-skip /
    // missing GPU content). Consume the mark and dirty the record — restores
    // always-render's one-frame self-heal under the cache.
    if (rs.DepthUnderDraw().Consume(static_cast<uint32_t>(viewId),
                                    static_cast<uint8_t>(DepthPassType::ShadowCascade),
                                    cascadeIndex))
        m_CascadeShadowCache.MarkUnderDrew(viewId, cacheSlot);
    if (m_CascadeShadowCache
            .Evaluate(viewId, cacheSlot, frame.FrameIndex(), cacheIn, IsStaticShadowCacheEnabled(),
                      /*includeCameraInSettle=*/m_FitFreezeEnabled)
            .Skip)
    {
        // Cached skip: the retained content is byte-identical to the current
        // fit, so refresh its stamp — the motion scheduler's staleness input
        // measures drift, and provably-current content has none.
        if (const auto it = m_CascadeContentFit.find(viewId);
            it != m_CascadeContentFit.end() && it->second[cascadeIndex].Valid)
            it->second[cascadeIndex].FrameStamp = frame.FrameIndex();
        return {};
    }
    // Motion round-robin deferral (GE_SHADOW_MOTION_CAP): the frame plan chose
    // to keep this cascade's retained layer for one more frame. No pass, no
    // OnRendered — the cache record and content fit keep describing the
    // retained content, and BuildShadowDataGPU samples it through that fit.
    if (IsCascadeDeferredThisFrame(viewId, frame.FrameIndex(), cascadeIndex))
        return {};

    // Cascade light VP as the camera, written at declaration. Both the world VP
    // (viewProj, taken when the origin is inactive => byte-identical legacy path)
    // and the camera-relative VP (viewProjRel + renderOriginSector, taken when the
    // origin is active) are filled, so shadow_depth_shared.vert's GE_ClipFromSectorLocal
    // rasterizes casters at fp32-of-small-magnitude at planetary distance (kills the
    // self-shadow acne) while staying identical near the origin.
    auto cam = frame.AllocUpload<CameraData>();
    if (cam.Valid())
    {
        CameraData shadowCam{};
        std::memcpy(shadowCam.view, kIdentity4x4.Data(), 64);
        std::memcpy(shadowCam.proj, kIdentity4x4.Data(), 64);
        std::memcpy(shadowCam.viewProj, frameData->LightVP[cascadeIndex].Data(), 64);
        std::memcpy(shadowCam.viewProjRel, frameData->LightVPRel[cascadeIndex].Data(), 64);
        shadowCam.renderOriginSector[0] = frameData->RenderOriginSector[0];
        shadowCam.renderOriginSector[1] = frameData->RenderOriginSector[1];
        shadowCam.renderOriginSector[2] = frameData->RenderOriginSector[2];
        shadowCam.renderOriginSector[3] = static_cast<int32_t>(kSectorSize);
        *cam.Ptr = shadowCam;
    }

    DepthOnlyPassParamsRG params{};
    params.ViewId = viewId;
    params.PassType = DepthPassType::ShadowCascade;
    params.CascadeIndex = cascadeIndex;
    params.DepthBiasEnable = true;
    // Pancaking. A directional light's caster ray is infinite, so the cascade's
    // near plane is an arbitrary cut through the caster set rather than a real
    // bound — casters nearer the light clamp to the near depth instead of being
    // clipped away. Paired with the retired near cull plane in OnScheduleCulling:
    // culling admits them, this lets them rasterize. Punctual shadows do NOT set
    // this — nothing can sit behind a spot or point light, so their near plane is
    // a genuine bound.
    params.DepthClampEnable = true;
    params.RasterizationSamples = 1;
    params.ViewportWidth = res;
    params.ViewportHeight = res;
    params.Resources = rs.BuildShadowPassResources(ctx.Seam, frame, viewId, cam.Buffer, cam.Offset);

    RenderServices* rsPtr = &rs;
    const RenderGraph::RGPass pass = frame.AddPass(
        passName, Rendering::PassPhase::kEarlySetup,
        [this, &ctx, shadowArr, cascadeIndex, viewId, frameData,
         frameStamp = frame.FrameIndex()](RenderGraph::RGPassBuilder& p)
        {
            // A2.4-P0-R: each directional depth cascade renders into its own array
            // layer with a self-contained exec (own viewport/scissor + per-draw
            // binds), so the 4 cascades record into workers concurrently.
            p.RecordInSecondary();
            // Builder runs synchronously inside AddPass — it MAY read ctx.
            if (ctx.SkinPaletteAtlas.IsValid())
                p.Read(ctx.SkinPaletteAtlas);
            else if (ctx.HasPendingSkinnedInstances)
                Logger::Log::Error(
                    "[RenderGraph] AddShadowCascadePassForView(view {}, cascade {}): skinned "
                    "instances pending but no SkinPaletteAtlas value is published — declare "
                    "ScheduleGpuSkinningAndRetarget(frame) BEFORE the cascade passes",
                    ctx.View, cascadeIndex);
            if (ctx.DrawStreamOrdering.IsValid())
                p.Read(ctx.DrawStreamOrdering, RenderGraph::RGBufferRead::Indirect);
            else if (ctx.HasPendingBucketerSlices)
                Logger::Log::Error(
                    "[RenderGraph] AddShadowCascadePassForView(view {}, cascade {}): buckets are "
                    "registered but no DrawStreamOrdering value is published — declare "
                    "ScheduleWorldBucketerDispatches(frame) BEFORE the cascade passes",
                    ctx.View, cascadeIndex);

            RenderGraph::RGAttachmentOps dops{};
            dops.Load = RenderGraph::RGLoadOp::Clear;
            dops.Store = RenderGraph::RGStoreOp::Store;
            dops.Clear.Depth = 0.0f; // reverse-Z: clear to far
            // Per-layer attach: 2b-1 cell tracking keeps the four cascades from
            // self-serializing beyond their real hazards.
            p.AttachDepth(shadowArr, dops, RenderGraph::RGDepthAccess::ReadWrite,
                          RenderGraph::RGRange{.BaseLayer = cascadeIndex, .LayerCount = 1});
            // A declared cascade must actually clear+rasterize the persistent
            // layer. Culling it leaves reverse-Z uncleared (1.0 = fully
            // shadowed) which the static cache would then freeze.
            p.PreventCulling();
            // Content-fit commit stays at DECLARE: BuildShadowDataGPU uploads this
            // snapshot for receivers in the same frame, before any pass executes.
            // Committing it at exec would leave receivers transforming by the
            // PREVIOUS frame's fit while sampling texels rasterized under this
            // one — a one-frame mismatch that shifts shadow edges whenever the
            // fit moves. PreventCulling above is what makes this safe: the layer
            // is guaranteed to clear+rasterize under exactly this fit.
            CascadeContentFit& cf = m_CascadeContentFit[viewId][cascadeIndex];
            cf.LightVP = frameData->LightVP[cascadeIndex];
            cf.LightVPRel = frameData->LightVPRel[cascadeIndex];
            cf.Sector[0] = frameData->RenderOriginSector[0];
            cf.Sector[1] = frameData->RenderOriginSector[1];
            cf.Sector[2] = frameData->RenderOriginSector[2];
            cf.OrthoHalfExtent = frameData->OrthoHalfExtent[cascadeIndex];
            cf.DepthSpan = frameData->DepthSpan[cascadeIndex];
            cf.FrameStamp = frameStamp;
            cf.Valid = true;
        },
        [this, rsPtr, params = std::move(params), viewId, cacheSlot,
         cacheIn](RenderGraph::RGContext& c) mutable
        {
            RecordDepthOnlyPass(c, std::move(params), rsPtr->MakeDepthDrawServices());
            // Settle stamp only: this records that the layer was actually
            // rasterized. The fit receivers use is committed at declare.
            m_CascadeShadowCache.OnRendered(viewId, cacheSlot, cacheIn);
        });
    ++m_DeclaredCascadePassesThisFrame;
    return pass;
}

Rendering::RenderGraph::RGPass ShadowMapRenderFeature::DeclareTransmittancePass(
    Rendering::RenderGraph::RGFrame& frame, RenderServices& rs, const FeatureDeclareContext& ctx,
    uint32_t cascadeIndex, const float lightDirWS[3], const char* passName)
{
    namespace RenderGraph = Rendering::RenderGraph;
    const ViewId viewId = ctx.View;

    if (!ctx.ViewNeedsCascades)
        return {};
    // Only pay for the glass-tint pass when this view actually has transmissive
    // CASTERS (derived with the batch keys, snapshotted into the ctx). Visible
    // glass that casts no shadow is dropped by the shadow cull, so its tint layer
    // would clear to exactly the 1x1 fallback's texel — the array and the four
    // passes buy nothing. Views without one never allocate the array or declare.
    if (!ctx.HasTransmissiveCaster)
        return {};
    if (!IsInitialized())
        return {};

    if (ctx.WorldAlreadyDeclared)
        Logger::Log::Error(
            "[RenderGraph] AddTransmittanceShadowCascadePassForView(view {}, cascade {}): the world "
            "pass already declared — the tint sample would read frame N-1; declare producers BEFORE "
            "the world pass",
            viewId, cascadeIndex);

    // The opaque cascade depth (read-only test target) + the glass tint colour
    // array. The depth import dedups by physical with the depth cascade arm's,
    // so reading it here forms the depth-write -> depth-read edge that orders
    // this pass AFTER the cascade depth pass.
    const RenderGraph::RGTexture shadowArr = rs.ImportShadowMapArrayRG(frame, viewId, ctx.Seam);
    const RenderGraph::RGTexture tintArr = rs.ImportTransmittanceShadowArrayRG(frame, viewId, ctx.Seam);
    if (!shadowArr.IsValid() || !tintArr.IsValid())
        return {};

    const CascadeFrameData* frameData = GetCachedFrameData(viewId);
    if (!frameData || cascadeIndex >= frameData->NumCascades)
        return {};

    const uint32_t res = GetConfig().Resolution;

    // Static-scene cache, tint family: the tint layer's content is a function
    // of the SAME inputs as the depth cascade (fit + camera + caster epoch)
    // plus its own pooled physical. Contributor pin: the tint depth-tests
    // read-only against the OPAQUE cascade layer, and contributor emitters
    // (terrain/ocean) only queue ShadowCascade commands — a tint-only
    // HasDepthCommands probe would be constant-false. Pin the tint whenever
    // EITHER family carries contributor commands, so cached tint can never
    // pair with fresher contributor-written opaque depth.
    const uint32_t cacheSlot = CascadeShadowCache::kTintSlotBase + cascadeIndex;
    const CascadeRenderInputs cacheIn = BuildCascadeCacheInputs(
        rs, viewId, *frameData, cascadeIndex,
        static_cast<uint64_t>(frame.PhysicalTexture(tintArr)), res,
        rs.HasDepthCommands(viewId, DepthPassType::ShadowCascade) ||
            rs.HasDepthCommands(viewId, DepthPassType::TransmittanceCascade),
        m_FitFreezeEnabled, m_CascadeShadowCache, cacheSlot);
    if (rs.DepthUnderDraw().Consume(static_cast<uint32_t>(viewId),
                                    static_cast<uint8_t>(DepthPassType::TransmittanceCascade),
                                    cascadeIndex))
        m_CascadeShadowCache.MarkUnderDrew(viewId, cacheSlot);
    if (m_CascadeShadowCache
            .Evaluate(viewId, cacheSlot, frame.FrameIndex(), cacheIn, IsStaticShadowCacheEnabled(),
                      /*includeCameraInSettle=*/m_FitFreezeEnabled)
            .Skip)
    {
        // Cached skip: the retained tint content is byte-identical to the
        // current fit — refresh its stamp so the pair-lockstep gate keeps
        // seeing matching content fits (mirrors the depth family).
        if (const auto it = m_TintContentFit.find(viewId);
            it != m_TintContentFit.end() && it->second[cascadeIndex].Valid)
            it->second[cascadeIndex].FrameStamp = frame.FrameIndex();
        return {};
    }
    // Motion round-robin deferral: the tint twin follows its depth cascade's
    // defer decision (same DeferMask bit) — both sample through the ONE
    // ge_shadowVP, so the pair's retained content must stay in lockstep. The
    // plan phase already canceled the deferral if EITHER family carried a
    // correctness-class cause or the families' content fits desynced
    // (transmission-visibility gap).
    if (IsCascadeDeferredThisFrame(viewId, frame.FrameIndex(), cascadeIndex))
        return {};

    // Same cascade light VP as the depth pass — glass must rasterize to the exact
    // texels of the opaque depth it tests against, so it takes the identical
    // camera-relative treatment (viewProjRel + renderOriginSector).
    auto cam = frame.AllocUpload<CameraData>();
    if (cam.Valid())
    {
        CameraData shadowCam{};
        std::memcpy(shadowCam.view, kIdentity4x4.Data(), 64);
        std::memcpy(shadowCam.proj, kIdentity4x4.Data(), 64);
        std::memcpy(shadowCam.viewProj, frameData->LightVP[cascadeIndex].Data(), 64);
        std::memcpy(shadowCam.viewProjRel, frameData->LightVPRel[cascadeIndex].Data(), 64);
        shadowCam.renderOriginSector[0] = frameData->RenderOriginSector[0];
        shadowCam.renderOriginSector[1] = frameData->RenderOriginSector[1];
        shadowCam.renderOriginSector[2] = frameData->RenderOriginSector[2];
        shadowCam.renderOriginSector[3] = static_cast<int32_t>(kSectorSize);
        // C1.5 caustic focusing: the tint fragment refracts the light through the
        // glass normal, so it needs the cascade light direction. cameraPos is
        // unused for an ortho light shadow camera, so carry the (caller-resolved,
        // shadow-casting) directional light's world direction there.
        shadowCam.cameraPos[0] = lightDirWS[0];
        shadowCam.cameraPos[1] = lightDirWS[1];
        shadowCam.cameraPos[2] = lightDirWS[2];
        *cam.Ptr = shadowCam;
    }

    DepthOnlyPassParamsRG params{};
    params.ViewId = viewId;
    params.PassType = DepthPassType::TransmittanceCascade;
    params.CascadeIndex = cascadeIndex;
    // Same bias as the opaque cascade so it CANCELS in the read-only compare: the
    // opaque depth was stored biased; biasing the glass test depth identically
    // makes the test glassDepth >= opaqueTrueDepth (no half-bias-band of
    // glass-behind-receiver leaking tint).
    params.DepthBiasEnable = true;
    // Clamp like the opaque cascade: the tint pass tests against that cascade's
    // depth, so a glass caster the opaque pass clamped in must be able to reach
    // the same texels rather than be clipped out of the comparison.
    params.DepthClampEnable = true;
    params.RasterizationSamples = 1;
    params.ViewportWidth = res;
    params.ViewportHeight = res;
    params.Resources = rs.BuildShadowPassResources(ctx.Seam, frame, viewId, cam.Buffer, cam.Offset);

    RenderServices* rsPtr = &rs;
    const RenderGraph::RGPass pass = frame.AddPass(
        passName, Rendering::PassPhase::kEarlySetup,
        [&ctx, shadowArr, tintArr, cascadeIndex](RenderGraph::RGPassBuilder& p)
        {
            // A2.4-P0-R: same self-contained RecordDepthOnlyPass exec as the depth
            // cascades (own viewport/scissor + per-draw binds), per-layer attach
            // isolation on both the tint color and the read-only depth.
            p.RecordInSecondary();
            if (ctx.SkinPaletteAtlas.IsValid())
                p.Read(ctx.SkinPaletteAtlas);
            else if (ctx.HasPendingSkinnedInstances)
                Logger::Log::Error(
                    "[RenderGraph] AddTransmittanceShadowCascadePassForView(view {}, cascade {}): "
                    "skinned instances pending but no SkinPaletteAtlas value is published — declare "
                    "ScheduleGpuSkinningAndRetarget(frame) BEFORE the cascade passes",
                    ctx.View, cascadeIndex);
            if (ctx.DrawStreamOrdering.IsValid())
                p.Read(ctx.DrawStreamOrdering, RenderGraph::RGBufferRead::Indirect);
            else if (ctx.HasPendingBucketerSlices)
                Logger::Log::Error(
                    "[RenderGraph] AddTransmittanceShadowCascadePassForView(view {}, cascade {}): "
                    "buckets are registered but no DrawStreamOrdering value is published — declare "
                    "ScheduleWorldBucketerDispatches(frame) BEFORE the cascade passes",
                    ctx.View, cascadeIndex);

            RenderGraph::RGAttachmentOps col{};
            col.Load = RenderGraph::RGLoadOp::Clear;
            col.Store = RenderGraph::RGStoreOp::Store;
            col.Clear.Color[0] = 1.0f; // RGB white = full transmission (no tint)
            col.Clear.Color[1] = 1.0f;
            col.Clear.Color[2] = 1.0f;
            col.Clear.Color[3] = 0.0f; // A = 0 = no glass present (gates the caustic)
            p.AttachColor(0, tintArr, col,
                          RenderGraph::RGRange{.BaseLayer = cascadeIndex, .LayerCount = 1});

            // Read-only test against the opaque cascade depth: keep it (Load),
            // never write it — the ReadOnly attach records StoreOp::None, so the
            // cascade the world pass samples later survives untouched.
            RenderGraph::RGAttachmentOps dops{};
            dops.Load = RenderGraph::RGLoadOp::Load;
            p.AttachDepth(shadowArr, dops, RenderGraph::RGDepthAccess::ReadOnly,
                          RenderGraph::RGRange{.BaseLayer = cascadeIndex, .LayerCount = 1});
        },
        [this, rsPtr, params = std::move(params), viewId, cacheSlot, cacheIn, cascadeIndex,
         lightVP = frameData->LightVP[cascadeIndex],
         frameStamp = frame.FrameIndex()](RenderGraph::RGContext& c) mutable
        {
            RecordDepthOnlyPass(c, std::move(params), rsPtr->MakeDepthDrawServices());
            m_CascadeShadowCache.OnRendered(viewId, cacheSlot, cacheIn);
            CascadeContentFit& cf = m_TintContentFit[viewId][cascadeIndex];
            cf.LightVP = lightVP;
            cf.FrameStamp = frameStamp;
            cf.Valid = true;
        });
    return pass;
}

Rendering::RenderGraph::RGPass ShadowMapRenderFeature::DeclareAreaPass(
    Rendering::RenderGraph::RGFrame& frame, RenderServices& rs, const FeatureDeclareContext& ctx)
{
    namespace RenderGraph = Rendering::RenderGraph;
    const ViewId viewId = ctx.View;

    // The ctx snapshot carries the area gate: no valid area light => decline (the
    // world table omits AreaShadowData and the fallback buffer covers the binding).
    if (!ctx.AreaShadow)
        return {};
    const AreaShadowFrameInfo& areaShadow = *ctx.AreaShadow;

    const uint32_t res = std::max(1u, ctx.PunctualResolution);

    if (ctx.WorldAlreadyDeclared)
        Logger::Log::Error(
            "[RenderGraph] AddAreaShadowPassForView(view {}): the world pass already declared — a "
            "later area pass derives WAR (the world samples frame N−1 shadows); declare "
            "producers BEFORE the world pass",
            viewId);

    // The per-view depth map. Frame-local ViewFrameRG makes the old
    // "if (!vfr.AreaShadowMap.IsValid())" dedup always-taken within one
    // declaration, so create unconditionally and publish write-only. The
    // persistence key (name) is unchanged, so the pooled physical persists across
    // frames exactly as before.
    Rendering::TextureDesc td{};
    td.width = res;
    td.height = res;
    td.depth = 1;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.format = static_cast<uint32_t>(Rendering::TextureFormat::D32_FLOAT);
    td.usage = static_cast<uint32_t>(Rendering::TextureUsage::DepthStencil)
             | static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
    td.sampleCount = 1;
    const std::string mapName =
        "AreaShadowMap.View" + std::to_string(static_cast<uint32_t>(viewId));
    td.debugName = mapName.c_str();
    const RenderGraph::RGTexture areaMap = frame.ImportPersistentTexture(mapName.c_str(), td);
    if (!areaMap.IsValid())
        return {};
    // The world pass binds ge_areaShadowMap with these samplers; ensure they
    // exist before its table is built (mirrors the old arm's eager call).
    rs.GetAreaShadowSampler();

    // THE alloc the world pass's resolved table reads ("AreaShadowData") — written
    // here at declaration; host-coherent, no graph declaration.
    auto data = frame.AllocUpload<AreaShadowDataGPU>();
    if (data.Valid())
    {
        AreaShadowDataGPU d{};
        std::memcpy(d.areaShadowVP, areaShadow.lightView.Data(), sizeof(d.areaShadowVP));
        d.areaShadowParams[0] = 1.0f;
        d.areaShadowParams[1] = static_cast<float>(areaShadow.packedLightIndex);
        d.areaShadowParams[2] = 0.0005f;
        d.areaShadowParams[3] = 0.02f;
        d.areaShadowParams2[0] = static_cast<float>(res);
        d.areaShadowParams2[1] = areaShadow.farPlane;
        d.areaShadowParams2[2] = areaShadow.lightSize;
        d.areaShadowParams2[3] = 0.0f;
        *data.Ptr = d;
    }
    rs.PublishAreaShadow(ctx.Seam, frame, viewId, areaMap, data);

    auto cam = frame.AllocUpload<CameraData>();
    if (cam.Valid())
    {
        CameraData shadowCam{};
        std::memcpy(shadowCam.view, areaShadow.lightView.Data(), 64);
        std::memcpy(shadowCam.proj, areaShadow.lightProj.Data(), 64);
        std::memcpy(shadowCam.viewProj, areaShadow.lightVP.Data(), 64);
        shadowCam.cameraPos[0] = areaShadow.position.x;
        shadowCam.cameraPos[1] = areaShadow.position.y;
        shadowCam.cameraPos[2] = areaShadow.position.z;
        shadowCam.cameraPos[3] = -areaShadow.farPlane;
        *cam.Ptr = shadowCam;
    }

    DepthOnlyPassParamsRG params{};
    params.ViewId = viewId;
    params.PassType = DepthPassType::AreaShadow;
    params.CascadeIndex = kAreaShadowCullingIndex;
    params.DepthBiasEnable = true;
    params.RasterizationSamples = 1;
    params.ViewportWidth = res;
    params.ViewportHeight = res;
    params.Resources = rs.BuildShadowPassResources(ctx.Seam, frame, viewId, cam.Buffer, cam.Offset);

    const std::string passName =
        "AreaShadow[View#" + std::to_string(static_cast<uint32_t>(viewId)) + "]";

    RenderServices* rsPtr = &rs;
    return frame.AddPass(
        passName.c_str(), Rendering::PassPhase::kEarlySetup,
        [&ctx, areaMap](RenderGraph::RGPassBuilder& p)
        {
            // A2.4-P0-R: self-contained RecordDepthOnlyPass exec over a dedicated
            // depth target — worker-recordable like the cascades.
            p.RecordInSecondary();
            if (ctx.SkinPaletteAtlas.IsValid())
                p.Read(ctx.SkinPaletteAtlas);
            else if (ctx.HasPendingSkinnedInstances)
                Logger::Log::Error(
                    "[RenderGraph] AddAreaShadowPassForView(view {}): skinned instances pending but "
                    "no SkinPaletteAtlas value is published — declare "
                    "ScheduleGpuSkinningAndRetarget(frame) BEFORE the area shadow pass",
                    ctx.View);
            if (ctx.DrawStreamOrdering.IsValid())
                p.Read(ctx.DrawStreamOrdering, RenderGraph::RGBufferRead::Indirect);
            else if (ctx.HasPendingBucketerSlices)
                Logger::Log::Error(
                    "[RenderGraph] AddAreaShadowPassForView(view {}): buckets are registered but no "
                    "DrawStreamOrdering value is published — declare "
                    "ScheduleWorldBucketerDispatches(frame) BEFORE the area shadow pass",
                    ctx.View);

            RenderGraph::RGAttachmentOps dops{};
            dops.Load = RenderGraph::RGLoadOp::Clear;
            dops.Store = RenderGraph::RGStoreOp::Store;
            dops.Clear.Depth = 0.0f;
            p.AttachDepth(areaMap, dops, RenderGraph::RGDepthAccess::ReadWrite);
            // No PreventCulling: the world pass's sampled Read anchors it.
        },
        [rsPtr, params = std::move(params)](RenderGraph::RGContext& c) mutable
        { RecordDepthOnlyPass(c, std::move(params), rsPtr->MakeDepthDrawServices()); });
}

Rendering::RenderGraph::RGPass ShadowMapRenderFeature::DeclareSpotPass(
    Rendering::RenderGraph::RGFrame& frame, RenderServices& rs, const FeatureDeclareContext& ctx)
{
    namespace RenderGraph = Rendering::RenderGraph;
    const ViewId viewId = ctx.View;

    if (!ctx.SpotShadow)
        return {};
    const SpotShadowFrameInfo& spotShadow = *ctx.SpotShadow;

    const uint32_t res = std::max(1u, ctx.PunctualResolution);

    if (ctx.WorldAlreadyDeclared)
        Logger::Log::Error(
            "[RenderGraph] AddSpotShadowPassForView(view {}): the world pass already declared — a "
            "later spot pass derives WAR (the world samples frame N−1 shadows); declare "
            "producers BEFORE the world pass",
            viewId);

    Rendering::TextureDesc td{};
    td.width = res;
    td.height = res;
    td.depth = 1;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.format = static_cast<uint32_t>(Rendering::TextureFormat::D32_FLOAT);
    td.usage = static_cast<uint32_t>(Rendering::TextureUsage::DepthStencil)
             | static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
    td.sampleCount = 1;
    const std::string mapName =
        "SpotShadowMap.View" + std::to_string(static_cast<uint32_t>(viewId));
    td.debugName = mapName.c_str();
    const RenderGraph::RGTexture spotMap = frame.ImportPersistentTexture(mapName.c_str(), td);
    if (!spotMap.IsValid())
        return {};

    rs.GetSpotShadowSampler();

    auto data = frame.AllocUpload<SpotShadowDataGPU>();
    if (data.Valid())
    {
        SpotShadowDataGPU d{};
        std::memcpy(d.spotShadowVP, spotShadow.lightVP.Data(), sizeof(d.spotShadowVP));
        d.spotShadowParams[0] = 1.0f;
        d.spotShadowParams[1] = static_cast<float>(spotShadow.packedLightIndex);
        d.spotShadowParams[2] = 0.0005f;
        d.spotShadowParams[3] = 0.02f;
        d.spotShadowParams2[0] = static_cast<float>(res);
        d.spotShadowParams2[1] = spotShadow.nearPlane;
        d.spotShadowParams2[2] = spotShadow.farPlane;
        d.spotShadowParams2[3] = 0.0f;
        *data.Ptr = d;
    }
    rs.PublishSpotShadow(ctx.Seam, frame, viewId, spotMap, data);

    auto cam = frame.AllocUpload<CameraData>();
    if (cam.Valid())
    {
        CameraData shadowCam{};
        std::memcpy(shadowCam.view, spotShadow.lightView.Data(), 64);
        std::memcpy(shadowCam.proj, spotShadow.lightProj.Data(), 64);
        std::memcpy(shadowCam.viewProj, spotShadow.lightVP.Data(), 64);
        shadowCam.cameraPos[0] = spotShadow.position.x;
        shadowCam.cameraPos[1] = spotShadow.position.y;
        shadowCam.cameraPos[2] = spotShadow.position.z;
        shadowCam.cameraPos[3] = spotShadow.farPlane;
        *cam.Ptr = shadowCam;
    }

    DepthOnlyPassParamsRG params{};
    params.ViewId = viewId;
    params.PassType = DepthPassType::SpotShadow;
    params.CascadeIndex = kSpotShadowCullingIndex;
    params.DepthBiasEnable = true;
    params.RasterizationSamples = 1;
    params.ViewportWidth = res;
    params.ViewportHeight = res;
    params.Resources = rs.BuildShadowPassResources(ctx.Seam, frame, viewId, cam.Buffer, cam.Offset);

    const std::string passName =
        "SpotShadow[View#" + std::to_string(static_cast<uint32_t>(viewId)) + "]";

    RenderServices* rsPtr = &rs;
    return frame.AddPass(
        passName.c_str(), Rendering::PassPhase::kEarlySetup,
        [&ctx, spotMap](RenderGraph::RGPassBuilder& p)
        {
            // A2.4-P0-R: self-contained RecordDepthOnlyPass exec over a dedicated
            // depth target — worker-recordable like the cascades.
            p.RecordInSecondary();
            if (ctx.SkinPaletteAtlas.IsValid())
                p.Read(ctx.SkinPaletteAtlas);
            else if (ctx.HasPendingSkinnedInstances)
                Logger::Log::Error(
                    "[RenderGraph] AddSpotShadowPassForView(view {}): skinned instances pending but "
                    "no SkinPaletteAtlas value is published — declare "
                    "ScheduleGpuSkinningAndRetarget(frame) BEFORE the spot shadow pass",
                    ctx.View);
            if (ctx.DrawStreamOrdering.IsValid())
                p.Read(ctx.DrawStreamOrdering, RenderGraph::RGBufferRead::Indirect);
            else if (ctx.HasPendingBucketerSlices)
                Logger::Log::Error(
                    "[RenderGraph] AddSpotShadowPassForView(view {}): buckets are registered but no "
                    "DrawStreamOrdering value is published — declare "
                    "ScheduleWorldBucketerDispatches(frame) BEFORE the spot shadow pass",
                    ctx.View);

            RenderGraph::RGAttachmentOps dops{};
            dops.Load = RenderGraph::RGLoadOp::Clear;
            dops.Store = RenderGraph::RGStoreOp::Store;
            dops.Clear.Depth = 0.0f;
            p.AttachDepth(spotMap, dops, RenderGraph::RGDepthAccess::ReadWrite);
        },
        [rsPtr, params = std::move(params)](RenderGraph::RGContext& c) mutable
        { RecordDepthOnlyPass(c, std::move(params), rsPtr->MakeDepthDrawServices()); });
}

void ShadowMapRenderFeature::DeclarePointPasses(Rendering::RenderGraph::RGFrame& frame,
                                                     RenderServices& rs,
                                                     const FeatureDeclareContext& ctx)
{
    namespace RenderGraph = Rendering::RenderGraph;
    const ViewId viewId = ctx.View;

    if (ctx.PointShadows.empty())
        return;

    // The atlas is a single 2D-array: budget slots × 6 cube faces = layers, all
    // at the atlas tile resolution (the High tier). A lower-tier slot renders into
    // the top-left tileRes² sub-rect of its full-resolution layer (max-res-with-
    // waste, design §4.2 alt (b)); one face per layer keeps per-layer RG isolation.
    const uint32_t budget = std::clamp(ctx.PointShadowBudget, 1u, kMaxPointShadowSlots);
    const uint32_t atlasRes = kPointAtlasTileResolution;
    const uint32_t atlasLayers = PointShadowAtlasLayerCount(budget);

    // Per-view log-on-change: the admitted slot set (count + each slot's tier /
    // tile resolution / surviving face count). A stable assignment logs once; a
    // tier flip shows a line — the hysteresis is verified by the lack of per-frame
    // spam under a slow dolly.
    {
        uint64_t declKey = 0xcbf29ce484222325ull; // FNV-1a over the slot config
        auto mix = [&declKey](uint64_t v) { declKey = (declKey ^ v) * 0x100000001b3ull; };
        mix(ctx.PointShadows.size());
        for (const PointShadowFrameInfo& fi : ctx.PointShadows)
        {
            const uint32_t faceCount =
                static_cast<uint32_t>(std::popcount(static_cast<uint32_t>(fi.faceMask & 0x3Fu)));
            mix((static_cast<uint64_t>(fi.shadowSlot) << 40) |
                (static_cast<uint64_t>(fi.tileResolution) << 8) | faceCount);
        }
        uint64_t& last = m_PointShadowDeclareState[viewId];
        if (last != declKey)
        {
            last = declKey;
            const float allocVramMB = static_cast<float>(atlasRes) * static_cast<float>(atlasRes) *
                                      4.0f * static_cast<float>(atlasLayers) / (1024.0f * 1024.0f);
            Logger::Log::Info("[PointShadow] view {} atlas {}x{} x{} layers ({:.1f} MB), {} of {} "
                              "slots shadowed:",
                              static_cast<uint32_t>(viewId), atlasRes, atlasRes, atlasLayers,
                              allocVramMB, ctx.PointShadows.size(), budget);
            for (const PointShadowFrameInfo& fi : ctx.PointShadows)
            {
                const uint32_t faceCount =
                    static_cast<uint32_t>(std::popcount(static_cast<uint32_t>(fi.faceMask & 0x3Fu)));
                // Committed coverage tier (from the resolved tile size), plus the
                // authored source tier (0=Inherit => coverage-driven, else pinned).
                const char* tierName = fi.tileResolution >= 1024u  ? "High"
                                       : fi.tileResolution >= 512u ? "Medium"
                                                                   : "Low";
                Logger::Log::Info("[PointShadow]   slot {} {} tier {}x{} (authored {}) rendered {}/6 "
                                  "faces (mask 0x{:x})",
                                  fi.shadowSlot, tierName, fi.tileResolution, fi.tileResolution,
                                  fi.resolutionTier, faceCount,
                                  static_cast<uint32_t>(fi.faceMask & 0x3Fu));
            }
        }
    }

    if (ctx.WorldAlreadyDeclared)
        Logger::Log::Error(
            "[RenderGraph] AddPointShadowPassesForView(view {}): the world pass already declared — "
            "later point passes derive WAR (the world samples frame N−1 shadows); declare "
            "producers BEFORE the world pass",
            viewId);

    Rendering::TextureDesc td{};
    td.width = atlasRes;
    td.height = atlasRes;
    td.depth = 1;
    td.mipLevels = 1;
    td.arrayLayers = atlasLayers;
    td.format = static_cast<uint32_t>(Rendering::TextureFormat::D32_FLOAT);
    td.usage = static_cast<uint32_t>(Rendering::TextureUsage::DepthStencil)
             | static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
    td.sampleCount = 1;
    td.flags = Rendering::TextureCreateFlags::ForceArrayView;
    const std::string mapName =
        "PointShadowAtlas.View" + std::to_string(static_cast<uint32_t>(viewId));
    td.debugName = mapName.c_str();
    const RenderGraph::RGTexture atlas = frame.ImportPersistentTexture(mapName.c_str(), td);
    if (!atlas.IsValid())
        return;

    rs.GetPointShadowSampler();

    // Upload the std430 slot SSBO array (budget entries; gaps left by an eviction
    // are zeroed and never indexed — no light's shadowSlot points at them).
    // 256 alignment covers minStorageBufferOffsetAlignment on all targets (Intel
    // reports 64+; 16 would under-align) and matches the light buffer upload.
    const uint64_t slotBytes = static_cast<uint64_t>(budget) * sizeof(PointShadowSlotGPU);
    const auto slotAlloc = frame.AllocUpload(slotBytes, 256);
    if (slotAlloc.Ptr)
    {
        auto* slots = static_cast<PointShadowSlotGPU*>(slotAlloc.Ptr);
        std::memset(slots, 0, static_cast<size_t>(slotBytes));
        for (const PointShadowFrameInfo& fi : ctx.PointShadows)
        {
            if (fi.shadowSlot < 0 || static_cast<uint32_t>(fi.shadowSlot) >= budget)
                continue;
            const uint32_t slot = static_cast<uint32_t>(fi.shadowSlot);
            PointShadowSlotGPU& s = slots[slot];
            for (uint32_t face = 0; face < kPointShadowFaceCount; ++face)
                std::memcpy(s.pointShadowVP[face], fi.lightVP[face].Data(),
                            sizeof(s.pointShadowVP[face]));
            s.pointShadowParams[0] = 1.0f; // enabled
            s.pointShadowParams[1] = static_cast<float>(PointShadowSlotFaceLayer(slot, 0)); // baseLayer
            s.pointShadowParams[2] = 0.0005f;                // depth bias
            // Shared with the face-cull keep margin floor (kFaceCullMarginFloor >
            // kPointShadowNormalBias) so a normal-offset receiver never samples a
            // face S1 culled. Changing this bias moves that invariant — keep them tied.
            s.pointShadowParams[3] = kPointShadowNormalBias; // receiver normal bias
            s.pointShadowParams2[0] = static_cast<float>(fi.tileResolution);
            s.pointShadowParams2[1] = fi.nearPlane;
            s.pointShadowParams2[2] = fi.farPlane;
            s.pointShadowParams2[3] = PointShadowTileScale(fi.tileResolution); // tileRes/atlasRes
        }
    }
    rs.PublishPointShadow(ctx.Seam, frame, viewId, atlas, slotAlloc.Buffer, slotAlloc.Offset,
                          slotBytes);

    for (const PointShadowFrameInfo& fi : ctx.PointShadows)
    {
        // L1a: a cached (clean) light declares NO face passes; its persistent
        // atlas layers retain last render's depth and the SSBO slot above still
        // carries its VPs so receivers keep sampling. The GPU-cull scheduler
        // honours the same flag on the same idempotent assignment, so no
        // survivor-stat slice is scheduled for a skipped light either (M2a reads
        // "unknown", never a wrong zero).
        if (!fi.needsRender)
            continue;

        // Same guard as the SSBO write above: on the frame the budget shrinks
        // (config hot-reload), stale assignment slots >= the new budget would
        // attach atlas layers beyond the new budget*6 layer count.
        if (fi.shadowSlot < 0 || static_cast<uint32_t>(fi.shadowSlot) >= budget)
            continue;
        const uint32_t slot = static_cast<uint32_t>(fi.shadowSlot);
        const uint32_t tileRes = std::max(1u, fi.tileResolution);

        for (uint32_t face = 0; face < kPointShadowFaceCount; ++face)
        {
            // S1: a face the camera can never sample is not declared (no clear, no
            // depth pass). Its atlas layer keeps last frame's contents; the
            // one-face-per-fragment property guarantees no visible pixel reads it.
            if ((fi.faceMask & (1u << face)) == 0u)
                continue;

            // Unique (slot, face) atlas layer and GPU-cull / M2a-survivor key.
            const uint32_t globalSlice = PointShadowSlotFaceLayer(slot, face);

            auto cam = frame.AllocUpload<CameraData>();
            if (cam.Valid())
            {
                CameraData shadowCam{};
                std::memcpy(shadowCam.view, fi.lightView[face].Data(), 64);
                std::memcpy(shadowCam.proj, fi.lightProj.Data(), 64);
                std::memcpy(shadowCam.viewProj, fi.lightVP[face].Data(), 64);
                shadowCam.cameraPos[0] = fi.position.x;
                shadowCam.cameraPos[1] = fi.position.y;
                shadowCam.cameraPos[2] = fi.position.z;
                shadowCam.cameraPos[3] = fi.farPlane;
                *cam.Ptr = shadowCam;
            }

            DepthOnlyPassParamsRG params{};
            params.ViewId = viewId;
            params.PassType = DepthPassType::PointShadow;
            params.CascadeIndex = globalSlice; // (slot,face) survivor key input (see PointShadowCullingIndex)
            params.DepthBiasEnable = true;
            params.RasterizationSamples = 1;
            // Sub-rect at the layer origin: the negative-viewport Y-flip composes in
            // tile-local space (VulkanCommandList flips within [0, tileRes]).
            params.ViewportX = 0;
            params.ViewportY = 0;
            params.ViewportWidth = tileRes;
            params.ViewportHeight = tileRes;
            params.Resources =
                rs.BuildShadowPassResources(ctx.Seam, frame, viewId, cam.Buffer, cam.Offset);

            const std::string passName = "PointShadow[View#" +
                                         std::to_string(static_cast<uint32_t>(viewId)) + ".Slot" +
                                         std::to_string(slot) + ".Face" + std::to_string(face) + "]";

            RenderServices* rsPtr = &rs;
            frame.AddPass(
                passName.c_str(), Rendering::PassPhase::kEarlySetup,
                [&ctx, atlas, slot, face, globalSlice](RenderGraph::RGPassBuilder& p)
                {
                    // A2.4-P0-R: each point face renders into its own atlas layer
                    // with a self-contained exec (tile-local viewport + per-draw
                    // binds) — the point-shadow arc's M2 fork-join slice.
                    p.RecordInSecondary();
                    if (ctx.SkinPaletteAtlas.IsValid())
                        p.Read(ctx.SkinPaletteAtlas);
                    else if (ctx.HasPendingSkinnedInstances)
                        Logger::Log::Error(
                            "[RenderGraph] AddPointShadowPassesForView(view {}, slot {}, face {}): "
                            "skinned instances pending but no SkinPaletteAtlas value is published — "
                            "declare ScheduleGpuSkinningAndRetarget(frame) BEFORE the point shadow "
                            "passes",
                            ctx.View, slot, face);
                    if (ctx.DrawStreamOrdering.IsValid())
                        p.Read(ctx.DrawStreamOrdering, RenderGraph::RGBufferRead::Indirect);
                    else if (ctx.HasPendingBucketerSlices)
                        Logger::Log::Error(
                            "[RenderGraph] AddPointShadowPassesForView(view {}, slot {}, face {}): "
                            "buckets are registered but no DrawStreamOrdering value is published — "
                            "declare ScheduleWorldBucketerDispatches(frame) BEFORE the point shadow "
                            "passes",
                            ctx.View, slot, face);

                    RenderGraph::RGAttachmentOps dops{};
                    dops.Load = RenderGraph::RGLoadOp::Clear;
                    dops.Store = RenderGraph::RGStoreOp::Store;
                    dops.Clear.Depth = 0.0f;
                    p.AttachDepth(atlas, dops, RenderGraph::RGDepthAccess::ReadWrite,
                                  RenderGraph::RGRange{.BaseLayer = globalSlice, .LayerCount = 1});
                },
                [rsPtr, params = std::move(params)](RenderGraph::RGContext& c) mutable
                { RecordDepthOnlyPass(c, std::move(params), rsPtr->MakeDepthDrawServices()); });
        }
    }
}

void ShadowMapRenderFeature::LogCascadeCacheStatsPeriodic(Rendering::ViewId viewId, bool enabled,
                                                          uint64_t casterContentVersion)
{
    constexpr uint32_t kLogPeriodFrames = 300;
    uint32_t& counter = m_CascadeCacheLogCounter[viewId];
    if (++counter % kLogPeriodFrames != 0)
        return;

    // Deltas since the previous window. Consuming advances the cache-side
    // snapshot in the same step, so no return below can leave the next window
    // double-counting this one.
    const CascadeShadowCache::Stats window = m_CascadeShadowCache.ConsumeStatsWindow(viewId);
    if (window.Evaluated == 0)
        return;

    const uint64_t evalW = window.Evaluated;
    const uint64_t skipW = window.Skipped;
    auto causeW = [&window](CascadeCacheDirtyCause c)
    { return window.CauseCounts[static_cast<size_t>(c)]; };
    const uint64_t cachedW = causeW(CascadeCacheDirtyCause::Cached);

    // Zero hits over a full enabled window is only a STORM when the misses are
    // not plain camera motion: a sustained orbit legitimately re-renders every
    // frame (camera/fit/settle causes) and must log as Info, not Warning.
    // Everything else (epoch churn, under-draw loops, physical thrash) with
    // zero hits is the storm the tripwire exists for — and it warns on the
    // FIRST window of an episode only (edge-triggered), then drops to Info.
    const uint64_t missW = evalW - cachedW;
    const uint64_t motionW = causeW(CascadeCacheDirtyCause::CameraChanged) +
                             causeW(CascadeCacheDirtyCause::CascadeFitChanged) +
                             causeW(CascadeCacheDirtyCause::CullNotSettled);
    const bool expectedMotionWindow = missW > 0 && motionW * 100 >= missW * 95;
    const bool stormWindow =
        enabled && skipW == 0 && evalW >= kLogPeriodFrames && !expectedMotionWindow;
    const bool wasStormActive = m_CascadeShadowCache.ExchangeStormActive(viewId, stormWindow);
    const bool warnStorm = stormWindow && !wasStormActive;

    const std::string causes = FormatCascadeCacheCauseHistogram(window, casterContentVersion);
    if (warnStorm)
    {
        Logger::Log::Warning(
            "[CascadeCache] view {} enabled but ZERO skips over {} evaluations — invalidation "
            "storm ({})",
            static_cast<uint32_t>(viewId), evalW, causes);
        return;
    }

    // A window whose every evaluation was a cache hit reprints identical numbers
    // for as long as the scene sits still. The storm edge above is computed first
    // and unconditionally, so staying quiet here cannot swallow it.
    if (skipW == evalW)
        return;

    // Twin of the [IdleElision] cause windows and behind the same opt-in: same
    // periodic shape, same investigation-only audience. The window snapshot is
    // consumed unconditionally at the top, so gating only the emission here
    // leaves no baseline behind.
    if (!Rendering::IdleElisionLoggingEnabled())
        return;

    const CascadeMotionScheduler::Stats& motion =
        m_MotionScheduler.GetStats(static_cast<uint32_t>(viewId));
    Logger::Log::Info("[CascadeCache] view {} {} window: eval {} skip {} cached {} | miss causes: "
                      "{} | motion-cap {}: deferred {} forcedAge {} forcedBeyondCap {}",
                      static_cast<uint32_t>(viewId), enabled ? "ON" : "off(measuring)", evalW,
                      skipW, cachedW, causes, m_MotionCap, motion.Deferred, motion.ForcedByAge,
                      motion.ForcedBeyondCap);
}

} // namespace Engine::Renderer
} // namespace GameEngine
