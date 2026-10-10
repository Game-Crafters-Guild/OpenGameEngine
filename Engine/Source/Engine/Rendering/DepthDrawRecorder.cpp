// DepthDrawRecorder.cpp
// Implementation of the shared depth-draw executor (design
// s3-depth-executor-2026-07). Relocated verbatim from
// RenderServices::ExecuteDepthOnlyPass; every former RenderServices member
// access is requalified onto the injected DepthDrawServices bundle. Records the
// depth/shadow draws for one already-declared pass into the passed-in RGContext.
#include "Engine/Rendering/DepthDrawRecorder.h"
#include "Engine/Rendering/ParallaxReliefDepth.h"

#include "Engine/Rendering/CpuDrawStreamBuilder.h"
#include "Engine/Rendering/DrawCommandProducer.h"
#include "Engine/Rendering/DrawStreamLookupKey.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialBinder.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/PipelineVariantCache.h"
#include "Engine/Rendering/ViewRegistry.h"
#include "Engine/Rendering/WorldDrawBuilder.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUDrawStreamBuilder.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Geometry/VertexAttributeFlags.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <unordered_set>
#include <utility>

#include "RenderServicesDetail.h"

namespace GameEngine
{
namespace Engine::Renderer
{
using namespace ::GameEngine::Rendering;

namespace
{
// The stripped depth layout (StrippedDepthVertexFlags) without the skinning
// streams the entry has no buffer for (DepthHeadVertexFlags adds back what a head reads).
Rendering::VertexAttributeFlags EffectiveVertexFlagsForDepthPass(
    const Rendering::MeshGPUEntry&        entry,
    const Rendering::MeshGPUEntryBindings& bindings)
{
    Rendering::VertexAttributeFlags flags = StrippedDepthVertexFlags(entry.vertexFlags);
    if (!bindings.jointsVB.IsValid())
        flags &= ~Rendering::VertexAttributeFlags::HasJoints;
    if (!bindings.weightsVB.IsValid())
        flags &= ~Rendering::VertexAttributeFlags::HasWeights;
    if (!bindings.joints1VB.IsValid())
        flags &= ~Rendering::VertexAttributeFlags::HasJoints1;
    if (!bindings.weights1VB.IsValid())
        flags &= ~Rendering::VertexAttributeFlags::HasWeights1;
    return flags;
}

// A vertex stream a depth draw binds beyond Core and skinning, at its fixed slot
// (VertexLayoutBuilder.h).
struct OptionalStreamBind
{
    Rendering::BufferHandle Buffer{};
    uint32_t Slot = 0;
};

// The tangent, colour and UV1..7 streams `drawVertexFlags` carries: exactly the
// bindings the draw's pipeline declares beyond Core and skinning, so none is left
// empty (VUID-vkCmdDrawIndexed-04007) and none is bound unread.
struct OptionalStreamBinds
{
    std::array<OptionalStreamBind, 9> Items{};
    uint32_t Count = 0;

    std::span<const OptionalStreamBind> View() const { return {Items.data(), Count}; }
};

OptionalStreamBinds CollectOptionalStreamBinds(Rendering::VertexAttributeFlags drawVertexFlags,
                                               const Rendering::MeshGPUEntryBindings& bindings)
{
    OptionalStreamBinds binds;
    if (Rendering::HasFlag(drawVertexFlags, Rendering::VertexAttributeFlags::HasTangent))
        binds.Items[binds.Count++] = {bindings.tangentVB, 1u};
    if (Rendering::HasFlag(drawVertexFlags, Rendering::VertexAttributeFlags::HasColor))
        binds.Items[binds.Count++] = {bindings.colorVB, 2u};
    if (Rendering::HasFlag(drawVertexFlags, Rendering::VertexAttributeFlags::HasUV1))
        binds.Items[binds.Count++] = {bindings.uv1VB, 3u};
    for (uint32_t i = 0; i < bindings.extraUvVB.size(); ++i)
    {
        const auto uvFlag = static_cast<Rendering::VertexAttributeFlags>(
            static_cast<uint32_t>(Rendering::VertexAttributeFlags::HasUV2) << i);
        if (Rendering::HasFlag(drawVertexFlags, uvFlag))
            binds.Items[binds.Count++] = {bindings.extraUvVB[i], 8u + i};
    }
    return binds;
}

} // namespace

std::span<const DrawCommand> ContributorDepthCommands(const ViewRegistry& views, Rendering::ViewId viewId,
                                                      DepthPassType passType,
                                                      Rendering::GPUDrawStreamBuilder::SlicePhase phase)
{
    if (passType == DepthPassType::DeformationMotion || phase != Rendering::GPUDrawStreamBuilder::SlicePhase::A)
        return {};
    const auto* pv = views.FindPerView(viewId);
    if (!pv)
        return {};
    const auto& cmds = pv->DepthCommands[static_cast<size_t>(passType)];
    return {cmds.data(), cmds.size()};
}

std::span<const DrawCommand> NonOccludingPrepassHeads(const ViewRegistry& views, Rendering::ViewId viewId)
{
    const auto* pv = views.FindPerView(viewId);
    if (!pv)
        return {};
    return {pv->NonOccludingPrepassHeads.data(), pv->NonOccludingPrepassHeads.size()};
}

// Prepass shared-depth collapse (prepass-collapse-2026-07-24): eligible
// camera-prepass draws bind the shared position-only depth PSO instead of the
// per-material full-fat variant. The kill switch restores the pre-change
// selection exactly for triage; the draw set and ranges are identical either
// way.
bool PrepassSharedDepthEnabled()
{
    static const bool kEnabled = [] {
        const char* v = std::getenv("GE_PREPASS_SHARED_DEPTH");
        return !(v && v[0] == '0');
    }();
    return kEnabled;
}

bool ParallaxDepthOffsetEnabled()
{
    static const bool kEnabled = [] {
        const char* v = std::getenv("GE_PARALLAX_DEPTH_OFFSET");
        return !(v && v[0] == '0');
    }();
    return kEnabled;
}

void RecordDepthOnlyPass(Rendering::RenderGraph::RGContext& c,
                         DepthOnlyPassParamsRG params,
                         const DepthDrawServices& services)
{
    auto* cmd = c.Cmd;
    if (!cmd)
        return;

    if (params.ViewportWidth != 0 && params.ViewportHeight != 0)
    {
        cmd->SetViewport(static_cast<float>(params.ViewportX),
                         static_cast<float>(params.ViewportY),
                         static_cast<float>(params.ViewportWidth),
                         static_cast<float>(params.ViewportHeight));
        cmd->SetScissor(params.ViewportX, params.ViewportY,
                        params.ViewportWidth, params.ViewportHeight);
    }

    // The deforming-motion producer draws one subset of the same keys: the
    // materials whose vertex modifier the composed motion variant can evaluate
    // at two endpoints. Every other pass type draws the view's whole set.
    const bool motionPass = params.PassType == DepthPassType::DeformationMotion;
    // The non-occluding prepass draws its forward heads alone: the entity batches are in the camera
    // prepass already, so the key list stays empty and every entity path below is skipped.
    const bool nonOccludingPrepass = params.PassType == DepthPassType::Prepass &&
                                     params.Phase == Rendering::GPUDrawStreamBuilder::SlicePhase::A &&
                                     params.Heads == PrepassHeads::NonOccluding;
    std::span<const WorldDrawBuilder::BatchKey> entityKeys =
        nonOccludingPrepass ? std::span<const WorldDrawBuilder::BatchKey>{}
        : motionPass        ? services.DrawBuilder.GetDeformingBatchKeys(params.ViewId)
                            : services.DrawBuilder.GetBatchKeys(params.ViewId);

    // Depth-only passes need only the Instanced keyword (GPU-driven vertex
    // fetch); lighting keywords are fragment-stage only. Narrowed at EXEC,
    // after every declaration ran — the world arm refreshes the per-view map
    // during declaration, so this sees the current pipeline's keywords even
    // on pipeline-(re)load frames (old-arm parity).
    const auto* passView = services.Views.FindPerView(params.ViewId);
    const MaterialKeyword passKeywords =
        DepthPassKeywords(passView ? passView->WorldPassKeywords : std::nullopt);

    // Same passInstanceIndex encoding as the old arm: passType in the high 16
    // bits so the prepass doesn't collide with cascade 0 in MaterialBinder's
    // per-pass descriptor cache (the terrain-flickers-black bug class).
    const uint32_t passInstanceIndex =
        (static_cast<uint32_t>(params.PassType) << 16) |
        ((params.PassType == DepthPassType::ShadowCascade ||
          params.PassType == DepthPassType::PointShadow ||
          params.PassType == DepthPassType::TransmittanceCascade) ? params.CascadeIndex : 0u);
    // Contributor draws (CBT terrain in the camera prepass, terrain grass in the non-occluding
    // prepass, registered depth producers in the light-space passes) carry their own pinned
    // depth pipelines.
    const auto depthCommands =
        nonOccludingPrepass ? NonOccludingPrepassHeads(services.Views, params.ViewId)
                            : ContributorDepthCommands(services.Views, params.ViewId, params.PassType, params.Phase);
    // The camera prepass also draws the non-occluding heads when no non-occluding prepass was
    // declared for the view (a pipeline without DepthResolve, or a resolve that did not run).
    const auto fallbackHeads =
        (params.PassType == DepthPassType::Prepass && !nonOccludingPrepass &&
         params.Phase == Rendering::GPUDrawStreamBuilder::SlicePhase::A && passView != nullptr &&
         !passView->NonOccludingPrepassDeclared)
            ? NonOccludingPrepassHeads(services.Views, params.ViewId)
            : std::span<const DrawCommand>{};
    LogMeshPassDiagnostic(
        "depth", "pass-enter", params.ViewId,
        entityKeys.size(), depthCommands.size(),
        services.Streams ? services.Streams->GetRangeCount() : 0u);

    const bool runEntityDraws = !entityKeys.empty();
    const bool hasAnyDraws = runEntityDraws || !depthCommands.empty() || !fallbackHeads.empty();
    if (!hasAnyDraws)
    {
        LogMeshPassDiagnostic(
            "depth", "pass-no-draws", params.ViewId,
            entityKeys.size(), depthCommands.size(),
            services.Streams ? services.Streams->GetRangeCount() : 0u);
        return;
    }

    // Per-slice cull stream identity (constant across every key of this pass).
    // TransmittanceCascade reuses the SAME per-cascade cull streams as the
    // depth cascade (glass was culled into them already), so it shares the
    // cascade slot. kCascadeIndexNone == main-view / depth-prepass slice.
    uint8_t cascadeIdx = Rendering::GPUDrawStreamBuilder::kCascadeIndexNone;
    if (params.PassType == DepthPassType::ShadowCascade ||
        params.PassType == DepthPassType::TransmittanceCascade)
        cascadeIdx = static_cast<uint8_t>(params.CascadeIndex);
    else if (params.PassType == DepthPassType::AreaShadow)
        cascadeIdx = kAreaShadowCullingIndex;
    else if (params.PassType == DepthPassType::SpotShadow)
        cascadeIdx = kSpotShadowCullingIndex;
    else if (params.PassType == DepthPassType::PointShadow)
        cascadeIdx = PointShadowCullingIndex(params.CascadeIndex);

    // Exec→declare under-draw feedback (cascade static-shadow cache): every
    // silent skip path below that drops draws the always-render pipeline would
    // simply re-record next frame marks this slice, so the NEXT declare treats
    // the committed render as dirty instead of a settled skip baseline (the
    // async pipeline/material publish gates, missing mesh entries, the stale-
    // survivor walk-skip that already CLEARED the layer). The semantic filters
    // (alpha-blend / transmission-depth split, draw-range-invalid for a class
    // with no slice in this cascade) deliberately do NOT mark — they fire in
    // steady state and their output IS the correct content. Only the cascade
    // families mark; they are the only consumers.
    const bool trackUnderDraw =
        services.UnderDraw != nullptr &&
        (params.PassType == DepthPassType::ShadowCascade ||
         params.PassType == DepthPassType::TransmittanceCascade);
    auto markUnderDraw = [&]()
    {
        if (trackUnderDraw)
            services.UnderDraw->Mark(static_cast<uint32_t>(params.ViewId),
                                     static_cast<uint8_t>(params.PassType),
                                     params.CascadeIndex);
    };

    // M2a batch-walk early-out. Recording is O(view batch keys): every shadow
    // depth pass re-resolves PSOs, rebuilds the descriptor table (BeginPass), and
    // records one indirect draw per key regardless of how many instances the GPU
    // cull left alive for THIS face/cascade — the dominant CPU cost of a
    // near-empty point face (design §1.3: ~1.7 ms CPU / face, resolution-
    // independent). Skip the whole walk when a prior frame's survivor count for
    // this shadow slice was zero: an empty slice's correct output is the
    // cleared-far layer the pass's RGLoadOp::Clear already produced, so skipping
    // is output-identical in steady state. Guard rails: only the opaque shadow
    // families early-out — the main-view depth prepass is never empty, and
    // TransmittanceCascade is left out CONSERVATIVELY, not for a coverage gap: its
    // glass IS counted in the shared cascade slot's passedCasters, which sums
    // every instance that passed the cull (the opaque/glass split only happens
    // later at record via the transmission-depth-filter), so passedCasters==0
    // means the tint pass is empty too — TC could legally join this early-out
    // later. Only with no contributor depth commands (terrain/ocean casters aren't
    // in the survivor stat); never on the unknown sentinel (fail-safe: record).
    // The survivor count is a stall-free, fence-gated readback (no GPU->CPU sync
    // on the frame path) and can be up to frames-in-flight (3) stale, so a caster
    // crossing into an emptied face's light frustum shows its shadow up to a few
    // frames late, then self-heals. Force off with GE_SHADOW_WALK_EARLYOUT=0.
    static const bool kShadowWalkEarlyOut = [] {
        const char* v = std::getenv("GE_SHADOW_WALK_EARLYOUT");
        return !(v && v[0] == '0');
    }();
    const bool isOpaqueShadowSlice =
        params.PassType == DepthPassType::ShadowCascade ||
        params.PassType == DepthPassType::AreaShadow ||
        params.PassType == DepthPassType::SpotShadow ||
        params.PassType == DepthPassType::PointShadow;
    // The survivor stat is produced by the GPU scatter; the compat profile never
    // runs it, so an unguarded early-out would skip every shadow slice forever.
    if (kShadowWalkEarlyOut && !services.CpuDrawStream && isOpaqueShadowSlice
        && depthCommands.empty() && services.Streams)
    {
        const uint32_t survivors = services.Streams->PreviousFrameShadowSurvivors(
            static_cast<uint32_t>(params.ViewId), cascadeIdx, params.Phase);
        if (survivors == 0u)
        {
            LogMeshPassDiagnostic("depth", "walk-skip-empty-slice", params.ViewId,
                                  entityKeys.size(), depthCommands.size(),
                                  services.Streams->GetRangeCount());
            // The survivor readback is up to frames-in-flight stale — an object
            // that just entered this slice would be invisible here while the
            // pass already cleared the layer. Under-draw mark keeps the static
            // cache from settling on the cleared layer (a steadily-empty slice
            // simply re-renders every frame, exactly like always-render).
            markUnderDraw();
            return;
        }
    }

    Rendering::GPUScene* gpuScene = services.Scene;
    const auto& meshRegistry = services.Meshes;

    const uint64_t kInstGpuInstancesAddr =
        (services.Device && gpuScene && gpuScene->GetInstanceBuffer().IsValid())
            ? services.Device->GetBufferDeviceAddress(gpuScene->GetInstanceBuffer())
            : 0ull;

    // The resolved table arrives pre-built (Cam + contributor overrides were
    // patched at declaration) — no post-BeginPass upserts in this arm.
    auto& binder = services.Materials.Binder();
    [[maybe_unused]] const uint64_t uploadedCompatListBytes =
        UploadedCompatInstanceListBytes(params.Resources);
    auto pass = binder.BeginPass(*cmd, params.ViewId, c, passKeywords,
                                 std::move(params.Resources), passInstanceIndex);

    ComposedPCInstanced pcInst{};

    // GE_DRAW_ATTRIBUTION: indirect-count draws this pass issued (per-group
    // under consolidation).
    uint32_t attribDrawsIssued = 0;

    // Draw consolidation. Non-empty span = the scatter published grouped
    // ranges: recordEntityBatch looks up by pool group, and the WALKS below
    // dedup draws on the RESOLVED (lookup classKey, group) pair — one indirect
    // draw per range, whichever key reaches it first. Empty = per-bucket path,
    // no dedup (keys are already unique per range there).
    const std::span<const uint32_t> meshGroups = services.MeshPoolGroups;
    std::unordered_set<uint64_t> drawnStreamRanges;
    if (!meshGroups.empty())
        drawnStreamRanges.reserve(entityKeys.size());

    Rendering::BufferHandle boundVbSlot[6]{};
    Rendering::BufferHandle boundIb{};
    Rendering::IndexType    boundIbType = Rendering::IndexType::Uint16;
    bool                    boundIbInit = false;
    auto setVbSticky = [&](Rendering::BufferHandle want, uint32_t slot)
    {
        if (want.IsValid() && (slot >= 6 || boundVbSlot[slot] != want))
        {
            cmd->SetVertexBuffer(want, slot);
            if (slot < 6) boundVbSlot[slot] = want;
        }
    };
    auto setIbSticky = [&](Rendering::BufferHandle want, Rendering::IndexType type)
    {
        if (want.IsValid() && (!boundIbInit || boundIb != want || boundIbType != type))
        {
            cmd->SetIndexBuffer(want, type);
            boundIb     = want;
            boundIbType = type;
            boundIbInit = true;
        }
    };

    // `lookup` arrives resolved by the walk, so the walk-level range dedup and
    // the range lookup here key on the SAME axes by construction.
    auto recordEntityBatch = [&](const WorldDrawBuilder::BatchKey& key,
                                 const DrawStreamLookupKey& lookup)
    {
        const Material* material = key.material;
        if (!material)
        {
            LogMeshDrawSkipDiagnostic(
                "depth", "material-null", params.ViewId,
                key.materialIndex, key.meshIndex,
                services.Streams && services.Streams->GetOrCreateScatterPipeline().IsValid(),
                false, 0ull, kInstGpuInstancesAddr,
                services.Device ? services.Device->GetCapabilities().supportsBufferDeviceAddress : false,
                services.Streams ? services.Streams->GetRangeCount() : 0u);
            markUnderDraw(); // async material publish gap — heals next frames
            return;
        }
        const auto* entry = meshRegistry.Find(key.mesh);
        if (!entry)
        {
            LogMeshDrawSkipDiagnostic(
                "depth", "mesh-entry-missing", params.ViewId,
                key.materialIndex, key.meshIndex,
                services.Streams && services.Streams->GetOrCreateScatterPipeline().IsValid(),
                false, 0ull, kInstGpuInstancesAddr,
                services.Device ? services.Device->GetCapabilities().supportsBufferDeviceAddress : false,
                services.Streams ? services.Streams->GetRangeCount() : 0u);
            markUnderDraw(); // mesh upload in flight — heals next frames
            return;
        }
        Rendering::MeshGPUEntryBindings entryBindings{};
        const bool drawable = meshRegistry.TryGetDrawableBindings(*entry, entryBindings);
        if (!drawable || entry->indexCount == 0)
        {
            LogMeshDrawSkipDiagnostic(
                "depth", !drawable ? kMeshNotDrawableReason : "index-count-zero",
                params.ViewId, key.materialIndex, key.meshIndex,
                services.Streams && services.Streams->GetOrCreateScatterPipeline().IsValid(),
                false, 0ull, kInstGpuInstancesAddr,
                services.Device ? services.Device->GetCapabilities().supportsBufferDeviceAddress : false,
                services.Streams ? services.Streams->GetRangeCount() : 0u);
            markUnderDraw(); // buffer not resident yet — heals next frames
            return;
        }
        const DepthPassMaterialFilter filter = FilterDepthPassMaterial(
            params.PassType, material->GetAlphaMode(),
            Rendering::HasKeyword(material->GetVariantKey().materialKeywords,
                                  Rendering::MaterialKeyword::Transmission));
        if (filter != DepthPassMaterialFilter::Draws)
        {
            LogMeshDrawSkipDiagnostic(
                "depth",
                filter == DepthPassMaterialFilter::AlphaBlend ? "alpha-blend-filter"
                                                              : "transmission-depth-filter",
                params.ViewId, key.materialIndex, key.meshIndex,
                services.Streams && services.Streams->GetOrCreateScatterPipeline().IsValid(),
                false, 0ull, kInstGpuInstancesAddr,
                services.Device ? services.Device->GetCapabilities().supportsBufferDeviceAddress : false,
                services.Streams ? services.Streams->GetRangeCount() : 0u);
            return;
        }

        const auto depthVertexFlags = EffectiveVertexFlagsForDepthPass(*entry, entryBindings);
        // The prepass additionally requires bit-exact clip positions against the
        // color pass (a GreaterOrEqual-gated re-write of re-shade) — guaranteed by
        // the shared camera_relative.glsl position chain plus invariant
        // gl_Position on both shaders (prepass-collapse-2026-07-24). SKINNED_8
        // meshes keep the per-material variant in the prepass: the shared skinned
        // VS blends only 4 influences and would diverge from the color pass.
        const bool isSkinned8Depth =
            Rendering::HasFlag(depthVertexFlags,
                               Rendering::VertexAttributeFlags::HasJoints1)
            || Rendering::HasFlag(depthVertexFlags,
                                  Rendering::VertexAttributeFlags::HasWeights1);
        // shadow_depth_shared.vert is precompiled SPIR-V with GE_INSTANCED and
        // buffer_reference baked in — there is no compat variant of it, so under
        // the compat profile every depth draw must take the composed
        // per-material variant, which routes through instance_io.glsl.
        const bool sharedDepthOffered =
            !services.CpuDrawStream &&
            (params.PassType == DepthPassType::ShadowCascade ||
             params.PassType == DepthPassType::AreaShadow ||
             params.PassType == DepthPassType::SpotShadow ||
             params.PassType == DepthPassType::PointShadow ||
             (params.PassType == DepthPassType::Prepass && PrepassSharedDepthEnabled() &&
              !isSkinned8Depth));
        const bool vertexModified =
            Rendering::HasKeyword(material->GetVariantKey().materialKeywords,
                                  Rendering::MaterialKeyword::HasVertexMod)
            || Rendering::HasKeyword(material->GetVariantKey().materialKeywords,
                                     Rendering::MaterialKeyword::HasVertexOutputMod);
        const DepthSegmentPipelineChoice head = ChooseDepthHeadPipeline(
            passKeywords, params.PassType,
            material->GetAlphaMode() == MaterialAlphaMode::Mask, vertexModified,
            sharedDepthOffered, WritesReliefDepth(material->GetVariantKey().materialKeywords));
        const Rendering::VertexAttributeFlags drawVertexFlags = DepthHeadVertexFlags(
            depthVertexFlags,
            Rendering::BoundStreamVertexFlags(*entry, entryBindings, material->IgnoresVertexColor()),
            vertexModified,
            DepthPassReadsVertexColor(params.PassType, material->GetAlphaMode()),
            head.Keywords);
        const OptionalStreamBinds optionalStreams = CollectOptionalStreamBinds(drawVertexFlags, entryBindings);
        const Rendering::CullModeFlags depthCullMode = material->IsDoubleSided()
            ? Rendering::CullModeFlagBits::None
            : services.CullMode;
        const bool isSkinnedDepth = Rendering::HasFlag(
            depthVertexFlags,
            Rendering::VertexAttributeFlags::HasJoints
            | Rendering::VertexAttributeFlags::HasWeights);

        // Resolve the depth pipeline (shared or per-material variant) for one
        // winding and fade state. Even segments draw with the view base;
        // mirrored (parity-1) segments flip it. Compiled lazily per segment that
        // actually draws, so mirror-free scenes never compile the flipped
        // winding and a settled frame never compiles the dither variant.
        //
        // A crossfading TAIL always takes the per-material variant with
        // DepthOnlyFragment | LodCrossfade: the fragment stage runs the same
        // GE_LodCrossfadeKeep the colour pass runs, so the depth this writes is
        // exactly the coverage the colour pass will keep. The shared depth VS
        // carries no fade code and could not dither, which is why the tail is
        // excluded from it here rather than in ChooseDepthHeadPipeline.
        struct ResolvedDepthPipeline
        {
            PipelineHandle pipeline{};
            const Rendering::ShaderMeta* meta = nullptr;
            std::span<const Rendering::DescriptorSetLayoutId> setLayouts;
            // 0 = derive the count from the variant meta (a fragment stage
            // brings the material's own sets); otherwise the vertex-only
            // pipeline's own set count.
            uint32_t setCount = 1u;
        };
        auto resolveDepthPipeline = [&](Rendering::FrontFace winding,
                                        bool crossfading) -> ResolvedDepthPipeline
        {
            ResolvedDepthPipeline rd{};
            const DepthSegmentPipelineChoice choice = ChooseDepthSegmentPipeline(
                head.Keywords, head.UseSharedDepth, head.ComposesFragment, crossfading);
            if (choice.UseSharedDepth)
            {
                const auto sharedId = services.Materials.Variants().GetSharedDepthPipelineId(
                    depthVertexFlags, entry->topology, depthCullMode, params.DepthBiasEnable,
                    params.DepthClampEnable, winding);
                if (sharedId.IsValid())
                {
                    rd.pipeline   = c.GetOrCreatePipelineVariant(sharedId);
                    rd.meta       = services.Materials.Variants().GetSharedDepthShaderMeta(isSkinnedDepth);
                    rd.setLayouts = services.Materials.Variants().GetSharedDepthSetLayouts(isSkinnedDepth);
                    return rd;
                }
            }
            auto [p, e] = services.Materials.Variants().GetOrCompileDepthInstancedVariant(
                *material, drawVertexFlags, entry->topology, choice.Keywords,
                params.RasterizationSamples, params.DepthBiasEnable, params.DepthClampEnable,
                winding, c);
            rd.pipeline = p;
            if (e) { rd.meta = e->VariantMeta.get(); rd.setLayouts = e->SetLayouts; }
            // Depth pipelines that compose a fragment stage reflect the
            // material's own sets; the pure-depth ones declare the sets their
            // vertex stage reads (set 0 alone for every mesh material), and the
            // count keeps the binder off the rest.
            rd.setCount = choice.ComposesFragment
                              ? 0u
                              : std::max<uint32_t>(1u, e ? static_cast<uint32_t>(e->SetLayouts.size()) : 1u);
            return rd;
        };

        // Compatibility profile: replay the frame's CPU-built instance list as
        // ONE direct instanced draw per batch. Shadow families read the caster
        // set —
        // the GPU scatter applies that flag test per record, and without it a
        // non-casting instance would start casting here.
        if (services.CpuDrawStream)
        {
            const bool shadowFamily = params.PassType == DepthPassType::ShadowCascade
                                   || params.PassType == DepthPassType::AreaShadow
                                   || params.PassType == DepthPassType::SpotShadow
                                   || params.PassType == DepthPassType::PointShadow;
            const CpuDrawStreamBuilder::InstanceList instances =
                services.CpuDrawStream->GetInstances(
                    params.ViewId, key.materialIndex, key.meshIndex,
                    shadowFamily ? CpuDrawStreamBuilder::InstanceSet::ShadowCasters
                                 : CpuDrawStreamBuilder::InstanceSet::Camera);
            if (instances.Count == 0u)
                return;
            assert(uploadedCompatListBytes ==
                       services.CpuDrawStream->GetIndexList(params.ViewId).size_bytes() &&
                   "the view's instance list was rebuilt after its first pass declared");

            const ResolvedDepthPipeline rd = resolveDepthPipeline(services.FrontFace,
                                                                  /*crossfading=*/false);
            if (!rd.pipeline.IsValid())
            {
                LogMeshDrawSkipDiagnostic(
                    "depth", "depth-pipeline-invalid", params.ViewId,
                    key.materialIndex, key.meshIndex, false, false, 0ull,
                    kInstGpuInstancesAddr, false, 0u);
                markUnderDraw(); // async depth-variant publish gate — heals on publish
                return;
            }

            setVbSticky(entryBindings.coreVB,    0);
            for (const OptionalStreamBind& stream : optionalStreams.View())
                setVbSticky(stream.Buffer, stream.Slot);
            setVbSticky(entryBindings.jointsVB,  4);
            setVbSticky(entryBindings.weightsVB, 5);
            setVbSticky(entryBindings.joints1VB,  6);
            setVbSticky(entryBindings.weights1VB, 7);
            setIbSticky(entryBindings.indexBuffer, static_cast<IndexType>(entry->indexType));

            // The batch's offset into the view's CompatInstanceList rides the
            // push constant instance_io.glsl's compat branch adds
            // gl_InstanceIndex to; firstInstance stays 0 because it does not
            // reach gl_InstanceIndex through SPIRV-Cross/Metal.
            const uint32_t compatListFirst = instances.First;
            Engine::Renderer::DrawBindings compatBindings{};
            compatBindings.PushConstants = std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(&compatListFirst),
                sizeof(compatListFirst));

            const auto resolvedPipe = binder.BindMaterialForDraw(
                pass, *material, drawVertexFlags, compatBindings, rd.pipeline,
                rd.meta, rd.setCount, rd.setLayouts);
            if (!resolvedPipe.IsValid())
            {
                LogMeshDrawSkipDiagnostic(
                    "depth", "material-bind-invalid", params.ViewId,
                    key.materialIndex, key.meshIndex, false, false, 0ull,
                    kInstGpuInstancesAddr, false, 0u);
                markUnderDraw(); // bind-time publish gate — heals on publish
                return;
            }

            cmd->DrawIndexed(entry->indexCount, instances.Count, entry->firstIndex,
                             static_cast<int32_t>(entry->vertexOffset),
                             /*firstInstance=*/0u);
            attribDrawsIssued += 1u;
            PushMeshDrawIssuedDiagnostic("depth", params.ViewId, key.materialIndex,
                                         key.meshIndex, 1u, 0u);
            return;
        }

        if (!services.Streams || !services.Streams->GetOrCreateScatterPipeline().IsValid())
        {
            LogMeshDrawSkipDiagnostic(
                "depth", "drawstream-pipeline-invalid", params.ViewId,
                key.materialIndex, key.meshIndex,
                false, false, 0ull, kInstGpuInstancesAddr,
                services.Device ? services.Device->GetCapabilities().supportsBufferDeviceAddress : false,
                services.Streams ? services.Streams->GetRangeCount() : 0u);
            markUnderDraw(); // scatter pipeline still compiling — heals next frames
            return;
        }
        const Rendering::GPUDrawStreamBuilder::BatchDrawRange drawRange =
            services.Streams->FindBatchDrawRange(
                static_cast<uint32_t>(params.ViewId), cascadeIdx,
                lookup.classKey, lookup.meshKey, params.Phase);
        if (!drawRange.IsValid() || !services.Device)
        {
            LogMeshDrawSkipDiagnostic(
                "depth", !services.Device ? "device-null" : "draw-range-invalid", params.ViewId,
                key.materialIndex, key.meshIndex,
                true, drawRange.IsValid(), 0ull, kInstGpuInstancesAddr,
                services.Device ? services.Device->GetCapabilities().supportsBufferDeviceAddress : false,
                services.Streams->GetRangeCount());
            return;
        }
        // Shared indirection buffer: arena-global firstInstance → one BDA for
        // every batch, slice, and cascade.
        const uint64_t slotIndirectionBda =
            services.Streams->GetSharedIndirectionAddress();
        if (slotIndirectionBda == 0 || kInstGpuInstancesAddr == 0)
        {
            LogMeshDrawSkipDiagnostic(
                "depth", slotIndirectionBda == 0 ? "indirection-bda-zero" : "instance-bda-zero",
                params.ViewId, key.materialIndex, key.meshIndex,
                true, true, slotIndirectionBda, kInstGpuInstancesAddr,
                services.Device->GetCapabilities().supportsBufferDeviceAddress,
                services.Streams->GetRangeCount());
            return;
        }

        pcInst.indirectionAddr  = slotIndirectionBda;
        pcInst.gpuInstancesAddr = kInstGpuInstancesAddr;
        std::span<const std::byte> pcBytes(
            reinterpret_cast<const std::byte*>(&pcInst), sizeof(pcInst));

        Engine::Renderer::DrawBindings drawBindings{};
        drawBindings.PushConstants = pcBytes;

        setVbSticky(entryBindings.coreVB,    0);
        for (const OptionalStreamBind& stream : optionalStreams.View())
            setVbSticky(stream.Buffer, stream.Slot);
        setVbSticky(entryBindings.jointsVB,  4);
        setVbSticky(entryBindings.weightsVB, 5);
        setVbSticky(entryBindings.joints1VB,  6);
        setVbSticky(entryBindings.weights1VB, 7);
        setIbSticky(entryBindings.indexBuffer, static_cast<IndexType>(entry->indexType));

        // Draw one parity segment with the depth pipeline compiled for its
        // winding. The mesh registry's index buffer + push constants are shared;
        // only the pipeline (front face) and the indirect offsets differ.
        auto issueSegment = [&](Rendering::FrontFace winding, bool crossfading,
                                const Rendering::GPUDrawStreamBuilder::BatchDrawSegment& seg)
        {
            const size_t   cmdOffset    = seg.cmdByteOffset;
            const size_t   countOffset  = seg.countByteOffset;
            const uint32_t maxDrawCount = seg.maxDrawCount;
            const ResolvedDepthPipeline rd = resolveDepthPipeline(winding, crossfading);
            if (!rd.pipeline.IsValid())
            {
                // Cold dither variant: skip THIS TAIL for the frame and let the
                // async compile heal it. The fading instance is then absent from
                // prepass depth for those frames — exactly the pre-design
                // behaviour, and bounded by the compile — while the colour pass
                // still draws it, so nothing vanishes. Skipping is the only safe
                // option: drawing the tail undithered here would put both levels
                // solidly into depth and z-kill the colour pass's dither.
                LogMeshDrawSkipDiagnostic(
                    "depth", "depth-pipeline-invalid", params.ViewId,
                    key.materialIndex, key.meshIndex,
                    true, true, slotIndirectionBda, kInstGpuInstancesAddr,
                    services.Device->GetCapabilities().supportsBufferDeviceAddress,
                    services.Streams->GetRangeCount());
                markUnderDraw(); // async depth-variant publish gate — heals on publish
                return;
            }
            const auto resolvedPipe = binder.BindMaterialForDraw(
                pass, *material, drawVertexFlags, drawBindings, rd.pipeline,
                rd.meta, rd.setCount, rd.setLayouts);
            if (!resolvedPipe.IsValid())
            {
                LogMeshDrawSkipDiagnostic(
                    "depth", "material-bind-invalid", params.ViewId,
                    key.materialIndex, key.meshIndex,
                    true, true, slotIndirectionBda, kInstGpuInstancesAddr,
                    services.Device->GetCapabilities().supportsBufferDeviceAddress,
                    services.Streams->GetRangeCount());
                markUnderDraw(); // bind-time publish gate — heals on publish
                return;
            }
            PushMeshDrawIssuedDiagnostic(
                "depth", params.ViewId, key.materialIndex, key.meshIndex,
                maxDrawCount,
                services.Streams ? services.Streams->GetRangeCount() : 0u);
            cmd->DrawIndexedIndirectCount(
                drawRange.recordBuffer,
                drawRange.countBuffer,
                maxDrawCount, // exact per-batch bound (snapshot capacity)
                /*stride=*/5u * sizeof(uint32_t),
                cmdOffset,
                countOffset);
            ++attribDrawsIssued;
        };

        // Segments(): heads AND crossfade tails — the same set the colour pass
        // issues. A fading instance has no head record, so the tail is its only
        // presence; drawing it here through the shared dither is what puts its
        // depth in front of every pre-world consumer (GTAO, SDSM, cluster
        // bounds) while keeping the two levels on disjoint pixels, so neither
        // z-kills the other. Only a camera slice publishes a tail, so a
        // light-space pass (the shadow families, the glass tint cascade) yields
        // heads alone; the assert holds the draw side to
        // DepthPassDrawsCrossfadeTails, the rule the warm-up follows.
        const Rendering::FrontFace baseWinding = services.FrontFace;
        for (const auto& sd : drawRange.Segments())
        {
            assert(!sd.crossfading || DepthPassDrawsCrossfadeTails(params.PassType));
            issueSegment(sd.mirrored ? Rendering::FlipWinding(baseWinding) : baseWinding,
                         sd.crossfading, sd.segment);
        }
    };

    auto recordContributorDraw = [&](const DrawCommand& dc)
    {
        Rendering::PipelineHandle resolvedPipe{};
        if (dc.InternedPipeline.IsValid())
            resolvedPipe = c.GetOrCreatePipelineVariant(dc.InternedPipeline);
        if (!resolvedPipe.IsValid())
            return;

        setVbSticky(dc.Geometry.AltGeom.VB, 0);
        setIbSticky(dc.Geometry.AltGeom.IB, dc.Geometry.AltGeom.IndexType);

        Rendering::PipelineHandle bound{};
        if (dc.Material)
        {
            bound = binder.BindMaterialForDraw(
                pass, *dc.Material, dc.VertexFlags, dc.Bindings,
                resolvedPipe, dc.PipelineMeta, dc.PipelineSetCount, /*setLayouts=*/{});
        }
        else if (dc.PipelineMeta)
        {
            bound = binder.BindPipelineForDraw(
                pass, resolvedPipe, *dc.PipelineMeta, dc.Bindings,
                dc.PipelineSetCount, /*setLayouts=*/{});
        }
        if (!bound.IsValid())
            return;

        if (dc.UseIndirect && dc.IndirectCommandBuffer.IsValid())
        {
            cmd->DrawIndexedIndirectCount(
                dc.IndirectCommandBuffer,
                dc.IndirectCountBuffer,
                dc.IndirectMaxDrawCount,
                dc.IndirectStride,
                dc.IndirectCommandOffset,
                dc.IndirectCountOffset);
        }
        else if (dc.SubDraws.empty())
        {
            cmd->DrawIndexed(dc.IndexCount, dc.InstanceCount,
                              dc.FirstIndex, dc.VertexOffset, dc.FirstInstance);
        }
        else
        {
            for (const auto& sub : dc.SubDraws)
            {
                cmd->DrawIndexed(sub.IndexCount, sub.InstanceCount,
                                  sub.FirstIndex, sub.VertexOffset, sub.FirstInstance);
            }
        }
    };

    // Opaque shadow passes collapse shared-depth-eligible casters: every
    // eligible material of a (mesh, side) drew into ONE class-sentinel range on
    // the GPU, so the consumer issues ONE draw per distinct (meshIndex,
    // double-sided) pair with the shared-depth PSO for that cull mode (a
    // representative material binds it — the depth pipeline is material-
    // independent). Material-dependent casters (alpha-test / vertex-mod /
    // transmissive / blend) keep their per-(mat, mesh) draw. TransmittanceCascade
    // draws only glass (dependent), and the depth prepass / main view keep their
    // per-(colorClass, mesh) ranges in lockstep with the color pass — the
    // prepass substitutes only the PSO for eligible draws (ChooseDepthHeadPipeline),
    // never the range keying (prepass-collapse-2026-07-24 §3.3 covers why).
    // The collapse is a property of the scatter's class-sentinel ranges: one
    // range holds every eligible material of a (mesh, side). The compat CPU
    // runs are keyed per (materialIndex, meshIndex), so collapsing there would
    // silently drop every non-representative material's instances.
    const bool collapseEligibleCasters =
        !services.CpuDrawStream &&
       (params.PassType == DepthPassType::ShadowCascade ||
        params.PassType == DepthPassType::AreaShadow ||
        params.PassType == DepthPassType::SpotShadow ||
        params.PassType == DepthPassType::PointShadow);
    // The axes a key's range was published under for THIS pass. Shadow slices
    // (cascadeIdx != None) publish shared-depth-eligible casters under a
    // per-side class sentinel — every eligible material of a (mesh, side)
    // collapsed into one range — while material-dependent casters keep their
    // class-id range; the cascade=None main-view depth prepass SHARES the
    // color table and keys on colorClassId in lockstep with the color pass
    // (P2-b), so its depth class is never consulted. The mesh axis is the pool
    // group when consolidation is active, else the meshIndex; absent groups
    // (dead mesh rows / failed allocations) are skipped by the walks below —
    // their instances self-reject GPU-side. A live key over an absent row is a
    // guard-fail: the per-bucket path would land in a MARKING guard
    // (entry-missing / mesh-not-drawable), so the skip marks under-draw too
    // — a cached cascade must never settle on it (per-bucket parity).
    auto lookupOf = [&](const WorldDrawBuilder::BatchKey& key,
                        Rendering::MaterialDepthClass cls) -> DrawStreamLookupKey
    {
        return ResolveDrawStreamLookupKey(key, cascadeIdx, cls, meshGroups);
    };
    // Walk-level range dedup (consolidation only): several keys can resolve to
    // one (lookup classKey, group) range — the class sentinels collapse
    // materials, the group axis collapses meshes. First key to resolve a range
    // is its representative; consume-on-attempt (a representative skipped by a
    // guard inside recordEntityBatch drops the range for one frame — the same
    // self-healing semantics as an unpopulated range, and the same hoisted
    // position the color pass ships). Hoisting the dedup ABOVE the content
    // guards is exact for the semantic filters (alpha mode / transmission ride
    // the class signature, so every key of a class resolves them identically)
    // and representative-only for the async-content guards (mesh-entry /
    // index-buffer): those still mark under-draw when the representative
    // trips them, and dead rows are caught per-key by the absent-group mark
    // above — the non-representative walk skips cost no coverage the cache
    // could settle on.
    auto claimStreamRange = [&](const DrawStreamLookupKey& lookup) -> bool
    {
        if (meshGroups.empty())
            return true; // per-bucket path: keys are already unique per range
        const uint64_t drawKey = (static_cast<uint64_t>(lookup.classKey) << 24) | lookup.meshKey;
        return drawnStreamRanges.insert(drawKey).second;
    };

    if (runEntityDraws && !collapseEligibleCasters)
    {
        for (const auto& key : entityKeys)
        {
            // The prepass never remaps to sentinels (cascadeIdx None); the
            // TransmittanceCascade walk resolves the class like the cascades.
            const DrawStreamLookupKey lookup = lookupOf(
                key, cascadeIdx == Rendering::GPUDrawStreamBuilder::kCascadeIndexNone
                         ? Rendering::MaterialDepthClass::MaterialDependent
                         : services.Materials.GetMaterialDepthClass(key.materialIndex));
            if (!meshGroups.empty() && lookup.meshKey == Rendering::MeshPoolGroupPlan::kAbsentGroup)
            {
                markUnderDraw(); // absent row under a live key — heals next frames
                continue;
            }
            if (!claimStreamRange(lookup))
                continue;
            recordEntityBatch(key, lookup);
        }
    }
    else if (runEntityDraws)
    {
        std::unordered_set<uint64_t> drawnEligible; // (meshIndex << 1) | doubleSidedBit
        for (const auto& key : entityKeys)
        {
            const Rendering::MaterialDepthClass cls = services.Materials.GetMaterialDepthClass(key.materialIndex);
            const DrawStreamLookupKey lookup = lookupOf(key, cls);
            if (!meshGroups.empty() && lookup.meshKey == Rendering::MeshPoolGroupPlan::kAbsentGroup)
            {
                markUnderDraw(); // absent row under a live key — heals next frames
                continue;
            }
            if (cls == Rendering::MaterialDepthClass::MaterialDependent)
            {
                if (!claimStreamRange(lookup))
                    continue;
                recordEntityBatch(key, lookup);
                continue;
            }
            const uint64_t dsBit =
                (cls == Rendering::MaterialDepthClass::EligibleDoubleSided) ? 1u : 0u;
            const uint64_t dedupKey = (static_cast<uint64_t>(key.meshIndex) << 1) | dsBit;
            if (!drawnEligible.insert(dedupKey).second)
                continue; // this (mesh, side) class group already drew its sentinel range
            // Under consolidation the walk dedup additionally collapses
            // (sentinel, group) across meshes.
            if (!claimStreamRange(lookup))
                continue;
            recordEntityBatch(key, lookup);
        }
    }
    for (const auto& dc : depthCommands)
        recordContributorDraw(dc);
    for (const auto& dc : fallbackHeads)
        recordContributorDraw(dc);

    if (DrawAttributionEnabled())
    {
        const char* attribTag = "depth-prepass";
        switch (params.PassType)
        {
        case DepthPassType::ShadowCascade:        attribTag = "depth-cascade"; break;
        case DepthPassType::AreaShadow:           attribTag = "depth-area"; break;
        case DepthPassType::SpotShadow:           attribTag = "depth-spot"; break;
        case DepthPassType::PointShadow:          attribTag = "depth-point"; break;
        case DepthPassType::TransmittanceCascade: attribTag = "depth-tint"; break;
        case DepthPassType::DeformationMotion:    attribTag = "deformation-motion"; break;
        default: break;
        }
        LogDrawAttribution(attribTag, params.ViewId, entityKeys.size(), attribDrawsIssued,
                           pass.PipelineBinds, pass.DescriptorBinds, !meshGroups.empty(),
                           0u /* group census logged by the color pass */);
    }

    binder.EndPass(pass);
}

} // namespace Engine::Renderer
} // namespace GameEngine
