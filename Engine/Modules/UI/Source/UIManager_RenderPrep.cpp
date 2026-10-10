#include "UI/UIManager.h"

#include "Logger/Logger.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineDescTranslator.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/ShaderProfileDefines.h"
#include "UI/UIFrameBufferRing.h"
#include "UI/UIPrimitive.h"
#include "UI/UITextureRegistry.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;

// Initial capacity for per-frame SSBO ring buffers (element count, not bytes).
// Sized to comfortably cover editor steady state (~50k primitive slots)
// without exercising the FrameBufferRing auto-grow path on startup.
//   primitives  : 262144 * 144B = 36 MB / frame
//   clips       :   1024 *  64B = 64 KB / frame
//   draw order  :  32768 *   4B = 128 KB / frame
// At 3 frames-in-flight per UIManager: ~108 MB primitives, plus clips/order.
// Elliptical corner radii cost the +16B/primitive and +16B/clip in those
// figures -- about +12 MB of persistent ring at this capacity. Capacities are
// element counts and every stride is sizeof-driven, so these numbers are
// documentation, not configuration.
static constexpr size_t kInitialPrimCapacity      = 262144;
static constexpr size_t kInitialClipCapacity      = 1024;
static constexpr size_t kInitialDrawOrderCapacity = 32768;

namespace
{
// The SDF stages are compiled by CMake rather than composed at runtime, so the
// compat profile is a separate .spv output per variant, not a keyword. One
// resolver keeps the suffix from being spelled in six places.
std::string SdfShaderPath(bool compat, const char* stem, const char* stage)
{
    std::string path = "Shaders/";
    path += stem;
    if (compat)
        path += "_compat";
    path += '.';
    path += stage;
    path += ".spv";
    return path;
}
} // namespace

void UIManager::CreateSdfRingBuffers()
{
    if (!m_Device)
        return;

    const uint32_t framesInFlight = std::max(1u, m_Device->GetFramesInFlight());

    // Create SSBO ring buffers for primitives, clips, and draw order.
    m_SdfPrimRing = std::make_unique<UI::FrameBufferRing<UI::UIPrimitive>>();
    m_SdfPrimRing->Create(m_Device, framesInFlight, kInitialPrimCapacity, "UISdfPrimitives");

    m_SdfClipRing = std::make_unique<UI::FrameBufferRing<UI::UIClipRect>>();
    m_SdfClipRing->Create(m_Device, framesInFlight, kInitialClipCapacity, "UISdfClipRects");

    m_SdfDrawOrderRing = std::make_unique<UI::FrameBufferRing<uint32_t>>();
    m_SdfDrawOrderRing->Create(m_Device, framesInFlight, kInitialDrawOrderCapacity, "UISdfDrawOrder");
}

void UIManager::InitSdfRenderer()
{
    if (!m_Device)
        return;

    CreateSdfRingBuffers();

    // Create texture registry.
    m_SdfTextureRegistry = std::make_unique<UI::UITextureRegistry>(m_Device);

    Logger::Log::Info("[UI] SDF renderer initialized (framesInFlight={})",
                      std::max(1u, m_Device->GetFramesInFlight()));
}

void UIManager::HealDeviceResourcesIfRebuilt()
{
    if (!m_Device)
        return;

    const uint64_t generation = m_Device->GetDeviceRebuildGeneration();
    const uint64_t previousGeneration = m_DeviceRebuildGeneration;
    if (generation == previousGeneration)
        return;

    // Counts are logged with the populations they came from: a UIManager that
    // had cached nothing at rebuild time drops zero of zero, which must stay
    // distinguishable from a heal that never ran at all.
    const size_t backgroundTextures = m_BgTextureCache.size();
    const size_t externalTextures = m_ExternalDeviceTextures.size();
    const size_t deferredBindings = m_SdfDeferredRGBindings.size();
    const size_t uploadsInFlight = m_BgUploadInFlight.size();
    const bool hadRings = m_SdfPrimRing != nullptr;

    // Drop, never destroy: the rebuild already destroyed every VkObject these
    // handles named. The background cache re-uploads from disk on the next
    // paint, which is the same path hot-reload eviction relies on.
    m_BgTextureCache.clear();

    // Externally published handles are borrowed, so UI does not free them — but
    // it must stop resolving them. ResolveExternalTexture gates only on
    // IsValid(), which a dead handle still passes, and hands the result to
    // UITextureRegistry::Register. Clearing is not a complete fix: publishers
    // re-publish the handle they still hold, which after a rebuild is itself
    // dead. What contains that is CreateTransientFrameSet's IsTextureHandleLive
    // substitution, which swaps any dead slot texture for the live dummy; the
    // publishers healing their own handles is tracked separately.
    m_ExternalDeviceTextures.clear();

    // A slot index in a deferred binding names the pre-rebuild slot space. The
    // next DFS releases every one of these back to the registry, which re-seeds
    // the slot and pushes it onto the free list — but UITextureRegistry's own
    // heal empties that list and resets the allocator to 1, so those releases
    // would hand the same index out twice. Clear rather than release: the slots
    // they name no longer exist.
    m_SdfDeferredRGBindings.clear();

    // In-flight upload markers survive the textures they were tracking. Left
    // set, EnsureBackgroundTextureUploaded early-returns on the dedupe check
    // forever. EvictBackgroundTexture drops these for the same reason. A
    // completion already marshalled when the heal runs still lands afterwards
    // and re-caches its dead handle for the session — contained by the
    // IsTextureHandleLive substitution, same shape as the publisher case above.
    m_BgUploadInFlight.clear();

    m_White1x1 = Rendering::INVALID_TEXTURE_HANDLE;
    m_SamplerRepeat = {};
    m_SamplerClamp = {};
    m_SamplerRepeatX = {};
    m_SamplerRepeatY = {};
    m_SamplerNearestClamp = {};

    m_DeviceRebuildGeneration = generation;
    CreateDeviceSamplersAndFallbackTexture();

    // The rings own device buffers; resetting runs their Destroy over dead
    // handles (a no-op) before fresh ones are created.
    if (hadRings)
    {
        m_SdfPrimRing.reset();
        m_SdfClipRing.reset();
        m_SdfDrawOrderRing.reset();
        CreateSdfRingBuffers();
    }

    // Dropping these does not itself recover anything: InternGraphics is keyed
    // on the description, so re-interning returns the same id. What recovers the
    // pipeline is the device-side self-heal — the concrete cache probes
    // IsPipelineAlive and recreates rather than serving a handle that outlived
    // its device (Device.cpp). These are cleared to stay consistent with
    // InvalidateRenderPassStateAfterTargetChange, which drops all four.
    m_SdfPipelineId = {};
    m_SdfPipelineIdSubpixel = {};
    m_SdfPipelineIdEncoded = {};
    m_SdfPipelineIdSubpixelEncoded = {};

    // Cached primitives store texture SLOT INDICES, and the heal just reset the
    // registry's slot space. Without this the editor sitting idle after a
    // rebuild — the exact post-rebuild window — never re-runs primitive
    // generation, so the transient set carries slot 0 alone while every cached
    // primitive still indexes 1..N. Same cause the texture-slot-availability
    // paths in UIManager_Assets.cpp use.
    MarkPrimitivesNeedRegen(0x200u);

    Logger::Log::Info(
        "[UI] UIManager: device rebuild {} -> {}; dropped {} background texture(s), {} external "
        "texture(s), {} deferred RG binding(s), {} in-flight upload(s), rings={}, "
        "re-provisioned white1x1={} samplers={}",
        previousGeneration,
        generation,
        backgroundTextures,
        externalTextures,
        deferredBindings,
        uploadsInFlight,
        hadRings ? "recreated" : "not-yet-created",
        m_White1x1 != Rendering::INVALID_TEXTURE_HANDLE,
        m_SamplerRepeat.IsValid() && m_SamplerClamp.IsValid() && m_SamplerRepeatX.IsValid()
            && m_SamplerRepeatY.IsValid() && m_SamplerNearestClamp.IsValid());
}

void UIManager::InvalidateRenderPassStateAfterTargetChange()
{
    // RenderGraph rebuilds the SDF pass every frame, so there is no persistent pass
    // state to reset here — only the cached SDF pipelines, which must be
    // dropped so they recompile against the new surface/HDR output format.
    m_SdfPipelineId = {};
    m_SdfPipelineIdSubpixel = {};
    m_SdfPipelineIdEncoded = {};
    m_SdfPipelineIdSubpixelEncoded = {};
}

void UIManager::EnsureSdfPipelineDesc()
{
    if (m_SdfPipelineReady)
        return;

    // The profile is a device property, resolved once at renderer init; reading
    // it here rather than plumbing it keeps the shader choice next to the
    // pipeline it configures, exactly as the material path does.
    m_SdfCompatProfile = Rendering::IsCompatShaderProfile();
    const bool compat = m_SdfCompatProfile;

    if (m_UiSdfVertShader.empty())
        m_UiSdfVertShader = Utils::LoadShaderFile(SdfShaderPath(compat, "ui_sdf", "vert").c_str());
    if (m_UiSdfFragShader.empty())
        m_UiSdfFragShader = Utils::LoadShaderFile(SdfShaderPath(compat, "ui_sdf", "frag").c_str());

    // Subpixel RGB AA twins are optional: a staged build without them (or a
    // load failure) must not take the UI down — the activation gate resolves
    // subpixel off and the grayscale pipeline keeps rendering.
    try
    {
        if (m_UiSdfVertShaderSubpixel.empty())
            m_UiSdfVertShaderSubpixel = Utils::LoadShaderFile(SdfShaderPath(compat, "ui_sdf_subpixel", "vert").c_str());
        if (m_UiSdfFragShaderSubpixel.empty())
            m_UiSdfFragShaderSubpixel = Utils::LoadShaderFile(SdfShaderPath(compat, "ui_sdf_subpixel", "frag").c_str());
    }
    catch (const std::exception& e)
    {
        Logger::Log::Warning(
            "[UI] Subpixel text AA shader variant unavailable ({}); the setting will resolve to grayscale.",
            e.what());
        m_UiSdfVertShaderSubpixel.clear();
        m_UiSdfFragShaderSubpixel.clear();
    }

    // Encoded-blend twins (fragment-only; the vertex stages are shared). A
    // build without them keeps every linear target rendering; an EncodedSrgb
    // declaration then draws nothing (clear-only) rather than borrowing a
    // linear-space pipeline — RenderRG owns that refusal.
    try
    {
        if (m_UiSdfFragShaderEncoded.empty())
            m_UiSdfFragShaderEncoded = Utils::LoadShaderFile(SdfShaderPath(compat, "ui_sdf_encoded", "frag").c_str());
        if (m_UiSdfFragShaderSubpixelEncoded.empty())
            m_UiSdfFragShaderSubpixelEncoded =
                Utils::LoadShaderFile(SdfShaderPath(compat, "ui_sdf_subpixel_encoded", "frag").c_str());
    }
    catch (const std::exception& e)
    {
        Logger::Log::Warning(
            "[UI] Encoded-blend shader variant unavailable ({}); EncodedSrgb targets will render "
            "clear-only frames.",
            e.what());
        m_UiSdfFragShaderEncoded.clear();
        m_UiSdfFragShaderSubpixelEncoded.clear();
    }

    // Descriptor set 0:
    //   binding 0 — UIPrimitive SSBO (slot-indexed, persistent + full upload)
    //   binding 1 — UIClipRect SSBO
    //   binding 2 — DrawOrder SSBO (Stage 2 part 3): vertex shader looks up
    //               its primitive slot via drawOrder[gl_InstanceIndex],
    //               so the persistent primitive buffer can hold elements
    //               in slot order (with holes) while still rendering in DFS.
    //
    // All three are `readonly buffer` in the GLSL, and the layout has to say so:
    // a WebGPU pipeline whose module declares var<storage, read> is rejected
    // against a read_write layout entry. Vulkan ignores the flag, and the
    // stage-visibility fallback that covers the other two cannot cover ClipRects
    // — it is fragment-only.
    constexpr uint32_t kReadOnly = kDescriptorBindingReadOnlyStorage;
    m_SdfSsboSetLayout = {};
    m_SdfSsboSetLayout.debugName = "UISdfSSBOSet";
    m_SdfSsboSetLayout.bindings.push_back(
        {0, DescriptorType::StorageBuffer, 1, kShaderStageVertex | kShaderStageFragment,
         "Primitives", kReadOnly});
    m_SdfSsboSetLayout.bindings.push_back(
        {1, DescriptorType::StorageBuffer, 1, kShaderStageFragment, "ClipRects", kReadOnly});
    m_SdfSsboSetLayout.bindings.push_back(
        {2, DescriptorType::StorageBuffer, 1, kShaderStageVertex, "DrawOrder", kReadOnly});

    // Descriptor set 1: Texture array (from UITextureRegistry)
    const auto& texLayout = m_SdfTextureRegistry
                                ? m_SdfTextureRegistry->GetLayoutDesc()
                                : DescriptorSetLayoutDesc{};

    // Pipeline descriptor
    PipelineDesc pd{};
    pd.type = PipelineType::Graphics;
    pd.topology = PrimitiveTopology::TriangleList;
    pd.EnableBlending(true, BlendFactor::One, BlendFactor::OneMinusSrcAlpha);
    pd.SetCullingMode(CullModeFlagBits::None);
    pd.colorAttachmentFormats.push_back(0u); // auto
    pd.pushConstantSize = static_cast<uint32_t>(sizeof(SdfPushConstants));
    pd.pushConstantStagesMask = kShaderStageVertex | kShaderStageFragment;
    // No vertex bindings — vertices are generated from gl_VertexIndex in the vertex shader.
    pd.descriptorSetLayouts.push_back(m_SdfSsboSetLayout);
    if (!texLayout.bindings.empty())
        pd.descriptorSetLayouts.push_back(texLayout);
    pd.vertexShader = m_UiSdfVertShader;
    pd.pixelShader = m_UiSdfFragShader;

    // Subpixel twin: identical layout, dual-source blend. src0 stays the
    // premultiplied colour; src1 carries per-channel alpha, so
    // ONE / ONE_MINUS_SRC1_COLOR is bit-identical to ONE / ONE_MINUS_SRC_ALPHA
    // wherever the shader writes src1 = vec4(alpha) — which every non-text
    // primitive does. Only Slug text diverges, per channel.
    m_SdfSubpixelVariantAvailable =
        !m_UiSdfVertShaderSubpixel.empty() && !m_UiSdfFragShaderSubpixel.empty();
    if (m_SdfSubpixelVariantAvailable)
    {
        PipelineDesc spd = pd;
        spd.vertexShader = m_UiSdfVertShaderSubpixel;
        spd.pixelShader = m_UiSdfFragShaderSubpixel;
        auto& attachment = spd.colorBlendState.attachments[0];
        attachment.dstColorBlendFactor = BlendFactor::OneMinusSrc1Color;
        attachment.dstAlphaBlendFactor = BlendFactor::OneMinusSrc1Alpha;
        m_SdfPipelineDescSubpixel = std::move(spd);
    }

    // Encoded twins: same descs with the encoded fragment stage swapped in.
    // Blend state is untouched — premultiplied source-over is the same
    // arithmetic whichever space the interpolated values live in; the
    // variant changes what the values ARE, not how the ROP combines them.
    m_SdfEncodedVariantAvailable = !m_UiSdfFragShaderEncoded.empty();
    if (m_SdfEncodedVariantAvailable)
    {
        PipelineDesc epd = pd;
        epd.pixelShader = m_UiSdfFragShaderEncoded;
        m_SdfPipelineDescEncoded = std::move(epd);
    }
    m_SdfSubpixelEncodedVariantAvailable =
        m_SdfSubpixelVariantAvailable && !m_UiSdfFragShaderSubpixelEncoded.empty();
    if (m_SdfSubpixelEncodedVariantAvailable)
    {
        PipelineDesc sepd = m_SdfPipelineDescSubpixel;
        sepd.pixelShader = m_UiSdfFragShaderSubpixelEncoded;
        m_SdfPipelineDescSubpixelEncoded = std::move(sepd);
    }

    m_SdfPipelineDesc = std::move(pd);
    m_SdfPipelineReady = true;

    Logger::Log::Info("[UI] SDF pipeline descriptor configured (subpixel variant {}, encoded "
                      "variant {}).",
                      m_SdfSubpixelVariantAvailable ? "available" : "unavailable",
                      m_SdfEncodedVariantAvailable ? "available" : "unavailable");
}

UIManager::SdfFrameData UIManager::PrepareSdfFrameData()
{
    SdfFrameData fd{};
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (!m_Device)
        return fd;

    if (!m_Root)
        return fd;

    // Ahead of the lazy init: after a device rebuild the cached handles are dead
    // but still IsValid(), so they must be dropped before anything registers or
    // binds them this frame.
    HealDeviceResourcesIfRebuilt();

    // Lazy-initialize the SDF renderer resources.
    if (!m_SdfPrimRing)
        InitSdfRenderer();

    EnsureSdfPipelineDesc();
    if (!m_SdfPipelineReady)
        return fd;

    // Ensure ResolvedStyle on every element is up-to-date before primitive
    // generation. Cheap tree walk copying POD fields.
    ResolveStyles();

    // Generate the persistent primitive buffer + draw-order list from the UI tree.
    GenerateAllPrimitives();

    // Protect the actual current draw, including retained primitives, before
    // reclaiming idle background images. Cache eviction never drops a texture
    // this frame is about to sample.
    EvictStaleBackgroundTextures();

    fd.Ready = true;

    // Stage 2 part 3: instance count is the size of m_DrawOrder, not the
    // primitive buffer (which holds slot-indexed data, including holes).
    fd.DrawCount = m_DrawOrder.size();
    if (fd.DrawCount == 0)
        return fd;

    // The vertex shader indexes m_PersistentPrimitives via slot indices read
    // from m_DrawOrder, so the upload must include every slot up to the
    // allocator's high-water mark (free-list holes are uninitialized but
    // never indexed by DrawOrder, so they don't render).
    fd.TotalSlots = m_PrimitiveAllocator.GetTotalSlots();
    if (fd.TotalSlots == 0 || m_PersistentPrimitives.size() < fd.TotalSlots)
        return fd;

    // Resolve target dimensions.
    fd.TargetW = m_LastLayoutWidth;
    fd.TargetH = m_LastLayoutHeight;
    if (fd.TargetW == 0 || fd.TargetH == 0)
        m_Device->GetSwapchainSize(fd.TargetW, fd.TargetH);
    if (fd.TargetW == 0 || fd.TargetH == 0)
        return fd;

    // Frame index for ring buffer rotation.
    const uint32_t frameIndex = m_Device->GetFrameIndex();

    // Upload primitive, clip, and draw-order data to GPU ring buffers.
    // Each ring keys UploadIfChanged on its own store version (bumped by
    // GenerateAllPrimitives for exactly the stores it mutated), so a clean
    // frame uploads zero bytes, a drain frame re-uploads only the stores it
    // touched — and the primitive ring further narrows to the dirty slot
    // span recorded by the drain (MarkRangeDirty), so a text/color change
    // uploads a few hundred bytes instead of the full snapshot.
    m_SdfPrimRing->BeginFrame(frameIndex);
    m_SdfClipRing->BeginFrame(frameIndex);
    m_SdfDrawOrderRing->BeginFrame(frameIndex);

    if (!m_SdfPrimRing->UploadIfChanged(frameIndex, m_PersistentPrimitives.data(), fd.TotalSlots,
                                        m_PrimitiveStoreVersion))
    {
        Logger::Log::Warning("[UI] SDF primitive ring upload failed (count={}).", fd.TotalSlots);
        return fd;
    }

    // Compat profile: the draw order is partitioned into per-texture runs and
    // each entry carries its run-local slot ordinal, so what reaches the GPU is
    // the partitioned word list. It depends on both source stores — the order
    // decides the spans, the primitives decide which texture each entry names —
    // so the ring's change gate keys on a version this pair drives.
    if (m_SdfCompatProfile)
    {
        // The length check is not redundant with the version pair: the words
        // are uploaded with fd.DrawCount as the count, so a Words() that does
        // not match the order it partitions would read past its own end. The
        // versions decide when to rebuild; this decides that we must.
        if (m_SdfCompatDrawOrderSource != m_DrawOrderStoreVersion ||
            m_SdfCompatPrimitiveSource != m_PrimitiveStoreVersion ||
            m_SdfCompatRuns.Words().size() != m_DrawOrder.size())
        {
            m_SdfCompatDrawOrderSource = m_DrawOrderStoreVersion;
            m_SdfCompatPrimitiveSource = m_PrimitiveStoreVersion;
            ++m_SdfCompatWordsVersion;
            m_SdfCompatRuns.Build(m_PersistentPrimitives, m_DrawOrder);
#if defined(GE_DEV_DIAG)
            // Slot pressure is the design question for this partition: every
            // extra run is another bind group and another draw. Logged on
            // change so the slot budget can be judged from a real frame.
            static size_t s_LastLoggedRunCount = 0;
            if (m_SdfCompatRuns.Runs().size() != s_LastLoggedRunCount)
            {
                s_LastLoggedRunCount = m_SdfCompatRuns.Runs().size();
                uint32_t peakTextures = 0;
                uint32_t peakGlyphs = 0;
                for (const UI::UICompatDrawRun& run : m_SdfCompatRuns.Runs())
                {
                    peakTextures = std::max(peakTextures, run.TextureCount);
                    peakGlyphs = std::max(peakGlyphs, run.GlyphCount);
                }
                Logger::Log::Info("[UI] compat draw partition: {} runs over {} primitives "
                                  "(peak {}/{} texture slots, {}/{} glyph slots)",
                                  s_LastLoggedRunCount, m_DrawOrder.size(), peakTextures,
                                  UI::kUiCompatTextureSlots, peakGlyphs, UI::kUiCompatGlyphSlots);
            }
#endif
        }
        if (!m_SdfDrawOrderRing->UploadIfChanged(frameIndex, m_SdfCompatRuns.Words().data(),
                                                 fd.DrawCount, m_SdfCompatWordsVersion))
        {
            Logger::Log::Warning("[UI] SDF draw-order ring upload failed (count={}).", fd.DrawCount);
            return fd;
        }
    }
    else if (!m_SdfDrawOrderRing->UploadIfChanged(frameIndex, m_DrawOrder.data(), fd.DrawCount,
                                                  m_DrawOrderStoreVersion))
    {
        Logger::Log::Warning("[UI] SDF draw-order ring upload failed (count={}).", fd.DrawCount);
        return fd;
    }

    // Stage 3: clips live in m_PersistentClipRects, slot-indexed via
    // m_ClipAllocator. Upload covers the allocator's high-water mark; the
    // shader looks up clips by index from a primitive's modeAndFlags so
    // any unused slots between live clips are simply never read.
    const size_t totalClipSlots = m_ClipAllocator.GetTotalSlots();
    if (totalClipSlots > 0)
    {
        if (m_PersistentClipRects.size() < totalClipSlots)
            m_PersistentClipRects.resize(totalClipSlots);
        if (!m_SdfClipRing->UploadIfChanged(frameIndex, m_PersistentClipRects.data(), totalClipSlots,
                                            m_ClipStoreVersion))
        {
            Logger::Log::Warning("[UI] SDF clip ring upload failed (count={}).", totalClipSlots);
            return fd;
        }
    }

    fd.UploadsOk = true;
#endif
    return fd;
}
