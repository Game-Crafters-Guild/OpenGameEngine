#include "UI/UIManager.h"

#include "Logger/Logger.h"
#include "Platform/SystemMetrics.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineDescTranslator.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "UI/CaretBlink.h"
#include "UI/UIFrameBufferRing.h"
#include "UI/UIPrimitive.h"
#include "UI/UITextureRegistry.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;

// RenderGraph pass of the SDF UI renderer — the only render path since the
// retained twin was deleted. Declared fresh every frame: no retained pass
// state, no bindingsHash/targetHash recreate machinery. The viewport is the
// engine's one orientation — a positive-height rect, clip +Y on its top row —
// and ui_sdf.vert maps its top-left-origin pixels into that. SdfPushConstants
// block: the UI shader outputs LINEAR, the terminal encode owns the only OETF.
int UIManager::ResolveOutputEncoding(UI::UITargetSpace targetSpace)
{
    static_assert(UI::UITargetSpace::kKindCount == 5,
                  "A new UITargetSpace needs an outputEncoding arm here.");
    switch (targetSpace.GetKind())
    {
    case UI::UITargetSpace::Kind::EncodedSrgb:
        return 1;
    case UI::UITargetSpace::Kind::HdrPq:
        return 2;
    case UI::UITargetSpace::Kind::Hlg:
        return 3;
    case UI::UITargetSpace::Kind::ScRgb:
        return 4;
    case UI::UITargetSpace::Kind::LinearSdr:
    case UI::UITargetSpace::Kind::Count: // size sentinel; no factory produces it
        break;
    }
    return 0;
}

UIManager::TextCoverageConstants UIManager::ResolveTextCoverageConstants(
    float contrast, float blendGamma, UI::UITargetSpace targetSpace)
{
    static_assert(UI::UITargetSpace::kKindCount == 5,
                  "A new UITargetSpace needs a blend-space arm here: a target that "
                  "interpolates raw sRGB-encoded bytes must resolve BlendSpaceEncoded = 1 "
                  "(the shader's Skia-direction retarget) — and its pins in "
                  "TextCoverageCorrectionTests must be updated with it.");
    // EncodedSrgb is the one attachment that interpolates raw sRGB-encoded
    // bytes; it selects the shader's Skia-direction retarget arm. Every other
    // space — LinearSdr and the three HDR flavours — is a linear-light
    // attachment: the ROP interpolates linear values and the terminal encode
    // owns the OETF. blendGamma names the TARGET space in both arms, so it
    // passes through un-gated: at its 0 default the linear arm is the
    // identity retarget by mathematics, while the encoded arm selects the
    // full Skia-direction correction (area-exact ink out of an encoded
    // source-over — Chrome's SK_GAMMA_SRGB configuration).
    const int blendSpaceEncoded =
        targetSpace.GetKind() == UI::UITargetSpace::Kind::EncodedSrgb ? 1 : 0;
    return TextCoverageConstants{std::clamp(contrast, 0.0f, 1.0f),
                                 std::clamp(blendGamma, 0.0f, 4.0f), blendSpaceEncoded};
}

bool UIManager::RenderRG(RenderGraph::RGFrame& frame, RenderGraph::RGTexture target,
                         UI::UITargetSpace targetSpace)
{
    // Default arm: the UI owns the target and clears it (editor chrome path).
    return RenderRG(frame, target, targetSpace, RenderGraph::RGLoadOp::Clear);
}

bool UIManager::RenderRG(RenderGraph::RGFrame& frame, RenderGraph::RGTexture target,
                         UI::UITargetSpace targetSpace, RenderGraph::RGLoadOp loadOp)
{
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (!target.IsValid() || !m_Device)
        return false;

    // Per-frame prep (init, styles, primitives, ring uploads). The pass is
    // declared even on zero-draw or
    // unready (!fd.ready: no root / shaders missing) frames — the UI pass is
    // the target's CLEARER (it absorbed the old Editor.ClearPresentationTarget),
    // and a presented target must never hold unwritten memory. The exec body
    // keeps every draw early-out; the attachment ClearOps run regardless.
    const SdfFrameData fd = PrepareSdfFrameData();

    // Resolve this frame's deferred bindings against the per-frame publish
    // map. Only stamps matching THIS (frame, frameIndex) are consumable —
    // anything else is a stale publish from a previous frame/incarnation and
    // is skipped (the slot keeps its last physical; CreateTransientFrameSet's
    // dead-handle repair covers destruction). Old-graph rgRef bindings are
    // skipped entirely on RenderGraph frames: their producers force old-arm excursion
    // frames, so they are never sampled stale here.
    struct ResolvedBinding
    {
        uint32_t Slot = 0;
        RenderGraph::RGTexture Tex{};
    };
    std::vector<ResolvedBinding> resolved;
    resolved.reserve(m_SdfDeferredRGBindings.size());
    for (const auto& binding : m_SdfDeferredRGBindings)
    {
        if (binding.ExternalName.empty())
            continue; // old-arm-only binding (rgRef) — not consumable here
        const auto itPub = m_ExternalRGPublished.find(binding.ExternalName);
        if (itPub == m_ExternalRGPublished.end())
            continue;
        const PublishedRGTexture& pub = itPub->second;
        if (!pub.For.IsFor(frame) || pub.TexId == RenderGraph::kInvalidId)
            continue; // stale publish — never consume a foreign frame's id
        resolved.push_back({binding.Slot, RenderGraph::RGTexture{pub.TexId}});
    }

    const BufferHandle primBuf      = m_SdfPrimRing ? m_SdfPrimRing->GetBuffer() : BufferHandle{};
    const BufferHandle clipBuf      = m_SdfClipRing ? m_SdfClipRing->GetBuffer() : BufferHandle{};
    const BufferHandle drawOrderBuf = m_SdfDrawOrderRing ? m_SdfDrawOrderRing->GetBuffer() : BufferHandle{};

    const bool uploadsOk = fd.UploadsOk;

    // Output encoding resolves before pipeline selection: it feeds both the
    // push constants and the subpixel activation gate. Both derive from the
    // HOST-declared target space — never from the display (#784): an SDR
    // offscreen attachment stays unlifted (and subpixel-eligible) under an
    // HDR display, and an HDR-bound attachment takes its lift under an SDR
    // display.
    const int outputEncoding = ResolveOutputEncoding(targetSpace);

    // The encoded-blend target selects the UI_BLEND_SPACE_ENCODED shader
    // variants: paint adapters pass CSS bytes raw and encode sampled linear
    // sources, so the ROP interpolates encoded values (#767). A build staged
    // without the variant renders NOTHING into an encoded declaration (the
    // clear still runs — the pass owns the target) rather than silently
    // blending in the wrong space.
    const bool blendEncoded =
        targetSpace.GetKind() == UI::UITargetSpace::Kind::EncodedSrgb;
#if defined(GE_DEV_DIAG)
    if (blendEncoded && !m_SdfEncodedVariantAvailable)
    {
        static bool s_WarnedEncodedVariantMissing = false;
        if (!s_WarnedEncodedVariantMissing)
        {
            s_WarnedEncodedVariantMissing = true;
            Logger::Log::Warning(
                "[UI] target declared EncodedSrgb but the encoded shader variant is not "
                "staged — UI draws are skipped (clear-only frames)");
        }
    }
#endif

    // Subpixel RGB text AA swaps in the dual-source pipeline twin — the twin
    // of whichever blend-space variant the target selected. Every gate miss
    // (setting off, HDR-space target, no dualSrcBlend, no staged variant)
    // lands on the matching grayscale pipeline — same draws, never dropped
    // content and never a wrong-space pipeline.
    const bool subpixelActive = ResolveTextSubpixelActive(
        m_TextSubpixelAA, outputEncoding,
        m_Device->GetCapabilities().supportsDualSourceBlending,
        blendEncoded ? m_SdfSubpixelEncodedVariantAvailable : m_SdfSubpixelVariantAvailable);
    m_LastResolvedOutputEncoding = outputEncoding;
    m_LastTextSubpixelActive = subpixelActive;

    // Text-correction constants resolve at declare time with the rest of the
    // target-space translation: the attachment's blend space selects the
    // retarget arm, in this one place.
    const TextCoverageConstants textCoverage =
        ResolveTextCoverageConstants(m_TextContrast, m_TextBlendGamma, targetSpace);

    frame.AddPass(
        "UI Overlay", static_cast<int32_t>(PassPhase::kUI),
        [&](RenderGraph::RGPassBuilder& p)
        {
            // Load-op is caller-chosen. Clear when the UI owns/initializes the
            // target (backbuffer direct-attach, or the FinalLinear transient —
            // the editor chrome path; Clear is the import contract there). Load
            // when compositing the UI OVER existing content (a game HUD over the
            // scene's FinalColor). The clear color below is ignored under Load.
            RenderGraph::RGAttachmentOps ops{};
            ops.Load = loadOp;
            ops.Store = RenderGraph::RGStoreOp::Store;
            ops.Clear.Color[0] = 0.0f;
            ops.Clear.Color[1] = 0.0f;
            ops.Clear.Color[2] = 0.0f;
            ops.Clear.Color[3] = 1.0f;
            p.AttachColor(0, target, ops);

            // SSBO reads (ring buffers rotate per device frame; the import
            // dedups by physical so repeat declares are id-stable).
            if (primBuf.IsValid())
                p.Read(frame.ImportExternalBuffer("UISdfPrims", primBuf),
                       RenderGraph::RGBufferRead::Storage);
            if (clipBuf.IsValid())
                p.Read(frame.ImportExternalBuffer("UISdfClips", clipBuf),
                       RenderGraph::RGBufferRead::Storage);
            if (drawOrderBuf.IsValid())
                p.Read(frame.ImportExternalBuffer("UISdfDrawOrder", drawOrderBuf),
                       RenderGraph::RGBufferRead::Storage);

            // One sampled read per resolved deferred binding — this IS the
            // barrier that orders the producers (scene/game pipelines) before
            // the UI samples them; in the hybrid era that ordering came from
            // executing the RenderGraph frame before the old graph compiled.
            for (const ResolvedBinding& rb : resolved)
                p.Read(rb.Tex, RenderGraph::RGTextureRead::Sampled);
        },
        [this, resolved, primBuf, clipBuf, drawOrderBuf, uploadsOk, outputEncoding,
         subpixelActive, blendEncoded, textCoverage](RenderGraph::RGContext& ctx)
        {
            // Slot updates run even on draw-skipped frames: the published
            // producers rendered this frame, and the registry slots must
            // track the current physicals for the NEXT drawn frame.
            if (m_SdfTextureRegistry)
            {
                for (const ResolvedBinding& rb : resolved)
                {
                    const TextureHandle physical = ctx.GetTexture(rb.Tex);
                    if (physical.IsValid())
                        m_SdfTextureRegistry->UpdateSlot(rb.Slot, physical);
                }
            }
            const size_t drawCount = m_DrawOrder.size();
            if (!uploadsOk || drawCount == 0)
                return;
            const size_t totalSlots = m_PrimitiveAllocator.GetTotalSlots();
            if (totalSlots == 0)
                return;

            const uint32_t frameIdx = m_Device->GetFrameIndex();

            // The paper-white / black-lift constants stay device-derived: they
            // parameterize the lift's STRENGTH for a target whose declared
            // space enables it; the target space alone decides WHETHER the
            // lift runs.
            const auto hdrState = m_Device->GetHdrOutputState();

            // Encoded declaration without the staged variant: skip the draws
            // (the attachment clear above already ran). Falling back to the
            // linear pipeline here would be a wrong-SPACE render, which is
            // strictly worse than missing content.
            if (blendEncoded && !m_SdfEncodedVariantAvailable)
                return;

            Rendering::GraphicsPipelineId& pipelineId =
                blendEncoded
                    ? (subpixelActive ? m_SdfPipelineIdSubpixelEncoded : m_SdfPipelineIdEncoded)
                    : (subpixelActive ? m_SdfPipelineIdSubpixel : m_SdfPipelineId);
            if (!pipelineId.IsValid())
            {
                Rendering::PipelineDesc& pipelineDesc =
                    blendEncoded ? (subpixelActive ? m_SdfPipelineDescSubpixelEncoded
                                                   : m_SdfPipelineDescEncoded)
                                 : (subpixelActive ? m_SdfPipelineDescSubpixel : m_SdfPipelineDesc);
                pipelineDesc.debugName =
                    blendEncoded ? (subpixelActive ? "UI_SDF_Subpixel_Encoded" : "UI_SDF_Encoded")
                                 : (subpixelActive ? "UI_SDF_Subpixel" : "UI_SDF");
                pipelineId =
                    Rendering::PipelineDescTranslator::InternGraphics(*m_Device, pipelineDesc);
            }
            const PipelineHandle pipeline = ctx.GetOrCreatePipelineVariant(pipelineId);
            if (!pipeline.IsValid())
                return;
            ctx.Cmd->SetPipeline(pipeline);

            uint32_t w = m_LastLayoutWidth;
            uint32_t h = m_LastLayoutHeight;
            if (w == 0 || h == 0)
                m_Device->GetSwapchainSize(w, h);

            ctx.Cmd->SetViewport(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
            ctx.Cmd->SetScissor(0, 0, w, h);

            // Verbatim twin of the old arm's push-constant block — the
            // single-OETF invariant lives here (outputEncoding/paperWhite/
            // blackLift drive LINEAR-space compositing; no encode).
            SdfPushConstants pc{};
            pc.TargetSize[0] = static_cast<float>(w);
            pc.TargetSize[1] = static_cast<float>(h);
            pc.TimeSeconds = m_Time;
            // Read per frame rather than cached, so changing the OS caret
            // setting takes effect without restarting the editor. This is a
            // win32k transition, not a shared-memory read like GetTickCount —
            // roughly two orders of magnitude dearer — but at sub-microsecond
            // cost once per frame it sits far below a frame's noise floor.
            pc.CaretPhaseTogglesPerSecond =
                UI::CaretPhaseTogglesPerSecond(Platform::GetCaretBlinkHalfPeriod());
            pc.TextContrast = textCoverage.Contrast;
            pc.TextBlendGamma = textCoverage.BlendGamma;
            pc.TextBlendSpaceEncoded = textCoverage.BlendSpaceEncoded;
            pc.EdgeSoftness = std::max(0.01f, m_TextSmoothingGamma);
            pc.OutputEncoding = outputEncoding;
            const float deviceWhite =
                std::clamp(hdrState.staticMetadata.paperWhiteNits, kHdrPaperWhiteFloorNits, 1000.0f);
            pc.PaperWhiteNits = m_HdrUiPaperWhiteNits > 0.0f
                ? std::clamp(m_HdrUiPaperWhiteNits, 40.0f, 350.0f)
                : deviceWhite;
            pc.DeviceWhiteNits = deviceWhite;
            // Auto (< 0): in HDR modes, lift by the display's black floor so
            // dark chrome sits at the darkest level the panel can actually
            // show — effectively 0 on OLED, the backlight floor on LCD.
            pc.BlackLiftNits = m_HdrUiBlackLiftNits >= 0.0f
                ? m_HdrUiBlackLiftNits
                : (pc.OutputEncoding >= 2
                       ? std::clamp(hdrState.staticMetadata.minMasteringLuminance,
                                    kHdrUiBlackLiftMinNits, kHdrUiBlackLiftMaxNits)
                       : 0.0f);
            if (!primBuf.IsValid() || !drawOrderBuf.IsValid())
                return;

            DescriptorSetDesc dsDesc{};
            dsDesc.layout = m_SdfSsboSetLayout;
            dsDesc.debugName = "UISdf_SSBO_Set";
            dsDesc.transient = true;
            DescriptorSetHandle ssboSet = m_Device->CreateDescriptorSet(dsDesc);
            if (!ssboSet.IsValid())
                return;

            const size_t primBytes  = totalSlots * sizeof(UI::UIPrimitive);
            const size_t primOffset = m_SdfPrimRing->GetFrameOffset(frameIdx);
            m_Device->UpdateStorageBufferBinding(ssboSet, 0, primBuf, primOffset, primBytes);

            if (clipBuf.IsValid())
            {
                const size_t clipSlots = m_ClipAllocator.GetTotalSlots();
                const size_t clipBytes = std::max(clipSlots, size_t(1)) * sizeof(UI::UIClipRect);
                const size_t clipOffset = m_SdfClipRing->GetFrameOffset(frameIdx);
                m_Device->UpdateStorageBufferBinding(ssboSet, 1, clipBuf, clipOffset, clipBytes);
            }

            const size_t drawOrderBytes  = drawCount * sizeof(uint32_t);
            const size_t drawOrderOffset = m_SdfDrawOrderRing->GetFrameOffset(frameIdx);
            m_Device->UpdateStorageBufferBinding(ssboSet, 2, drawOrderBuf, drawOrderOffset,
                                                 drawOrderBytes);

            ctx.Cmd->BindDescriptorSet(0, ssboSet, pipeline);

            // Compat profile: one draw per run, each against the bind group
            // holding exactly that run's textures, with the run's first
            // draw-order index on a push constant. Painter's order is intact —
            // runs are consecutive spans of the same list, never a reordering.
            if (m_SdfCompatProfile)
            {
                if (!m_SdfTextureRegistry)
                    return;
                for (const UI::UICompatDrawRun& run : m_SdfCompatRuns.Runs())
                {
                    if (run.Count == 0)
                        continue;
                    const DescriptorSetHandle texSet = m_SdfTextureRegistry->CreateDrawRunSet(run);
                    if (!texSet.IsValid())
                        continue;
                    ctx.Cmd->BindDescriptorSet(1, texSet, pipeline);
                    pc.DrawBase = run.First;
                    ctx.Cmd->SetConstants(0, sizeof(pc), &pc);
                    ctx.Cmd->Draw(/*vertexCount=*/6u,
                                  /*instanceCount=*/run.Count,
                                  /*firstVertex=*/0u,
                                  /*firstInstance=*/0u);
                }
                return;
            }

            ctx.Cmd->SetConstants(0, sizeof(pc), &pc);

            if (m_SdfTextureRegistry)
            {
                DescriptorSetHandle texSet = m_SdfTextureRegistry->CreateTransientFrameSet();
                if (texSet.IsValid())
                    ctx.Cmd->BindDescriptorSet(1, texSet, pipeline);
            }

            ctx.Cmd->Draw(/*vertexCount=*/6u,
                          /*instanceCount=*/static_cast<uint32_t>(drawCount),
                          /*firstVertex=*/0u,
                          /*firstInstance=*/0u);
        });
    return true;
#else
    (void)frame;
    (void)target;
    (void)targetSpace;
    return false;
#endif
}
