#include "Engine/Rendering/MaterialBindingCache.h"

#include "Engine/Rendering/Material.h"
#include "Logger/Logger.h"

namespace GameEngine
{
namespace Engine::Renderer
{
namespace
{
// Binding debug names must match the declarations the compat profile emits in
// bindless_textures.glsl — MaterialBinder classifies the set by binding 0's
// name, and the shader-meta diff tooling compares them.
constexpr const char* kTextureBindingNames[kTextureSlotArraySize] = {
    "ge_MaterialTextures0", "ge_MaterialTextures1", "ge_MaterialTextures2", "ge_MaterialTextures3",
    "ge_MaterialTextures4", "ge_MaterialTextures5", "ge_MaterialTextures6", "ge_MaterialTextures7",
};
constexpr const char* kSamplerBindingNames[kTextureSlotArraySize] = {
    "ge_MaterialSamplers0", "ge_MaterialSamplers1", "ge_MaterialSamplers2", "ge_MaterialSamplers3",
    "ge_MaterialSamplers4", "ge_MaterialSamplers5", "ge_MaterialSamplers6", "ge_MaterialSamplers7",
};

// Terrain builds sampler2D in the vertex stage and grass compaction samples
// masks in compute, so the material set must be visible to all three stages —
// same reach as the bindless set it replaces.
constexpr uint32_t kMaterialSetStages = Rendering::kShaderStageVertex
                                      | Rendering::kShaderStageFragment
                                      | Rendering::kShaderStageCompute;
} // namespace

Rendering::DescriptorSetLayoutDesc MaterialBindingCache::GetSetLayout()
{
    Rendering::DescriptorSetLayoutDesc layout{};
    layout.bindings.reserve(kTextureSlotArraySize * 2u);

    for (uint32_t slot = 0; slot < kTextureSlotArraySize; ++slot)
    {
        Rendering::DescriptorBinding tex{};
        tex.binding = slot;
        tex.type = Rendering::DescriptorType::Texture;
        tex.count = 1;
        tex.shaderStages = kMaterialSetStages;
        tex.debugName = kTextureBindingNames[slot];
        layout.bindings.push_back(tex);
    }
    for (uint32_t slot = 0; slot < kTextureSlotArraySize; ++slot)
    {
        Rendering::DescriptorBinding smp{};
        smp.binding = kSamplerBindingOffset + slot;
        smp.type = Rendering::DescriptorType::Sampler;
        smp.count = 1;
        smp.shaderStages = kMaterialSetStages;
        smp.debugName = kSamplerBindingNames[slot];
        layout.bindings.push_back(smp);
    }

    layout.debugName = "MaterialTextureSetLayout";
    return layout;
}

Rendering::TextureHandle MaterialBindingCache::DefaultTextureForSlot(uint32_t slotOrdinal) const
{
    // UnassignedSlotDefault, the rule the bindless defaults read too, so both
    // profiles render an unassigned slot identically.
    return DefaultTexture(UnassignedSlotDefault(static_cast<TextureSlot>(slotOrdinal)));
}

Rendering::TextureHandle MaterialBindingCache::DefaultTexture(SlotDefaultTexture texture) const
{
    switch (texture)
    {
    case SlotDefaultTexture::FlatNormal: return m_Defaults.FlatNormal;
    case SlotDefaultTexture::Black:      return m_Defaults.Black;
    case SlotDefaultTexture::White:      break;
    }
    return m_Defaults.White;
}

void MaterialBindingCache::Initialize(Rendering::IDevice* device, const SlotDefaults& defaults,
                                      Rendering::SamplerHandle defaultSampler)
{
    std::lock_guard lock(m_Mutex);
    // Forget, do not release: a second Initialize only happens after a device
    // rebuild, which already destroyed the pool these sets came from.
    m_Sets.clear();
    m_DefaultSet = {};
    m_Device = device;
    m_Defaults = defaults;
    m_DefaultSampler = defaultSampler;
    m_DefaultSet = CreateSetLocked(nullptr, defaultSampler, "MaterialTextureSet.Defaults");
    if (!m_DefaultSet.IsValid())
    {
        Logger::Log::Error(
            "MaterialBindingCache::Initialize: failed to create the default material "
            "texture set; compat-profile draws will have no material set to bind.");
    }
}

void MaterialBindingCache::Shutdown()
{
    Clear();

    std::lock_guard lock(m_Mutex);
    ReleaseLocked(m_DefaultSet);
    m_DefaultSet = {};
    m_Device = nullptr;
    m_Defaults = {};
    m_DefaultSampler = {};
}

Rendering::DescriptorSetHandle MaterialBindingCache::GetOrBuild(const Material& material,
                                                                Rendering::SamplerHandle sampler)
{
    std::lock_guard lock(m_Mutex);
    if (!m_Device)
        return {};

    const Rendering::SamplerHandle effective = sampler.IsValid() ? sampler : m_DefaultSampler;

    auto& entry = m_Sets[material.GetGuid()];
    if (entry.Set.IsValid() && entry.Sampler == effective)
        return entry.Set;

    // A preset change reaches here as a differing sampler handle, so it rebuilds
    // without needing its own invalidation hook.
    ReleaseLocked(entry.Set);
    entry.Set = CreateSetLocked(&material, effective, "MaterialTextureSet");
    entry.Sampler = effective;
    return entry.Set.IsValid() ? entry.Set : m_DefaultSet;
}

Rendering::DescriptorSetHandle MaterialBindingCache::DefaultSet()
{
    std::lock_guard lock(m_Mutex);
    return m_DefaultSet;
}

void MaterialBindingCache::Invalidate(const GUID& materialGuid)
{
    std::lock_guard lock(m_Mutex);
    auto it = m_Sets.find(materialGuid);
    if (it == m_Sets.end())
        return;
    ReleaseLocked(it->second.Set);
    m_Sets.erase(it);
}

void MaterialBindingCache::Clear()
{
    std::lock_guard lock(m_Mutex);
    for (auto& [guid, entry] : m_Sets)
        ReleaseLocked(entry.Set);
    m_Sets.clear();
}

Rendering::DescriptorSetHandle MaterialBindingCache::CreateSetLocked(const Material* material,
                                                                     Rendering::SamplerHandle sampler,
                                                                     const char* debugName)
{
    if (!m_Device)
        return {};

    Rendering::DescriptorSetDesc desc{};
    desc.layout = GetSetLayout();
    desc.debugName = debugName;
    desc.transient = false;
    const Rendering::DescriptorSetHandle set = m_Device->CreateDescriptorSet(desc);
    if (!set.IsValid())
        return {};

    // Every binding is written before the set is ever bound: the layout is not
    // partially-bound, so an unwritten slot is a validation error even when the
    // shader never samples it.
    for (uint32_t slot = 0; slot < kTextureSlotArraySize; ++slot)
    {
        Rendering::TextureHandle tex = material ? material->GetTextureForSlot(slot)
                                                : Rendering::TextureHandle{};
        if (!tex.IsValid())
            tex = material && material->IsTextureAwaited(slot)
                      ? DefaultTexture(material->GetAwaitedTextureDefault(slot))
                      : DefaultTextureForSlot(slot);
        m_Device->UpdateImageBinding(set, slot, tex);
        m_Device->UpdateSamplerBinding(set, kSamplerBindingOffset + slot, sampler);
    }
    return set;
}

void MaterialBindingCache::ReleaseLocked(Rendering::DescriptorSetHandle set)
{
    // Persistent-pool sets are reclaimed when their pool is destroyed, so this
    // only drops the device's bookkeeping — a set still referenced by a
    // command buffer in flight stays valid until the pool goes away.
    if (m_Device && set.IsValid())
        m_Device->DestroyDescriptorSet(set);
}

} // namespace Engine::Renderer
} // namespace GameEngine
