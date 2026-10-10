#include "Engine/Rendering/MaterialBinder.h"

#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Materials/ShaderMeta.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace GameEngine::Engine::Renderer
{
namespace
{

// Bindings this many and above get no warn-once diagnostic, because the
// warn-once bookkeeping is a bitmask of this width. The SKIP itself is never
// gated on it — a binding past the window still drops the draw, just quietly,
// so keep this at or above the highest binding any set declares (the world
// pass's DDGI keyword currently reaches b37, Includes/ddgi_probes.glsl).
constexpr uint32_t kMaxWarnTrackedBinding = 64;

// Fibonacci-hash a cache-key hash to a power-of-two L1 slot index (256 slots).
// Mirrors PipelineVariantCache.cpp / the device PipelineCache's L1IndexFor.
inline size_t PerPassL1Index(size_t keyHash) noexcept
{
    return static_cast<size_t>(
        (static_cast<uint64_t>(keyHash) * 0x9E3779B97F4A7C15ull) >> (64 - 8));
}

using ::GameEngine::Rendering::DescriptorBinding;
using ::GameEngine::Rendering::DescriptorBindingMeta;
using ::GameEngine::Rendering::DescriptorSetDesc;
using ::GameEngine::Rendering::DescriptorSetHandle;
using ::GameEngine::Rendering::DescriptorSetLayoutDesc;
using ::GameEngine::Rendering::DescriptorSetMeta;
using ::GameEngine::Rendering::DescriptorSetUpdate;
using ::GameEngine::Rendering::DescriptorType;

DescriptorType MapShaderMetaBindingType(uint32_t t)
{
    using namespace ::GameEngine::Rendering::ShaderMetaBindingType;
    switch (t)
    {
        case kUniformBuffer:        return DescriptorType::UniformBuffer;
        case kStorageBuffer:        return DescriptorType::StorageBuffer;
        case kSampler:              return DescriptorType::Sampler;
        case kSampledImage:         return DescriptorType::Texture;
        case kStorageImage:         return DescriptorType::StorageImage;
        case kCombinedImageSampler: return DescriptorType::CombinedImageSampler;
        case kAccelerationStructure: return DescriptorType::AccelerationStructure;
        default:                    return DescriptorType::UniformBuffer;
    }
}

// Locate the reflected set with index `setIdx`. Returns nullptr if missing.
const DescriptorSetMeta* FindSetMeta(
    const ::GameEngine::Rendering::ShaderMeta& meta, uint32_t setIdx)
{
    for (const auto& s : meta.Sets)
    {
        if (s.Set == setIdx)
            return &s;
    }
    return nullptr;
}

// Linear search over a DrawBindings buffer table by name.
const DrawBindings::BufferEntry* FindDrawBuffer(const DrawBindings& draw, StringId nameId)
{
    for (const auto& e : draw.Buffers)
    {
        if (e.Name == nameId)
            return &e;
    }
    return nullptr;
}

// Linear search over a DrawBindings texture table by name.
const DrawBindings::TextureEntry* FindDrawTexture(const DrawBindings& draw, StringId nameId)
{
    for (const auto& e : draw.Textures)
    {
        if (e.Name == nameId)
            return &e;
    }
    return nullptr;
}

// Linear search over the pass's resolved buffer table by name.
const ResolvedPassResources::BufferEntry* FindPassBuffer(
    const ResolvedPassResources& pr, StringId nameId)
{
    for (const auto& e : pr.Buffers)
    {
        if (e.Name == nameId)
            return &e;
    }
    return nullptr;
}

// Linear search over the pass's resolved texture table by name.
const ResolvedPassResources::TextureEntry* FindPassTexture(
    const ResolvedPassResources& pr, StringId nameId)
{
    for (const auto& e : pr.Textures)
    {
        if (e.Name == nameId)
            return &e;
    }
    return nullptr;
}

} // namespace

uint64_t MaterialBinder::NextPerPassEpoch() noexcept
{
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

MaterialBinder::MaterialBinder(RenderServices& services, ::GameEngine::Rendering::IDevice& device)
    : m_Services(services)
    , m_Device(device)
    , m_PerPassEpoch(NextPerPassEpoch())
{
}

MaterialBinder::~MaterialBinder() = default;

void MaterialBinder::OnBeginFrame()
{
    // Transient descriptor sets are reset by the device's per-frame pool at
    // frame boundary; clearing the cache releases the now-stale handles so
    // the next-frame lookups allocate fresh sets. Serial (frame boundary,
    // before any parallel record window opens — A2.4-D6). Move to a fresh epoch
    // so per-thread L1 slots holding last frame's (now-stale) handles are
    // retired on their next probe.
    m_PerPassCache.clear();
    m_PerPassEpoch.store(NextPerPassEpoch(), std::memory_order_release);
}

void MaterialBinder::AssertPerPassLockHeld() const noexcept
{
#if GE_DEBUG_INSTRUMENTATION
    assert(m_PerPassInsertLockHeldDebug
           && "MaterialBinder per-pass cold insert without holding "
              "m_PerPassCacheMutex exclusively (A2.4-D6).");
#endif
}

PassBindingContext MaterialBinder::BeginPass(
    ::GameEngine::Rendering::CommandList& cmd,
    ::GameEngine::Rendering::ViewId view,
    const ::GameEngine::Rendering::RenderGraph::RGContext& passCtx,
    ::GameEngine::Rendering::MaterialKeyword passKeywords,
    ResolvedPassResources resources,
    uint32_t passInstanceIndex)
{
    PassBindingContext pass{};
    pass.Cmd               = &cmd;
    pass.View              = view;
    pass.PassRG            = &passCtx;
    pass.PassKeywords      = passKeywords;
    pass.FrameSlot         = m_Device.GetFrameIndex();
    pass.Samples           = passCtx.GetCurrentSampleCount();
    pass.PassInstanceIndex = passInstanceIndex;
    pass.PassResources     = std::move(resources);
    return pass;
}

void MaterialBinder::EndPass(PassBindingContext& pass)
{
    (void)pass;
}

void MaterialBinder::InvalidateStickyBindsForPipeline(
    PassBindingContext& pass,
    std::span<const ::GameEngine::Rendering::DescriptorSetLayoutId> pipelineSetLayouts)
{
    using namespace ::GameEngine::Rendering;

    // Vulkan's pipeline-layout-compatibility rule: when a pipeline is bound
    // whose set-N layout differs from the previously bound pipeline's,
    // descriptor-set bindings at set N and all higher sets are disturbed.
    // Sets below the first incompatibility keep their bindings. Mirror that
    // on the sticky-bind tracking so the next set walk only rebinds what
    // Vulkan actually invalidated. Without precomputed layout ids we can't
    // tell — clear everything (conservative, matches pre-optimisation
    // behavior). All entity / variant-cached call sites pass precomputed
    // ids — the fallback exists for callers that don't.
    if (pipelineSetLayouts.empty())
    {
        pass.CurrentSets.fill({});
        pass.CurrentSetLayouts.fill(DescriptorSetLayoutId{});
        return;
    }

    uint32_t firstDiff = kMaxDescriptorSets;
    for (uint32_t i = 0; i < kMaxDescriptorSets; ++i)
    {
        const auto newLayout = (i < pipelineSetLayouts.size())
            ? pipelineSetLayouts[i]
            : DescriptorSetLayoutId{};
        if (newLayout != pass.CurrentSetLayouts[i])
        {
            firstDiff = i;
            break;
        }
    }
    // Clear sticky binds for the disturbed range and update layout tracking
    // for every slot so the NEXT pipeline change has correct comparison
    // material. Sets below `firstDiff` are untouched.
    for (uint32_t i = firstDiff; i < kMaxDescriptorSets; ++i)
    {
        pass.CurrentSets[i] = {};
        pass.CurrentSetLayouts[i] = (i < pipelineSetLayouts.size())
            ? pipelineSetLayouts[i]
            : DescriptorSetLayoutId{};
    }
}

bool MaterialBinder::IsBindlessTextureSet(const DescriptorSetMeta& setMeta)
{
    // The engine's bindless texture set is identified by binding 0 named
    // `ge_BindlessTextures`. Post image/sampler split it is a SAMPLED_IMAGE array
    // (binding 0) plus a SAMPLER array (binding 1, "ge_BindlessSamplers"); the
    // sampler binding rides along in the same global set, so binding the set once
    // covers both. Match on binding 0 by name (SPIR-V reflection collapses the
    // unbounded array to Count=1, so the count isn't reliable). Accept both the
    // split (Texture) and legacy (CombinedImageSampler) types so the classifier is
    // robust to either shader ABI.
    for (const auto& b : setMeta.Bindings)
    {
        if (b.Binding != 0 || b.Name != "ge_BindlessTextures")
            continue;
        const DescriptorType t = MapShaderMetaBindingType(b.Type);
        return t == DescriptorType::Texture || t == DescriptorType::CombinedImageSampler;
    }
    return false;
}

bool MaterialBinder::IsCompatMaterialTextureSet(const DescriptorSetMeta& setMeta)
{
    // The compatibility profile's per-material set, identified by binding 0
    // named `ge_MaterialTextures0` (bindless_textures.glsl under
    // GE_COMPAT_PROFILE). Its samplers ride bindings 64..71 of the same set, so
    // binding the set once covers both.
    for (const auto& b : setMeta.Bindings)
    {
        if (b.Binding != 0 || b.Name != "ge_MaterialTextures0")
            continue;
        const DescriptorType t = MapShaderMetaBindingType(b.Type);
        return t == DescriptorType::Texture || t == DescriptorType::CombinedImageSampler;
    }
    return false;
}

MaterialBinder::ResolvedSet MaterialBinder::ClassifySet(
    const DescriptorSetMeta& setMeta,
    const DrawBindings& draw) const
{
    ResolvedSet out{};
    out.Meta       = &setMeta;
    out.IsBindless = IsBindlessTextureSet(setMeta);
    out.Scope      = SetScope::PerPass;
    if (out.IsBindless)
        return out;

    // A set is per-draw if any binding resolves from DrawBindings; otherwise it
    // draws only from PassResources (Cam/Light/MaterialParams SSBO/shadow maps)
    // and is per-pass. Material params + textures are not a per-set source —
    // they live in the shared SSBO and the global bindless set.
    for (const auto& b : setMeta.Bindings)
    {
        const StringId nameId = b.ResolvedNameId();
        const DescriptorType type = MapShaderMetaBindingType(b.Type);

        if (type == DescriptorType::UniformBuffer || type == DescriptorType::StorageBuffer)
        {
            if (FindDrawBuffer(draw, nameId) != nullptr)
            {
                out.Scope = SetScope::PerDraw;
                return out;
            }
        }
        else if (type == DescriptorType::CombinedImageSampler || type == DescriptorType::Texture)
        {
            if (FindDrawTexture(draw, nameId) != nullptr)
            {
                out.Scope = SetScope::PerDraw;
                return out;
            }
        }
    }

    return out;
}

DescriptorSetHandle MaterialBinder::BuildSetForBinding(
    PassBindingContext& pass,
    const Material& material,
    uint32_t setIdx,
    const DrawBindings& draw)
{
    const auto& meta = material.GetShaderMeta();
    if (!meta)
        return {};
    const auto* setMeta = FindSetMeta(*meta, setIdx);
    if (!setMeta || setMeta->Bindings.empty())
        return {};
    return BuildSetForBindingFromMeta(pass, material, *setMeta, draw);
}

DescriptorSetHandle MaterialBinder::BuildSetForBindingFromMeta(
    PassBindingContext& pass,
    const Material& material,
    const ::GameEngine::Rendering::DescriptorSetMeta& setMeta,
    const DrawBindings& draw,
    bool* outUnresolvable)
{
    if (setMeta.Bindings.empty())
        return {};

    // Bindless short-circuit: return the engine's global bindless set without
    // any per-material allocation.
    if (IsBindlessTextureSet(setMeta))
    {
        if (m_Services.Textures().IsBindlessEnabled())
            return m_Services.Textures().BindlessTextureSet();
        // If bindless isn't enabled the engine should have compiled a
        // non-bindless variant; fall through and build per-material so the
        // descriptor layout still validates.
    }

    // Compat analogue: TextureService caches one set per material, keyed by
    // GUID and rebuilt whenever a slot rebinds.
    if (IsCompatMaterialTextureSet(setMeta))
    {
        if (auto set = m_Services.Textures().MaterialTextureSet(material); set.IsValid())
            return set;
    }

    const ResolvedSet rs = ClassifySet(setMeta, draw);

    // Per-pass: shared across every draw in the (view, keywords, layout,
    // instanceIndex) tuple. Cheapest path — built once and bound sticky.
    // PassInstanceIndex separates shadow cascades whose per-pass resources
    // (Cam UBO) differ per-cascade while the layout is identical.
    if (rs.Scope == SetScope::PerPass)
    {
        // P1: the layout hash is precomputed on the reflected meta
        // (FinalizeSetLayout, called from MergeStages + from_json). Falls
        // back to an on-the-fly hash for test setups that synthesize a
        // meta without calling FinalizeSetLayout — same value, just
        // computed lazily here instead of eagerly upstream.
        const uint64_t layoutHash = (setMeta.BuiltLayoutHash != 0)
            ? setMeta.BuiltLayoutHash
            : ComputeDescriptorSetLayoutHash(setMeta);
        const PerPassCacheKey key{
            static_cast<uint64_t>(pass.View),
            static_cast<uint64_t>(pass.PassKeywords),
            layoutHash,
            pass.PassInstanceIndex};

        const uint64_t epoch = m_PerPassEpoch.load(std::memory_order_acquire);

        // A2.4-D6: per-thread L1 (no lock) fronts m_PerPassCache; on a miss take a
        // shared_lock find. Up to N record workers share the map concurrently.
        // The array is thread_local and so outlives any one binder — the epoch is
        // what makes a slot this binder's and this frame's.
        thread_local std::array<PerPassL1Slot, kPerPassL1Slots> l1{};
        auto& slot = l1[PerPassL1Index(PerPassCacheKeyHash{}(key))];
        if (slot.Epoch == epoch && slot.Value.IsValid() && slot.Key == key)
            return slot.Value;
        {
            std::shared_lock lock(m_PerPassCacheMutex);
            if (auto it = m_PerPassCache.find(key); it != m_PerPassCache.end())
            {
                slot = {key, it->second, epoch};
                return it->second;
            }
        }

        // Build without the lock (per-draw descriptor allocation is thread-safe;
        // it draws from the device's own pool).
        bool unresolvable = false;
        auto handle = BuildSetInternal(pass, material, setMeta, SetScope::PerPass, draw, &unresolvable);
        // Never cache a set that tripped the unresolved-buffer guard: it would hand the
        // (invalid) draw-skip decision to later frames without re-running resolution.
        if (handle.IsValid() && !unresolvable)
        {
            std::unique_lock lock(m_PerPassCacheMutex);
#if GE_DEBUG_INSTRUMENTATION
            m_PerPassInsertLockHeldDebug = true;
#endif
            AssertPerPassLockHeld();
            // Double-check: another worker may have inserted the same (view,
            // keywords, layout, passInstance) set while we built without the lock.
            // Keep the first; ours is a redundant transient set the pool reclaims.
            auto [it, inserted] = m_PerPassCache.try_emplace(key, handle);
            handle = it->second;
            slot = {key, handle, epoch};
#if GE_DEBUG_INSTRUMENTATION
            m_PerPassInsertLockHeldDebug = false;
#endif
        }
        if (outUnresolvable && unresolvable)
            *outUnresolvable = true;
        return handle;
    }

    // Per-draw: always a fresh transient allocation, never cached. The
    // device's per-frame DB pool absorbs the allocation cost.
    return BuildSetInternal(pass, material, setMeta, SetScope::PerDraw, draw, outUnresolvable);
}

DescriptorSetHandle MaterialBinder::BuildSetInternal(
    PassBindingContext& pass,
    const Material& material,
    const DescriptorSetMeta& setMeta,
    SetScope scope,
    const DrawBindings& draw,
    bool* outUnresolvable)
{
    using namespace ::GameEngine::Rendering;

    auto layout = ::GameEngine::Rendering::MaterialBuilder::BuildSetLayout(setMeta);

    // Skip layouts that require UPDATE_AFTER_BIND (e.g. the bindless texture
    // layout). Those can only come from the persistent pool — the binder's
    // transient cache would trigger VUID-03044 on allocation. The bindless
    // short-circuit catches the engine's standard case; this is a defensive
    // guard for any future shader-declared UAB layout.
    const bool needsUpdateAfterBind = std::any_of(
        layout.bindings.begin(), layout.bindings.end(),
        [](const DescriptorBinding& b) {
            return (b.flags & kDescriptorBindingUpdateAfterBind) != 0;
        });
    if (needsUpdateAfterBind)
        return {};

    DescriptorSetDesc dsDesc{};
    dsDesc.layout    = std::move(layout);
    dsDesc.transient = true;
    dsDesc.debugName = "MaterialBinder.Set";
    auto handle = m_Device.CreateDescriptorSet(dsDesc);
    if (!handle.IsValid())
        return {};

    // Warn-once diagnostics are per-worker thread_local (A2.4-D6): operator[]
    // mutates on every lookup (default-constructs the slot), so a shared map is
    // racy under concurrent record. A genuinely-missing binding may then log
    // once per worker that hits it — harmless, and no lock / no shared state.
    thread_local std::unordered_map<WarnKey, uint64_t, WarnKeyHash> warnedBindings;
    uint64_t& warnedMask = warnedBindings[WarnKey{&material, setMeta.Set}];

    // Scratch buffer reused across draws on the same recording thread. Made
    // thread_local because parallel command recording calls this concurrently
    // from per-thread command lists.
    thread_local std::vector<DescriptorSetUpdate> updates;
    updates.clear();
    updates.reserve(setMeta.Bindings.size());

    auto materialSamplerCached = SamplerHandle{};
    auto getMaterialSampler = [&]() {
        if (!materialSamplerCached.IsValid())
            materialSamplerCached = m_Services.Textures().GetSampler(material.GetSamplerPreset());
        return materialSamplerCached;
    };

    for (const auto& b : setMeta.Bindings)
    {
        const StringId nameId = b.ResolvedNameId();
        const DescriptorType type = MapShaderMetaBindingType(b.Type);

        switch (type)
        {
            case DescriptorType::UniformBuffer:
            case DescriptorType::StorageBuffer:
            {
                // Resolve buffer by name across sources.
                if (const auto* e = FindDrawBuffer(draw, nameId))
                {
                    DescriptorSetUpdate u{};
                    u.binding = b.Binding;
                    u.type    = type;
                    u.buffers = {e->Buffer};
                    if (e->Offset != 0 || e->Range != 0)
                    {
                        u.bufferOffsets = {static_cast<size_t>(e->Offset)};
                        u.bufferRanges  = {static_cast<size_t>(e->Range)};
                    }
                    updates.push_back(std::move(u));
                    break;
                }
                if (const auto* e = FindPassBuffer(pass.PassResources, nameId);
                    e && e->Buffer.IsValid())
                {
                    DescriptorSetUpdate u{};
                    u.binding = b.Binding;
                    u.type    = type;
                    u.buffers = {e->Buffer};
                    if (e->Offset != 0 || e->Range != 0)
                    {
                        u.bufferOffsets = {static_cast<size_t>(e->Offset)};
                        u.bufferRanges  = {static_cast<size_t>(e->Range)};
                    }
                    updates.push_back(std::move(u));
                    break;
                }
                // Material params live in the shared MaterialParams SSBO (a
                // PassResource), so a buffer that resolves from neither
                // DrawBindings nor PassResources is genuinely unbound. Binding a draw
                // with an unbound uniform/storage buffer is fatal under descriptor
                // buffers (garbage device address on fetch), so flag the draw for skip
                // rather than issuing it — BindMaterialForDraw drops it. Error (not
                // Warning): a dropped draw + loud log beats a device lost. Throttled
                // per (material,set,binding) via warnedMask, same as before.
                if (outUnresolvable)
                    *outUnresolvable = true;
                if (b.Binding < kMaxWarnTrackedBinding && !(warnedMask & (1ull << b.Binding)))
                {
                    warnedMask |= (1ull << b.Binding);
                    Logger::Log::Error(
                        "MaterialBinder: set={} binding={} ('{}') buffer resolves from neither "
                        "DrawBindings nor PassResources — SKIPPING the draw (an unbound descriptor "
                        "is a garbage device address). Fix the binding's provider.",
                        setMeta.Set, b.Binding, b.Name);
                }
                break;
            }
            case DescriptorType::CombinedImageSampler:
            case DescriptorType::Texture:
            {
                // Per-draw wins.
                if (const auto* e = FindDrawTexture(draw, nameId);
                    e && e->Texture.IsValid())
                {
                    auto sampler = e->Sampler;
                    if (!sampler.IsValid())
                        sampler = getMaterialSampler();
                    if (type == DescriptorType::CombinedImageSampler && sampler.IsValid())
                    {
                        m_Device.UpdateCombinedImageSamplerBinding(
                            handle, b.Binding, e->Texture, sampler, e->ArrayIndex);
                    }
                    else
                    {
                        DescriptorSetUpdate u{};
                        u.binding      = b.Binding;
                        u.arrayElement = e->ArrayIndex;
                        u.type         = type;
                        u.textures     = {e->Texture};
                        if (sampler.IsValid())
                            u.samplers = {sampler};
                        updates.push_back(std::move(u));
                    }
                    break;
                }
                // Then per-pass (shadow map, MSM moments). Material textures are
                // not resolved here — they are sampled from the global bindless
                // texture array indexed by the SSBO's per-instance TextureIndices.
                if (const auto* e = FindPassTexture(pass.PassResources, nameId);
                    e && e->Texture.IsValid())
                {
                    auto sampler = e->Sampler;
                    if (!sampler.IsValid())
                        sampler = getMaterialSampler();
                    if (type == DescriptorType::CombinedImageSampler && sampler.IsValid())
                    {
                        m_Device.UpdateCombinedImageSamplerBinding(
                            handle, b.Binding, e->Texture, sampler);
                    }
                    else
                    {
                        // A separate sampled image (ibl.glsl's compat arm) takes a
                        // texture-only write: the combined update would also write
                        // a split sampler at binding+64 that the reflected layout
                        // never declared.
                        DescriptorSetUpdate u{};
                        u.binding  = b.Binding;
                        u.type     = type;
                        u.textures = {e->Texture};
                        updates.push_back(std::move(u));
                    }
                    break;
                }
                // Nothing provides this slot. Bind the slot's default rather than
                // leaving the descriptor alone: a partially-filled bind group is
                // fatal on backends that validate a bind group against its whole
                // layout (WebGPU rejects the CreateBindGroup outright, taking the
                // frame's command buffer with it). The default has the slot's own
                // shape (ResolveSlotDefault), which the same validation demands.
                const SlotDefault fallback = ResolveSlotDefault(b, getMaterialSampler());
                if (fallback.Texture.IsValid())
                {
                    const SamplerHandle sampler = fallback.Sampler;
                    if (type == DescriptorType::CombinedImageSampler && sampler.IsValid())
                    {
                        m_Device.UpdateCombinedImageSamplerBinding(handle, b.Binding, fallback.Texture, sampler);
                    }
                    else
                    {
                        DescriptorSetUpdate u{};
                        u.binding  = b.Binding;
                        u.type     = type;
                        u.textures = {fallback.Texture};
                        updates.push_back(std::move(u));
                    }
                }
                // No default has this slot's shape: the entry stays unwritten, which is an
                // invalid bind group on WebGPU and a garbage descriptor under descriptor
                // buffers, so the draw is skipped, as for an unbound buffer.
                else if (outUnresolvable)
                {
                    *outUnresolvable = true;
                }
                if (b.Binding < kMaxWarnTrackedBinding && !(warnedMask & (1ull << b.Binding)))
                {
                    warnedMask |= (1ull << b.Binding);
                    if (fallback.Texture.IsValid())
                        Logger::Log::Warning(
                            "MaterialBinder: set={} binding={} ('{}') texture not resolvable — bound the "
                            "slot default",
                            setMeta.Set, b.Binding, b.Name);
                    else
                        Logger::Log::Error(
                            "MaterialBinder: set={} binding={} ('{}') texture not resolvable and its shape "
                            "has no slot default — SKIPPING the draw. Provide the binding from DrawBindings "
                            "or the pass resources.",
                            setMeta.Set, b.Binding, b.Name);
                }
                break;
            }
            case DescriptorType::Sampler:
            {
                // Per-draw wins, exactly as the texture cases above. A draw that binds its own
                // split sampler (terrain's compat arm shares ONE REPEAT sampler across twelve
                // layer maps) carries it in DrawBindings; resolving only from pass resources fell
                // through to the material's sampler, whose wrap mode is not the one those maps
                // need — every layer tap then clamped to a single texel and the terrain rendered
                // one flat colour.
                if (const auto* e = FindDrawTexture(draw, nameId); e && e->Sampler.IsValid())
                {
                    m_Device.UpdateSamplerBinding(handle, b.Binding, e->Sampler);
                    break;
                }
                // Then pass resources by name; the entry's texture is carriage (TextureEntry has
                // no sampler-only shape). First user: ibl.glsl's compat arm, which splits the IBL
                // trio over one shared sampler to stay under WebGPU's per-stage sampler cap.
                if (const auto* e = FindPassTexture(pass.PassResources, nameId);
                    e && e->Sampler.IsValid())
                {
                    m_Device.UpdateSamplerBinding(handle, b.Binding, e->Sampler);
                    break;
                }
                // Same completeness rule as the texture case above: an unwritten
                // entry sinks the whole bind group, so fall back to the material's
                // own sampler. The throttle only gates the log.
                if (const auto sampler = getMaterialSampler(); sampler.IsValid())
                {
                    m_Device.UpdateSamplerBinding(handle, b.Binding, sampler);
                }
                if (b.Binding < kMaxWarnTrackedBinding && !(warnedMask & (1ull << b.Binding)))
                {
                    warnedMask |= (1ull << b.Binding);
                    Logger::Log::Warning(
                        "MaterialBinder: set={} binding={} ('{}') sampler not resolvable — bound the "
                        "material sampler",
                        setMeta.Set, b.Binding, b.Name);
                }
                break;
            }
            case DescriptorType::AccelerationStructure:
            {
                // Nothing on the world-pass path binds a TLAS today — DDGI
                // traces in compute and binds its own set through
                // NamedDescriptorWriter. Reaching here means a raster shader
                // reflected an accelerationStructureEXT with no provider, which
                // is a ray query against an unwritten descriptor slot: skip the
                // draw rather than issue it, same rule as an unbound buffer.
                if (outUnresolvable)
                    *outUnresolvable = true;
                if (b.Binding < kMaxWarnTrackedBinding && !(warnedMask & (1ull << b.Binding)))
                {
                    warnedMask |= (1ull << b.Binding);
                    Logger::Log::Error(
                        "MaterialBinder: set={} binding={} ('{}') is an acceleration structure, which "
                        "this path has no provider for — SKIPPING the draw. Bind it from the owning "
                        "feature's own descriptor set instead.",
                        setMeta.Set, b.Binding, b.Name);
                }
                break;
            }
            default:
                // Storage image and friends: add concrete cases as callers
                // need them. Keeps the binder honest about what it supports.
                break;
        }
    }

    if (!updates.empty())
        m_Device.UpdateDescriptorSetBatch(handle, updates);

    (void)scope;
    return handle;
}

MaterialBinder::SlotDefault MaterialBinder::ResolveSlotDefault(const DescriptorBindingMeta& binding,
                                                               SamplerHandle materialSampler)
{
    // A meta synthesized without image reflection names no shape: the per-name 2D default.
    if (!binding.Image)
        return {m_Services.Textures().ResolveDefaultTexture(binding.Name), materialSampler};
    const DescriptorBindingMeta::ImageInfo& image = *binding.Image;
    if (image.Dim != 2 || image.Multisample || image.UnsignedInteger)
        return {};
    if (image.Cube)
    {
        if (image.Depth || image.Arrayed)
            return {};
        return {m_Services.Textures().GetDefaultBlackCubeTexture(), materialSampler};
    }
    // Both depth defaults hold reverse-Z far (0.0), so a GreaterOrEqual comparison passes and a
    // shadow tap reads fully lit. The cascade fallback's several layers give it a 2D-array view;
    // the one-layer area fallback has a 2D view.
    if (image.Depth)
    {
        const TextureHandle depth = image.Arrayed ? m_Services.GetCascadeShadowFallbackTexture()
                                                  : m_Services.GetAreaShadowFallbackTexture();
        return {depth, m_Services.GetCascadeShadowSampler()};
    }
    if (image.Arrayed)
        return {m_Services.Textures().GetDefaultWhiteArrayTexture(), materialSampler};
    return {m_Services.Textures().ResolveDefaultTexture(binding.Name), materialSampler};
}

::GameEngine::Rendering::PipelineHandle MaterialBinder::BindMaterialForDraw(
    PassBindingContext& pass,
    const Material& material,
    ::GameEngine::Rendering::VertexAttributeFlags meshVertexFlags,
    const DrawBindings& draw,
    ::GameEngine::Rendering::PipelineHandle preResolvedPipeline,
    const ::GameEngine::Rendering::ShaderMeta* metaOverride,
    uint32_t pipelineSetCount,
    std::span<const ::GameEngine::Rendering::DescriptorSetLayoutId> pipelineSetLayouts)
{
    using namespace ::GameEngine::Rendering;

    if (!pass.Cmd)
    {
        assert(false && "MaterialBinder::BindMaterialForDraw requires a valid PassBindingContext");
        return {};
    }

    auto* cmd = pass.Cmd;

    // 1. Resolve pipeline variant — honour a caller-resolved handle when
    //    provided (instanced cache, depth-narrowed variants, etc.).
    PipelineHandle pipe = preResolvedPipeline;
    if (!pipe.IsValid())
    {
        const auto pipelineId = material.GetGraphicsPipelineIdForFlags(meshVertexFlags, &m_Device);
        pipe = pass.PassRG ? pass.PassRG->GetOrCreatePipelineVariant(pipelineId)
                           : PipelineHandle{};
    }
    if (!pipe.IsValid())
        return {};

    if (pipe != pass.CurrentPipeline)
    {
        cmd->SetPipeline(pipe);
        pass.CurrentPipeline = pipe;
        ++pass.PipelineBinds;
        InvalidateStickyBindsForPipeline(pass, pipelineSetLayouts);
    }

    // 2. Walk every reflected set. Build each via the unified name-driven
    //    resolver and skip BindDescriptorSet when the handle hasn't changed
    //    since the previous draw (pipeline-layout compatibility sticky bind).
    //
    //    Callers using narrowed pipelines (depth-only) pass a non-zero
    //    pipelineSetCount to bound the walk — the material's reflected meta
    //    carries fragment-stage sets the depth pipeline layout doesn't have.
    // Prefer the variant meta the caller supplied — it reflects the pipeline
    // we actually bound. The base material's stored ShaderMeta reflects only
    // the material's authored keywords; per-pass keywords (Instanced,
    // ForwardPlus, Shadows) introduce additional set-0 bindings that the
    // base meta doesn't see. Walking base meta against a variant pipeline
    // leaves the variant's extra slots unwritten — descriptor data reads as
    // zero/garbage and the surface renders black.
    const auto& fallbackMeta = material.GetShaderMeta();
    const ::GameEngine::Rendering::ShaderMeta* walkMeta =
        metaOverride ? metaOverride : fallbackMeta.get();
    if (walkMeta)
    {
        const uint32_t setBound =
            (pipelineSetCount > 0) ? pipelineSetCount : kMaxDescriptorSets;
        for (const auto& setMeta : walkMeta->Sets)
        {
            if (setMeta.Bindings.empty())
                continue;
            if (setMeta.Set >= kMaxDescriptorSets)
                continue;
            if (setMeta.Set >= setBound)
                continue;
            bool unresolvable = false;
            auto setHandle = BuildSetForBindingFromMeta(pass, material, setMeta, draw, &unresolvable);
            if (unresolvable)
                return {}; // skip the draw: a required set buffer had no provider (would be unbound)
            if (!setHandle.IsValid())
                continue;
            if (pass.CurrentSets[setMeta.Set] != setHandle)
            {
                cmd->BindDescriptorSet(setMeta.Set, setHandle, pipe);
                pass.CurrentSets[setMeta.Set] = setHandle;
                ++pass.DescriptorBinds;
            }
        }
    }

    // 3. Push constants. Layout matching is the caller's responsibility — the
    //    binder treats them as opaque bytes.
    if (!draw.PushConstants.empty())
    {
        cmd->SetConstants(0, 0,
                          draw.PushConstants.size(),
                          draw.PushConstants.data());
    }

    return pipe;
}

::GameEngine::Rendering::PipelineHandle MaterialBinder::BindPipelineForDraw(
    PassBindingContext& pass,
    ::GameEngine::Rendering::PipelineHandle pipeline,
    const ::GameEngine::Rendering::ShaderMeta& meta,
    const DrawBindings& draw,
    uint32_t pipelineSetCount,
    std::span<const ::GameEngine::Rendering::DescriptorSetLayoutId> pipelineSetLayouts)
{
    using namespace ::GameEngine::Rendering;

    if (!pass.Cmd)
    {
        assert(false && "MaterialBinder::BindPipelineForDraw requires a valid PassBindingContext");
        return {};
    }
    if (!pipeline.IsValid())
        return {};

    auto* cmd = pass.Cmd;
    if (pipeline != pass.CurrentPipeline)
    {
        cmd->SetPipeline(pipeline);
        pass.CurrentPipeline = pipeline;
        InvalidateStickyBindsForPipeline(pass, pipelineSetLayouts);
    }

    const uint32_t setBound =
        (pipelineSetCount > 0) ? pipelineSetCount : kMaxDescriptorSets;
    for (const auto& setMeta : meta.Sets)
    {
        if (setMeta.Bindings.empty())
            continue;
        if (setMeta.Set >= kMaxDescriptorSets)
            continue;
        if (setMeta.Set >= setBound)
            continue;

        // Bindless short-circuit: hand back the engine's global set without
        // any per-pass allocation. The materialless path mirrors what
        // BuildSetForBinding does for material-backed sets — except on the
        // compat profile, where there is no material to key a set by, so the
        // all-defaults set stands in.
        DescriptorSetHandle setHandle{};
        if (IsBindlessTextureSet(setMeta))
        {
            if (m_Services.Textures().IsBindlessEnabled())
                setHandle = m_Services.Textures().BindlessTextureSet();
        }
        else if (IsCompatMaterialTextureSet(setMeta))
        {
            setHandle = m_Services.Textures().DefaultMaterialTextureSet();
        }
        if (!setHandle.IsValid())
            setHandle = BuildPipelineSet(pass, setMeta, draw);
        if (!setHandle.IsValid())
            continue;
        if (pass.CurrentSets[setMeta.Set] != setHandle)
        {
            cmd->BindDescriptorSet(setMeta.Set, setHandle, pipeline);
            pass.CurrentSets[setMeta.Set] = setHandle;
        }
    }

    if (!draw.PushConstants.empty())
    {
        cmd->SetConstants(0, 0,
                          draw.PushConstants.size(),
                          draw.PushConstants.data());
    }

    return pipeline;
}

::GameEngine::Rendering::DescriptorSetHandle MaterialBinder::BuildPipelineSet(
    PassBindingContext& pass,
    const ::GameEngine::Rendering::DescriptorSetMeta& setMeta,
    const DrawBindings& draw)
{
    using namespace ::GameEngine::Rendering;

    auto layout = ::GameEngine::Rendering::MaterialBuilder::BuildSetLayout(setMeta);

    // UAB layouts can only come from the persistent pool. The bindless
    // short-circuit upstream catches the engine's canonical case; this is a
    // defensive guard mirroring BuildSetInternal.
    const bool needsUpdateAfterBind = std::any_of(
        layout.bindings.begin(), layout.bindings.end(),
        [](const DescriptorBinding& b) {
            return (b.flags & kDescriptorBindingUpdateAfterBind) != 0;
        });
    if (needsUpdateAfterBind)
        return {};

    DescriptorSetDesc dsDesc{};
    dsDesc.layout    = std::move(layout);
    dsDesc.transient = true;
    dsDesc.debugName = "MaterialBinder.PipelineSet";
    auto handle = m_Device.CreateDescriptorSet(dsDesc);
    if (!handle.IsValid())
        return {};

    // Scratch buffer reused across draws on the same recording thread. Made
    // thread_local because parallel command recording calls this concurrently
    // from per-thread command lists.
    thread_local std::vector<DescriptorSetUpdate> updates;
    updates.clear();
    updates.reserve(setMeta.Bindings.size());

    // Warn-once mask for unresolved bindings — keyed by (meta, set). The
    // materialless path has no Material* to key on, so it gets its own
    // bookkeeping. A contributor that forgets to populate a required
    // binding would otherwise produce a GPU-side garbage read with zero
    // CPU diagnostic. Per-worker thread_local (A2.4-D6): operator[] mutates on
    // every lookup, so a shared map is racy under concurrent record.
    thread_local std::unordered_map<const ::GameEngine::Rendering::DescriptorSetMeta*, uint64_t>
        warnedPipelineBindings;
    uint64_t& warnedMask = warnedPipelineBindings[&setMeta];
    auto warnUnresolved = [&](const ::GameEngine::Rendering::DescriptorBindingMeta& b,
                              const char* kind)
    {
        if (b.Binding < kMaxWarnTrackedBinding && !(warnedMask & (1ull << b.Binding)))
        {
            warnedMask |= (1ull << b.Binding);
            Logger::Log::Warning(
                "MaterialBinder.PipelineSet: set={} binding={} ('{}') {} not resolvable",
                setMeta.Set, b.Binding, b.Name, kind);
        }
    };

    for (const auto& b : setMeta.Bindings)
    {
        const StringId nameId = b.ResolvedNameId();
        const DescriptorType type = MapShaderMetaBindingType(b.Type);

        switch (type)
        {
            case DescriptorType::UniformBuffer:
            case DescriptorType::StorageBuffer:
            {
                if (const auto* e = FindDrawBuffer(draw, nameId);
                    e && e->Buffer.IsValid())
                {
                    DescriptorSetUpdate u{};
                    u.binding = b.Binding;
                    u.type    = type;
                    u.buffers = {e->Buffer};
                    if (e->Offset != 0 || e->Range != 0)
                    {
                        u.bufferOffsets = {static_cast<size_t>(e->Offset)};
                        u.bufferRanges  = {static_cast<size_t>(e->Range)};
                    }
                    updates.push_back(std::move(u));
                    break;
                }
                if (const auto* e = FindPassBuffer(pass.PassResources, nameId);
                    e && e->Buffer.IsValid())
                {
                    DescriptorSetUpdate u{};
                    u.binding = b.Binding;
                    u.type    = type;
                    u.buffers = {e->Buffer};
                    if (e->Offset != 0 || e->Range != 0)
                    {
                        u.bufferOffsets = {static_cast<size_t>(e->Offset)};
                        u.bufferRanges  = {static_cast<size_t>(e->Range)};
                    }
                    updates.push_back(std::move(u));
                    break;
                }
                warnUnresolved(b, "buffer");
                break;
            }
            case DescriptorType::CombinedImageSampler:
            case DescriptorType::Texture:
            {
                if (const auto* e = FindDrawTexture(draw, nameId);
                    e && e->Texture.IsValid() && e->Sampler.IsValid())
                {
                    m_Device.UpdateCombinedImageSamplerBinding(
                        handle, b.Binding, e->Texture, e->Sampler, e->ArrayIndex);
                    break;
                }
                if (const auto* e = FindPassTexture(pass.PassResources, nameId);
                    e && e->Texture.IsValid() && e->Sampler.IsValid())
                {
                    m_Device.UpdateCombinedImageSamplerBinding(
                        handle, b.Binding, e->Texture, e->Sampler);
                    break;
                }
                warnUnresolved(b, "texture");
                break;
            }
            case DescriptorType::AccelerationStructure:
                // See the same case in BindMaterialSet — no pipeline-set
                // provider exists, and silence here would leave a ray query
                // reading an unwritten descriptor slot.
                warnUnresolved(b, "acceleration structure");
                break;
            default:
                break;
        }
    }

    if (!updates.empty())
        m_Device.UpdateDescriptorSetBatch(handle, updates);

    return handle;
}

} // namespace GameEngine::Engine::Renderer
