#include "UI/UITextureRegistry.h"

#include "Logger/Logger.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Materials/ShaderProfileDefines.h"
#include "Rendering/Text/FontAtlas.h"

#include <algorithm>
#include <cassert>
#include <unordered_set>

using namespace GameEngine::Rendering;
using namespace GameEngine::Rendering::Text;

namespace GameEngine
{
namespace UI
{
namespace
{
struct UiTextureLimits
{
    uint32_t colorTextures = UITextureRegistry::kDefaultMaxTextures;
    uint32_t bandTextures = UITextureRegistry::kDefaultMaxBandTextures;
};

UiTextureLimits ResolveUiTextureLimits(const RenderingDeviceCapabilities& caps)
{
    UiTextureLimits limits{};

    // The SDF pipeline has two sampled-image arrays in the fragment stage:
    // binding 0 for general UI/color/curve textures and binding 1 for Slug
    // band textures. Band pages are few, so reserve a small fixed slice instead
    // of halving MoltenVK's tight sampled-image budget.
    uint32_t imageBudget = UITextureRegistry::kDefaultMaxTextures
                         + UITextureRegistry::kDefaultMaxBandTextures;
    if (caps.maxPerStageSampledImages > 0)
        imageBudget = std::min(imageBudget, caps.maxPerStageSampledImages);

    static constexpr uint32_t kUiSdfNonImageFragmentBindings = 4;
    // Set 0 exposes two fragment SSBOs, while set 1 exposes two sampler objects.
    if (caps.maxPerStageResources > kUiSdfNonImageFragmentBindings)
        imageBudget = std::min(imageBudget, caps.maxPerStageResources - kUiSdfNonImageFragmentBindings);

    imageBudget = std::max(2u, imageBudget);

    const uint32_t bandReservation = std::min(
        UITextureRegistry::kDefaultMaxBandTextures,
        std::max(1u, imageBudget / 8u));
    limits.bandTextures = std::min(bandReservation, imageBudget - 1u);
    limits.colorTextures = std::min(
        UITextureRegistry::kDefaultMaxTextures,
        std::max(1u, imageBudget - limits.bandTextures));

    return limits;
}

// Create a 1x1 shader-resource texture seeded with a single texel.
// Backs the reserved slot-0 fallback of each bindless array: a real, live,
// correctly-typed image for slots that are unpopulated or whose texture has
// been retired.
TextureHandle CreateDummyTexture1x1(IDevice* device, TextureFormat format,
                                    const void* texel, uint32_t texelBytes,
                                    const char* debugName)
{
    TextureDesc td{};
    td.width = 1;
    td.height = 1;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.format = static_cast<uint32_t>(format);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource)
             | static_cast<uint32_t>(TextureUsage::TransferDst);
    td.debugName = debugName;
    TextureHandle tex = device->CreateTexture(td);
    if (!tex.IsValid())
        return {};

    BufferDesc st{};
    st.size = texelBytes;
    st.usage = static_cast<uint32_t>(BufferUsage::TransferSrc);
    st.memoryUsage = BufferMemoryUsage::Upload;
    st.debugName = "UISdf_DummyStaging";
    BufferHandle staging = device->CreateBuffer(st);
    device->UpdateBuffer(staging, 0, texelBytes, texel);

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(
        tex, ResourceState::Undefined, ResourceState::CopyDest));
    cl->CopyBufferToTextureSubresource(staging, tex, 0, 0, 1, 1, 0, texelBytes);
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(
        tex, ResourceState::CopyDest, ResourceState::ShaderResource));
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    device->ExecuteCommandLists(lists);
    device->DestroyBuffer(staging);
    return tex;
}
}

// Bindless layout (set 1): two runtime-sized sampled-image arrays plus two
// fixed samplers, so the layout is not constrained by
// maxPerStageDescriptorSamplers. PARTIALLY_BOUND lets unused image slots stay
// untouched; CreateTransientFrameSet writes a fresh set each draw, so
// cross-frame writes never happen.
void UITextureRegistry::BuildBindlessLayoutDesc(const RenderingDeviceCapabilities& caps)
{
    const UiTextureLimits limits = ResolveUiTextureLimits(caps);
    m_MaxTextures = limits.colorTextures;
    m_MaxBandTextures = limits.bandTextures;

    // PARTIALLY_BOUND permits leaving image slots unwritten, but it does not
    // make reading them safe: the transient descriptor memory behind this set is
    // bump-allocated and never zeroed, so an unwritten slot holds a recycled
    // descriptor rather than a null one. A slot is only safe to leave unwritten
    // if the shader provably never indexes it. Binding 1 is small and its data
    // drives a shader loop bound, so CreateTransientFrameSet fills all of it.

    m_LayoutDesc.debugName = "UISdfTextureSet";
    m_LayoutDesc.bindings.clear();

    DescriptorBinding binding{};
    binding.binding = 0;
    binding.type = DescriptorType::Texture;
    binding.count = m_MaxTextures;
    binding.shaderStages = kShaderStageFragment;
    binding.debugName = "textures";
    binding.flags = kDescriptorBindingPartiallyBound;
    m_LayoutDesc.bindings.push_back(binding);

    // Binding 1: integer texture array for Slug band data (utexture2D)
    DescriptorBinding bandBinding{};
    bandBinding.binding = 1;
    bandBinding.type = DescriptorType::Texture;
    bandBinding.count = m_MaxBandTextures;
    bandBinding.shaderStages = kShaderStageFragment;
    bandBinding.debugName = "bandTextures";
    bandBinding.flags = kDescriptorBindingPartiallyBound;
    m_LayoutDesc.bindings.push_back(bandBinding);

    DescriptorBinding linearSamplerBinding{};
    linearSamplerBinding.binding = 2;
    linearSamplerBinding.type = DescriptorType::Sampler;
    linearSamplerBinding.count = 1;
    linearSamplerBinding.shaderStages = kShaderStageFragment;
    linearSamplerBinding.debugName = "uiLinearSampler";
    m_LayoutDesc.bindings.push_back(linearSamplerBinding);

    DescriptorBinding nearestSamplerBinding{};
    nearestSamplerBinding.binding = 3;
    nearestSamplerBinding.type = DescriptorType::Sampler;
    nearestSamplerBinding.count = 1;
    nearestSamplerBinding.shaderStages = kShaderStageFragment;
    nearestSamplerBinding.debugName = "uiNearestSampler";
    m_LayoutDesc.bindings.push_back(nearestSamplerBinding);
}

// Compat layout (set 1): one binding per slot, in the order
// Shaders/UI/ui_sdf_textures.glsl declares them. Registration stays
// registry-wide — only the per-run bind group narrows indices to slots — so the
// slot ceilings above are unrelated to this layout's width.
void UITextureRegistry::BuildCompatLayoutDesc()
{
    m_MaxTextures = kDefaultMaxTextures;
    m_MaxBandTextures = kDefaultMaxBandTextures;

    m_LayoutDesc.debugName = "UISdfTextureSetCompat";
    m_LayoutDesc.bindings.clear();

    auto AddTexture = [this](uint32_t slot, const char* name, bool unsignedInteger)
    {
        DescriptorBinding b{};
        b.binding = slot;
        b.type = DescriptorType::Texture;
        b.count = 1;
        b.shaderStages = kShaderStageFragment;
        b.debugName = name;
        b.imageIsUnsignedInteger = unsignedInteger;
        m_LayoutDesc.bindings.push_back(b);
    };

    for (uint32_t i = 0; i < kUiCompatTextureSlots; ++i)
        AddTexture(kCompatTextureBinding0 + i, "uiTexture", false);
    for (uint32_t i = 0; i < kUiCompatGlyphSlots; ++i)
        AddTexture(kCompatGlyphCurveBinding0 + i, "uiGlyphCurve", false);
    // Slug band pages are R16G16_UINT, read only with texelFetch.
    for (uint32_t i = 0; i < kUiCompatGlyphSlots; ++i)
        AddTexture(kCompatGlyphBandBinding0 + i, "uiGlyphBand", true);

    DescriptorBinding linearSampler{};
    linearSampler.binding = kCompatLinearSamplerBinding;
    linearSampler.type = DescriptorType::Sampler;
    linearSampler.count = 1;
    linearSampler.shaderStages = kShaderStageFragment;
    linearSampler.debugName = "uiLinearSampler";
    m_LayoutDesc.bindings.push_back(linearSampler);

    DescriptorBinding nearestSampler = linearSampler;
    nearestSampler.binding = kCompatNearestSamplerBinding;
    nearestSampler.debugName = "uiNearestSampler";
    m_LayoutDesc.bindings.push_back(nearestSampler);
}

UITextureRegistry::UITextureRegistry(IDevice* device)
    : m_Device(device)
    , m_FramesInFlight(device ? device->GetFramesInFlight() : 1u)
{
    if (!device)
        return;

    // Create a default linear-clamp sampler (1 = linear, 2 = clamp-to-edge).
    SamplerDesc sampDesc{};
    sampDesc.minFilter = 1;
    sampDesc.magFilter = 1;
    sampDesc.mipFilter = 0;
    sampDesc.addressModeU = 2;
    sampDesc.addressModeV = 2;
    sampDesc.addressModeW = 2;
    sampDesc.maxAnisotropy = 1.0f;
    sampDesc.debugName = "UISdfDefaultSampler";
    m_DefaultSampler = device->CreateSampler(sampDesc);

    m_Compat = Rendering::IsCompatShaderProfile();
    if (m_Compat)
        BuildCompatLayoutDesc();
    else
        BuildBindlessLayoutDesc(device->GetCapabilities());

    // Nothing else can observe the registry during construction, so the *Locked
    // helper runs here without taking m_Mutex.
    m_DeviceRebuildGeneration = device->GetDeviceRebuildGeneration();
    CreateDeviceResourcesLocked();
}

void UITextureRegistry::CreateDeviceResourcesLocked()
{
    if (!m_Device)
        return;

    // Create a default linear-clamp sampler (1 = linear, 2 = clamp-to-edge).
    SamplerDesc sampDesc{};
    sampDesc.minFilter = 1;
    sampDesc.magFilter = 1;
    sampDesc.mipFilter = 0;
    sampDesc.addressModeU = 2;
    sampDesc.addressModeV = 2;
    sampDesc.addressModeW = 2;
    sampDesc.maxAnisotropy = 1.0f;
    sampDesc.debugName = "UISdfDefaultSampler";
    m_DefaultSampler = m_Device->CreateSampler(sampDesc);

    // Create nearest-neighbor sampler for integer textures (band data).
    // texelFetch ignores the sampler, but Vulkan requires a valid one.
    {
        SamplerDesc nearestDesc{};
        nearestDesc.minFilter = 0; // nearest
        nearestDesc.magFilter = 0; // nearest
        nearestDesc.mipFilter = 0;
        nearestDesc.addressModeU = 2; // clamp-to-edge
        nearestDesc.addressModeV = 2;
        nearestDesc.addressModeW = 2;
        nearestDesc.maxAnisotropy = 1.0f;
        nearestDesc.debugName = "UISdf_NearestSampler";
        m_NearestSampler = m_Device->CreateSampler(nearestDesc);
    }

    // Slot 0 of each array is reserved as its "no texture" fallback. Both
    // arrays are PARTIALLY_BOUND and materialised into transient descriptor
    // memory that is bump-allocated and never zeroed, so a slot left unwritten
    // exposes whatever descriptor last occupied those bytes. Every slot the
    // shader can index must therefore resolve to a live image of the right
    // type, and slot 0 is the one that is always available to resolve to.
    {
        const uint32_t white = 0xFFFFFFFFu;
        m_DummyTexture = CreateDummyTexture1x1(m_Device, TextureFormat::RGBA8_UNORM,
                                               &white, sizeof(white), "UISdf_DummyWhite");
        if (m_DummyTexture.IsValid())
            m_Binding0Entries[0] = { m_DummyTexture, m_DefaultSampler };
    }

    // Binding 1 is an integer array (utexture2D), so its fallback must be an
    // integer-format image — substituting the RGBA8_UNORM white dummy is a
    // format-class mismatch with undefined results. The format matches the real
    // band textures exactly (R16G16_UINT, see RegisterSlugTextures).
    //
    // Zero-filled is what makes it safe rather than merely well-typed: the
    // shader reads a band record's .x as a curve count and runs it as a loop
    // trip count, so a zeroed record means zero iterations and a blank glyph.
    {
        const uint32_t zeroBandRecord = 0u;
        m_BandDummyTexture = CreateDummyTexture1x1(m_Device, TextureFormat::R16G16_UINT,
                                                   &zeroBandRecord, sizeof(zeroBandRecord),
                                                   "UISdf_DummyBandZero");
        if (m_BandDummyTexture.IsValid())
            m_Binding1Entries[0] = { m_BandDummyTexture, m_NearestSampler };
    }
}

void UITextureRegistry::HealIfDeviceRebuiltLocked()
{
    if (!m_Device)
        return;

    const uint64_t generation = m_Device->GetDeviceRebuildGeneration();
    const uint64_t previousGeneration = m_DeviceRebuildGeneration;
    if (generation == previousGeneration)
        return;

    // Counts are logged with the populations they came from: a registry that was
    // empty at rebuild time drops zero of zero, which must stay distinguishable
    // from a registry that dropped nothing because the heal never ran.
    const size_t colorSlots = m_Binding0Entries.size();
    const size_t bandSlots = m_Binding1Entries.size();
    const size_t atlasPages = m_AtlasPageCache.size();
    size_t slugPages = 0;
    for (const auto& [atlasId, fontCache] : m_SlugCache)
        slugPages += fontCache.Pages.size();

    // Drop, never destroy: the rebuild already destroyed every VkObject these
    // handles named, so a Destroy* here resolves to nothing. Borrowed slot
    // entries go with the owned ones — their owners (UI texture cache, font
    // atlases) re-register during the next paint, and every call site takes its
    // slot index fresh from primitive generation, so no caller is holding an
    // index across this reset.
    m_Binding0Entries.clear();
    m_Binding1Entries.clear();
    m_HandleToIndex.clear();
    m_FreeSlots.clear();
    m_SlugCache.clear();
    m_AtlasPageCache.clear();
    m_NextSlot = 1;
    // Mirrors the member's initial value, so it must be changed with it: band
    // slot 0 is reserved for the band dummy, exactly as color slot 0 is for the
    // white one. A reset to 0 would hand the dummy's reserved slot to the first
    // Slug page, which the fallback path would then silently overwrite.
    m_NextBandSlot = 1;
    m_DummyTexture = {};
    m_BandDummyTexture = {};
    m_DefaultSampler = {};
    m_NearestSampler = {};

    m_DeviceRebuildGeneration = generation;
    CreateDeviceResourcesLocked();

    // Deliberately not re-derived: m_MaxTextures, m_MaxBandTextures and
    // m_LayoutDesc still hold the limits resolved from the ORIGINAL device's
    // capabilities. A rebuild that lands on a different adapter (a fallback
    // after the primary was lost) could report smaller per-stage limits, and
    // this registry would keep handing out slots above them. Re-deriving is not
    // local to this function: the layout desc is baked into the descriptor set
    // layout the SDF pipelines were created against, so changing the binding
    // counts mid-session invalidates those pipeline layouts too.

    Logger::Log::Info(
        "UITextureRegistry: device rebuild {} -> {}; dropped {} color slot(s), {} band slot(s), "
        "{} Slug page(s), {} atlas page(s); re-provisioned dummy={} bandDummy={} linearSampler={} "
        "nearestSampler={}",
        previousGeneration,
        generation,
        colorSlots,
        bandSlots,
        slugPages,
        atlasPages,
        m_DummyTexture.IsValid(),
        m_BandDummyTexture.IsValid(),
        m_DefaultSampler.IsValid(),
        m_NearestSampler.IsValid());
}

UITextureRegistry::~UITextureRegistry()
{
    Shutdown();
}

void UITextureRegistry::Shutdown()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Device)
        return;

    // Destroy cached atlas page GPU textures.
    for (auto& [key, entry] : m_AtlasPageCache)
    {
        if (entry.Texture.IsValid())
            m_Device->DestroyTexture(entry.Texture);
    }
    m_AtlasPageCache.clear();

    m_HandleToIndex.clear();
    m_FreeSlots.clear();
    m_Binding0Entries.clear();
    m_Binding1Entries.clear();

    if (m_DummyTexture.IsValid())
    {
        m_Device->DestroyTexture(m_DummyTexture);
        m_DummyTexture = {};
    }

    if (m_BandDummyTexture.IsValid())
    {
        m_Device->DestroyTexture(m_BandDummyTexture);
        m_BandDummyTexture = {};
    }

    // Destroy Slug cached textures (all pages per font).
    for (auto& [id, fontCache] : m_SlugCache)
    {
        for (auto& page : fontCache.Pages)
        {
            if (page.CurveTexture.IsValid())
                m_Device->DestroyTexture(page.CurveTexture);
            if (page.BandTexture.IsValid())
                m_Device->DestroyTexture(page.BandTexture);
        }
    }
    m_SlugCache.clear();

    if (m_NearestSampler.IsValid())
    {
        m_Device->DestroySampler(m_NearestSampler);
        m_NearestSampler = {};
    }

    if (m_DefaultSampler.IsValid())
    {
        m_Device->DestroySampler(m_DefaultSampler);
        m_DefaultSampler = {};
    }

    m_Device = nullptr;
}

uint32_t UITextureRegistry::AllocateSlot()
{
    if (!m_FreeSlots.empty())
    {
        uint32_t slot = m_FreeSlots.back();
        m_FreeSlots.pop_back();
        return slot;
    }

    if (m_NextSlot >= m_MaxTextures)
    {
        LogTextureLimitExceededLocked("binding0");
        return 0;
    }

    return m_NextSlot++;
}

uint32_t UITextureRegistry::AllocateBandSlot()
{
    if (m_NextBandSlot >= m_MaxBandTextures)
    {
        LogTextureLimitExceededLocked("band");
        // Slot 0 (the zero-filled band dummy) is the exhaustion fallback, as it
        // is for binding 0. Returning m_MaxBandTextures would hand the shader an
        // index one past the end of the descriptor array.
        return 0;
    }

    return m_NextBandSlot++;
}

void UITextureRegistry::LogTextureLimitExceededLocked(const char* poolName)
{
    if (m_TextureLimitWarningBudget <= 0)
        return;
    --m_TextureLimitWarningBudget;

    // distinctTex far below active binding0 means the pool is full of duplicate
    // or reserved-but-not-released slots (a leak) rather than genuinely-distinct
    // images; handleToIndex is the deduped (registered) slot count, so the gap
    // to active is the per-frame RG ReserveSlot bindings. This breakdown is what
    // distinguishes a real fill from a slot leak. Only runs when already over
    // budget (rare) and is capped by m_TextureLimitWarningBudget.
    std::unordered_set<uint64_t> distinctTex;
    for (const auto& [slot, e] : m_Binding0Entries)
        distinctTex.insert(static_cast<uint64_t>(e.Texture.id));

    Logger::Log::Warning(
        "[UITextureRegistry] Exceeded UI texture limit in {}; max binding0={}, max band={}, active binding0={}, band={}, free={} | distinctTex={} handleToIndex={}",
        poolName ? poolName : "unknown",
        m_MaxTextures,
        m_MaxBandTextures,
        m_Binding0Entries.size(),
        m_Binding1Entries.size(),
        m_FreeSlots.size(),
        distinctTex.size(),
        m_HandleToIndex.size());
}

void UITextureRegistry::FreeSlot(uint32_t index)
{
    if (index == 0)
        return;
    // Immediate return to the free pool: CreateTransientFrameSet allocates a
    // fresh descriptor set every frame and writes its own snapshot of the
    // active slot state, so any in-flight frame's transient set already holds
    // its own descriptor copy for this slot. No cross-frame aliasing means no
    // need to park-and-retire as pre-Session-7 did.
    m_FreeSlots.push_back(index);
}

uint32_t UITextureRegistry::Register(TextureHandle tex, SamplerHandle sampler)
{
    return RegisterPreUploaded(tex, sampler);
}

std::vector<TextureHandle> UITextureRegistry::GetTextureHandlesForSlots(
    const std::vector<uint32_t>& slots) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    std::vector<TextureHandle> result;
    result.reserve(slots.size());
    for (const auto slot : slots)
        if (const auto it = m_Binding0Entries.find(slot); it != m_Binding0Entries.end())
            result.push_back(it->second.Texture);
    return result;
}

uint32_t UITextureRegistry::RegisterPreUploadedLocked(TextureHandle tex, SamplerHandle sampler)
{
    if (!m_Device || !tex.IsValid())
        return 0;

    // Use texture+sampler as the map key so the same image can be displayed
    // with either linear or point filtering in different UI elements.
    const uint64_t samplerId = sampler.IsValid() ? static_cast<uint64_t>(sampler.id) : 0ull;
    const auto key = std::make_pair(static_cast<uint64_t>(tex.id), samplerId);
    auto it = m_HandleToIndex.find(key);
    if (it != m_HandleToIndex.end())
        return it->second;

    uint32_t slot = AllocateSlot();
    if (slot == 0)
        return 0;

    SamplerHandle samp = sampler.IsValid() ? sampler : m_DefaultSampler;
    m_Binding0Entries[slot] = { tex, samp };
    m_HandleToIndex[key] = slot;
    return slot;
}

uint32_t UITextureRegistry::RegisterPreUploaded(TextureHandle tex, SamplerHandle sampler)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    HealIfDeviceRebuiltLocked();
    return RegisterPreUploadedLocked(tex, sampler);
}

void UITextureRegistry::Unregister(uint32_t index)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    HealIfDeviceRebuiltLocked();
    UnregisterLocked(index);
}

void UITextureRegistry::UnregisterByHandle(TextureHandle tex)
{
    if (!tex.IsValid())
        return;
    std::lock_guard<std::mutex> lock(m_Mutex);
    HealIfDeviceRebuiltLocked();
    const uint64_t textureId = static_cast<uint64_t>(tex.id);
    std::vector<uint32_t> slots;
    for (const auto& [key, slot] : m_HandleToIndex)
    {
        if (key.first == textureId)
            slots.push_back(slot);
    }
    for (uint32_t slot : slots)
        UnregisterLocked(slot);
}

void UITextureRegistry::UnregisterLocked(uint32_t index)
{
    if (index == 0)
        return;

    for (auto it = m_HandleToIndex.begin(); it != m_HandleToIndex.end(); ++it)
    {
        if (it->second == index)
        {
            m_HandleToIndex.erase(it);
            break;
        }
    }

    // Replace the slot's state with the dummy before parking it. Previous
    // frames still in-flight keep reading the transient set they were
    // materialised from; that set captured the slot's old texture before
    // Unregister ran and is safely owned by the frame's pool / DB ring.
    if (m_DummyTexture.IsValid())
        m_Binding0Entries[index] = { m_DummyTexture, m_DefaultSampler };
    else
        m_Binding0Entries.erase(index);

    FreeSlot(index);
}

uint32_t UITextureRegistry::ReserveSlot()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    HealIfDeviceRebuiltLocked();
    uint32_t slot = AllocateSlot();
    if (slot != 0 && m_DummyTexture.IsValid())
    {
        // Seed the slot with the dummy so a primitive referencing it before
        // UpdateSlot lands reads white rather than a stale binding.
        m_Binding0Entries[slot] = { m_DummyTexture, m_DefaultSampler };
    }
    return slot;
}

void UITextureRegistry::ReleaseSlot(uint32_t slot)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    HealIfDeviceRebuiltLocked();
    if (slot == 0)
        return;
    // Reserved/deferred slots are not part of m_HandleToIndex dedup. Clear
    // their binding state before recycling so CreateTransientFrameSet never
    // re-emits a stale texture handle after the source RG texture is retired.
    if (m_DummyTexture.IsValid())
        m_Binding0Entries[slot] = { m_DummyTexture, m_DefaultSampler };
    else
        m_Binding0Entries.erase(slot);
    FreeSlot(slot);
}

void UITextureRegistry::UpdateSlot(uint32_t slot, TextureHandle tex, SamplerHandle sampler)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    HealIfDeviceRebuiltLocked();
    if (!m_Device || slot == 0 || !tex.IsValid())
        return;
    SamplerHandle samp = sampler.IsValid() ? sampler : m_DefaultSampler;
    m_Binding0Entries[slot] = { tex, samp };
}

DescriptorSetHandle UITextureRegistry::CreateTransientFrameSet()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Device)
        return {};

    // Heal before the first descriptor write of the frame. Every handle written
    // below is then one the live device can resolve, so the descriptor-buffer
    // path has no dropped write to report for this set.
    HealIfDeviceRebuiltLocked();

    DescriptorSetDesc dsDesc{};
    dsDesc.layout = m_LayoutDesc;
    dsDesc.debugName = "UISdfTextureDescSet";
    dsDesc.transient = true;
    DescriptorSetHandle set = m_Device->CreateDescriptorSet(dsDesc);
    if (!set.IsValid())
        return {};

    if (m_DefaultSampler.IsValid())
        m_Device->UpdateSamplerBinding(set, 2, m_DefaultSampler);
    if (m_NearestSampler.IsValid())
        m_Device->UpdateSamplerBinding(set, 3, m_NearestSampler);

    // Binding 0: sampled-image array of UI textures (icons, atlas pages,
    // Slug curve textures, RG-resolved scene views). Typically <30 active slots.
    for (auto& [slot, e] : m_Binding0Entries)
    {
        TextureHandle texture = e.Texture;
        if (texture.IsValid() && !m_Device->IsTextureHandleLive(texture))
        {
            texture = m_DummyTexture;
            e.Texture = texture;
            e.Sampler = m_DefaultSampler;
        }
        if (texture.IsValid())
            m_Device->UpdateImageBinding(set, 0, texture, slot);
    }

    // Binding 1: utexture2D array of Slug band textures (one per font page).
    // Every slot is written, not just the occupied ones. The array is
    // PARTIALLY_BOUND over transient descriptor memory that is bump-allocated
    // and never zeroed, so an unwritten slot hands the shader a recycled
    // descriptor from whatever previously occupied those bytes — and the shader
    // turns this data into a loop trip count, so a stale descriptor here is a
    // GPU hang, not a wrong pixel. Retired textures fall back to the same
    // zero-filled integer dummy; the RGBA8_UNORM one belongs to binding 0 and
    // would be a format-class mismatch in an integer array.
    for (uint32_t slot = 0; slot < m_MaxBandTextures; ++slot)
    {
        TextureHandle texture = m_BandDummyTexture;
        if (auto it = m_Binding1Entries.find(slot); it != m_Binding1Entries.end())
        {
            SlotEntry& e = it->second;
            if (e.Texture.IsValid() && !m_Device->IsTextureHandleLive(e.Texture))
            {
                e.Texture = m_BandDummyTexture;
                e.Sampler = m_NearestSampler;
            }
            if (e.Texture.IsValid())
                texture = e.Texture;
        }
        if (texture.IsValid())
            m_Device->UpdateImageBinding(set, 1, texture, slot);
    }

    return set;
}

TextureHandle UITextureRegistry::ResolveSlotTextureLocked(
    std::unordered_map<uint32_t, SlotEntry>& entries, uint32_t index)
{
    auto it = entries.find(index);
    if (it == entries.end())
        return m_DummyTexture;

    TextureHandle texture = it->second.Texture;
    if (texture.IsValid() && !m_Device->IsTextureHandleLive(texture))
    {
        texture = m_DummyTexture;
        it->second.Texture = texture;
        it->second.Sampler = m_DefaultSampler;
    }
    return texture.IsValid() ? texture : m_DummyTexture;
}

DescriptorSetHandle UITextureRegistry::CreateDrawRunSet(const UICompatDrawRun& run)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Device)
        return {};

    DescriptorSetDesc dsDesc{};
    dsDesc.layout = m_LayoutDesc;
    dsDesc.debugName = "UISdfTextureDescSetCompat";
    dsDesc.transient = true;
    DescriptorSetHandle set = m_Device->CreateDescriptorSet(dsDesc);
    if (!set.IsValid())
        return {};

    if (m_DefaultSampler.IsValid())
        m_Device->UpdateSamplerBinding(set, kCompatLinearSamplerBinding, m_DefaultSampler);
    if (m_NearestSampler.IsValid())
        m_Device->UpdateSamplerBinding(set, kCompatNearestSamplerBinding, m_NearestSampler);

    // Every slot is written, claimed or not: the compat layout has no
    // partially-bound flag to lean on, and an unwritten binding is a validation
    // error on the strict backends this profile exists for.
    for (uint32_t i = 0; i < kUiCompatTextureSlots; ++i)
    {
        const TextureHandle tex = i < run.TextureCount
                                      ? ResolveSlotTextureLocked(m_Binding0Entries, run.Textures[i])
                                      : m_DummyTexture;
        m_Device->UpdateImageBinding(set, kCompatTextureBinding0 + i, tex, 0);
    }
    for (uint32_t i = 0; i < kUiCompatGlyphSlots; ++i)
    {
        const bool claimed = i < run.GlyphCount;
        const TextureHandle curve =
            claimed ? ResolveSlotTextureLocked(m_Binding0Entries, run.GlyphCurves[i]) : m_DummyTexture;
        const TextureHandle band =
            claimed ? ResolveSlotTextureLocked(m_Binding1Entries, run.GlyphBands[i]) : m_BandDummyTexture;
        m_Device->UpdateImageBinding(set, kCompatGlyphCurveBinding0 + i, curve, 0);
        m_Device->UpdateImageBinding(set, kCompatGlyphBandBinding0 + i, band, 0);
    }

    return set;
}

// Helper: upload raw data to a GPU texture via staging buffer.
static void UploadTextureData(IDevice* device, TextureHandle tex,
                              uint32_t w, uint32_t h,
                              const void* data, size_t dataSize, uint32_t rowPitch,
                              bool wasShaderResource)
{
    BufferDesc st{};
    st.size = dataSize;
    st.usage = static_cast<uint32_t>(BufferUsage::TransferSrc);
    st.memoryUsage = BufferMemoryUsage::Upload;
    st.debugName = "UISdf_TexStaging";
    BufferHandle staging = device->CreateBuffer(st);
    device->UpdateBuffer(staging, 0, dataSize, data);

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(
        tex,
        wasShaderResource ? ResourceState::ShaderResource : ResourceState::Undefined,
        ResourceState::CopyDest));
    cl->CopyBufferToTextureSubresource(staging, tex, 0, 0, w, h, 0, rowPitch);
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(
        tex, ResourceState::CopyDest, ResourceState::ShaderResource));
    cl->End();
    device->ExecuteCommandLists({cl.get()});
    device->DestroyBuffer(staging);
}

// Convert a block of float values to half-float (IEEE 754 binary16).
// Used to upload Slug curve data into an RGBA16F texture.
static void ConvertFloatsToHalf(const float* src, uint16_t* dst, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        float f = src[i];
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        uint32_t sign = (bits >> 31) & 1;
        int32_t exp = ((bits >> 23) & 0xFF) - 127;
        uint32_t mantissa = bits & 0x7FFFFF;
        uint16_t h;
        if (exp > 15) h = static_cast<uint16_t>((sign << 15) | 0x7C00);
        else if (exp < -14) h = static_cast<uint16_t>(sign << 15);
        else h = static_cast<uint16_t>((sign << 15) | ((exp + 15) << 10) | (mantissa >> 13));
        dst[i] = h;
    }
}

const std::vector<UITextureRegistry::SlugTextureIndices>&
UITextureRegistry::RegisterSlugTextures(const FontAtlas& atlas)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    static const std::vector<SlugTextureIndices> kEmpty;
    if (!m_Device)
        return kEmpty;

    // Runs ahead of the read-only fast path below: after a rebuild the cached
    // pages name destroyed textures, so returning them would hand a worker
    // indices into dead slots. In practice the collect phase pre-registers every
    // font from the owner thread, so the heal lands there and workers only ever
    // see the already-healed cache.
    HealIfDeviceRebuiltLocked();

    const uint32_t atlasId = atlas.GetAtlasId();
    const int pageCount = atlas.GetSlugPageCount();
    if (pageCount <= 0)
        return kEmpty;

    // Read-only fast path: everything registered and up to date. This keeps
    // steady-state calls free of writes, which matters for the parallel
    // drain — workers re-request the slug pages per text emission while the
    // UI thread's collect phase has already registered/uploaded them.
    if (auto it = m_SlugCache.find(atlasId); it != m_SlugCache.end() &&
        it->second.Pages.size() == static_cast<size_t>(pageCount))
    {
        bool current = true;
        for (int pi = 0; pi < pageCount; ++pi)
        {
            const SlugPageEntry& entry = it->second.Pages[pi];
            if (!entry.CurveTexture.IsValid() ||
                entry.UploadedGen != atlas.GetSlugPageGeneration(pi))
            {
                current = false;
                break;
            }
        }
        if (current)
            return it->second.Indices;
    }

    // Write path: creates/uploads GPU textures and may reallocate Indices,
    // invalidating references held by concurrent readers. The parallel drain
    // pre-registers every font during collect precisely so workers stay on
    // the fast path above.
    assert(std::this_thread::get_id() == m_OwnerThreadId &&
           "RegisterSlugTextures write path reached from a non-owner thread");

    SlugFontCache& cache = m_SlugCache[atlasId];
    cache.Pages.resize(static_cast<size_t>(pageCount));
    cache.Indices.resize(static_cast<size_t>(pageCount));

    static constexpr int kTexWidth = FontAtlas::kSlugTextureWidth;
    static constexpr int kPageRows = FontAtlas::kSlugPageRows;

    for (int pi = 0; pi < pageCount; ++pi)
    {
        SlugPageEntry& entry = cache.Pages[pi];
        const uint32_t pageGen = atlas.GetSlugPageGeneration(pi);

        // First time we see this page: allocate the GPU textures and
        // claim stable bindless slots. Within a device generation neither the
        // textures nor the slots change after this — only the texture CONTENTS
        // are re-uploaded when pageGen advances. A device rebuild discards the
        // cache entirely, so the next call lands here again on fresh handles.
        if (!entry.CurveTexture.IsValid())
        {
            TextureDesc ct{};
            ct.width = kTexWidth;
            ct.height = kPageRows;
            ct.mipLevels = 1;
            ct.arrayLayers = 1;
            ct.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
            ct.usage = static_cast<uint32_t>(TextureUsage::ShaderResource)
                     | static_cast<uint32_t>(TextureUsage::TransferDst);
            ct.debugName = "UISlug_CurveTex";
            entry.CurveTexture = m_Device->CreateTexture(ct);
            entry.CurveSlot = RegisterPreUploadedLocked(entry.CurveTexture, m_DefaultSampler);

            TextureDesc bt{};
            bt.width = kTexWidth;
            bt.height = kPageRows;
            bt.mipLevels = 1;
            bt.arrayLayers = 1;
            bt.format = static_cast<uint32_t>(TextureFormat::R16G16_UINT);
            bt.usage = static_cast<uint32_t>(TextureUsage::ShaderResource)
                     | static_cast<uint32_t>(TextureUsage::TransferDst);
            bt.debugName = "UISlug_BandTex";
            entry.BandTexture = m_Device->CreateTexture(bt);
            entry.BandSlot = AllocateBandSlot();
            // Slot 0 means the band pool is exhausted; it holds the zero-filled
            // dummy, so this page's glyphs render blank instead of sampling a
            // slot that was never assigned to them.
            if (entry.BandSlot != 0)
            {
                SamplerHandle samp = m_NearestSampler.IsValid() ? m_NearestSampler : m_DefaultSampler;
                m_Binding1Entries[entry.BandSlot] = { entry.BandTexture, samp };
            }

            // uploadedGen is still 0 — the initial upload below will populate the texture.
        }

        // Re-upload only when the page's generation changes (new glyphs packed in).
        // Uploads the entire page; the in-use rows are contiguous at the top so
        // this is a single staged copy. Existing primitives keep pointing at the
        // same stable slot; only the texel data changes.
        if (entry.UploadedGen != pageGen)
        {
            int curveRows = atlas.GetSlugPageCurveRowsUsed(pi);
            int bandRows  = atlas.GetSlugPageBandRowsUsed(pi);
            if (curveRows < 1) curveRows = 1;
            if (bandRows  < 1) bandRows  = 1;

            // Curve data: convert float → half and upload.
            {
                const float* data = atlas.GetSlugPageCurveData(pi);
                size_t floatCount = static_cast<size_t>(kTexWidth) * curveRows * 4;
                m_HalfConversionBuffer.resize(floatCount);
                ConvertFloatsToHalf(data, m_HalfConversionBuffer.data(), floatCount);
                uint32_t rowPitch = static_cast<uint32_t>(kTexWidth * 4 * sizeof(uint16_t));
                UploadTextureData(m_Device, entry.CurveTexture,
                                  kTexWidth, static_cast<uint32_t>(curveRows),
                                  m_HalfConversionBuffer.data(),
                                  floatCount * sizeof(uint16_t), rowPitch,
                                  /*wasShaderResource=*/entry.UploadedGen != 0);
            }

            // Band data: direct upload.
            {
                const uint16_t* data = atlas.GetSlugPageBandData(pi);
                size_t dataSize = static_cast<size_t>(kTexWidth) * bandRows * 2 * sizeof(uint16_t);
                uint32_t rowPitch = static_cast<uint32_t>(kTexWidth * 2 * sizeof(uint16_t));
                UploadTextureData(m_Device, entry.BandTexture,
                                  kTexWidth, static_cast<uint32_t>(bandRows),
                                  data, dataSize, rowPitch,
                                  /*wasShaderResource=*/entry.UploadedGen != 0);
            }

            entry.UploadedGen = pageGen;
        }

        cache.Indices[pi] = {entry.CurveSlot, entry.BandSlot};
    }

    return cache.Indices;
}

uint32_t UITextureRegistry::RegisterColorAtlasPage(const FontAtlas& atlas, int pageIndex)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Device)
        return 0;

    HealIfDeviceRebuiltLocked();

    const uint32_t atlasId = atlas.GetAtlasId();
    const uint64_t key = MakeAtlasPageKey(atlasId, pageIndex, /*isColor=*/true);
    const uint32_t contentGen = atlas.GetColorContentGeneration();

    auto it = m_AtlasPageCache.find(key);
    if (it != m_AtlasPageCache.end())
    {
        auto& entry = it->second;
        if (entry.Texture.IsValid() && entry.RegistryIndex != 0
            && entry.ContentGeneration == contentGen)
            return entry.RegistryIndex;
    }

    if (pageIndex >= atlas.GetColorPageCount())
        return 0;
    auto ref = atlas.GetColorAtlasRef(pageIndex);
    auto pixOpt = atlas.GetColorAtlasPixels(pageIndex);
    if (!pixOpt || ref.width <= 0 || ref.height <= 0)
        return 0;

    const auto& pix = *pixOpt;
    const uint32_t w = static_cast<uint32_t>(ref.width);
    const uint32_t h = static_cast<uint32_t>(ref.height);

    AtlasPageEntry& entry = m_AtlasPageCache[key];
    bool reuseTexture = entry.Texture.IsValid() && entry.Width == w && entry.Height == h;

    if (!reuseTexture)
    {
        if (entry.Texture.IsValid())
        {
            if (entry.RegistryIndex != 0) { UnregisterLocked(entry.RegistryIndex); entry.RegistryIndex = 0; }
            m_Device->DestroyTexture(entry.Texture);
            entry.Texture = {};
        }
        TextureDesc td{};
        td.width = w; td.height = h; td.mipLevels = 1; td.arrayLayers = 1;
        td.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
        td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource)
                 | static_cast<uint32_t>(TextureUsage::TransferDst);
        td.debugName = "UISdf_ColorAtlas";
        entry.Texture = m_Device->CreateTexture(td);
        entry.Width = w; entry.Height = h;
    }
    if (!entry.Texture.IsValid())
        return 0;

    uint32_t rowPitch = static_cast<uint32_t>(pix.rowPitch);
    size_t uploadSize = pix.rowPitch * static_cast<size_t>(h);
    UploadTextureData(m_Device, entry.Texture, w, h, pix.data, uploadSize, rowPitch, reuseTexture);

    if (entry.RegistryIndex == 0)
        entry.RegistryIndex = RegisterPreUploadedLocked(entry.Texture, m_DefaultSampler);

    entry.ContentGeneration = contentGen;
    return entry.RegistryIndex;
}

} // namespace UI
} // namespace GameEngine
