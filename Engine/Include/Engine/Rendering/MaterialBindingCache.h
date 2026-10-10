// Per-material descriptor set for the compatibility profile's Classic
// (non-bindless) material texture path. Owned by TextureService.

#pragma once

#include "AssetCore/GUID.h"
#include "Rendering/Core/Device.h"
#include "Types/Types.h"

#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace GameEngine
{
namespace Engine::Renderer
{
class Material;
enum class SlotDefaultTexture : uint8_t;

// Builds and caches the set-1 bind group the compatibility profile samples
// material textures through: one texture per slot ordinal at bindings 0..7 and
// the matching pre-selected sampler at bindings 64..71. The layout must stay in
// lockstep with the GE_COMPAT_PROFILE block of
// Engine/Modules/Rendering/Shaders/Includes/bindless_textures.glsl.
//
// The full profile never touches this class — it binds one global unbounded
// bindless set instead, so nothing here runs on a bindless device.
//
// Thread-safety: GetOrBuild runs on record threads (MaterialBinder) while
// invalidation runs on the render thread, so every entry point takes m_Mutex.
class MaterialBindingCache
{
  public:
    // Sampler bindings sit at texture binding + this, mirroring the shader
    // cook's combined-image-sampler split rule.
    static constexpr uint32_t kSamplerBindingOffset = 64u;

    // Canonical layout for the compat material set. Also patched into every
    // pipeline layout's set 1 so a cached set is bind-compatible with any
    // material pipeline, including variants whose reflection dropped a slot.
    static Rendering::DescriptorSetLayoutDesc GetSetLayout();

    // Per-slot fallbacks, substituted whenever a slot carries no texture (or
    // its texture has not streamed in yet). Mirrors
    // Material::InitBindlessDefaults so both profiles show the same thing.
    struct SlotDefaults
    {
        Rendering::TextureHandle White{};
        Rendering::TextureHandle Black{};
        Rendering::TextureHandle FlatNormal{};
    };

    // `device`, the defaults and `defaultSampler` must outlive this cache.
    // Forgets any prior entries without touching the device: the only way to
    // reach a second Initialize is a device rebuild, whose teardown already
    // freed every set this cache allocated.
    void Initialize(Rendering::IDevice* device, const SlotDefaults& defaults,
                    Rendering::SamplerHandle defaultSampler);
    // Releases every cached set through the (still live) device.
    void Shutdown();

    // Descriptor set for `material`, built on first use. `sampler` is the
    // material's resolved preset sampler; a change to it rebuilds the entry,
    // so the caller needs no separate invalidation for sampler edits.
    Rendering::DescriptorSetHandle GetOrBuild(const Material& material,
                                              Rendering::SamplerHandle sampler);

    // All-defaults set, for draws that bind the material texture set without a
    // material in hand. Always valid once Initialize has run.
    Rendering::DescriptorSetHandle DefaultSet();

    // Drop one material's cached set; the next GetOrBuild rebuilds it from the
    // material's current slot bindings.
    void Invalidate(const GUID& materialGuid);
    // Releases every cached set. Use Initialize (not this) after a device
    // rebuild — the handles are already dead there.
    void Clear();

    Rendering::TextureHandle DefaultTextureForSlot(uint32_t slotOrdinal) const;

  private:
    Rendering::TextureHandle DefaultTexture(SlotDefaultTexture texture) const;

    struct Entry
    {
        Rendering::DescriptorSetHandle Set{};
        Rendering::SamplerHandle Sampler{};
    };

    // Caller must hold m_Mutex.
    Rendering::DescriptorSetHandle CreateSetLocked(const Material* material,
                                                   Rendering::SamplerHandle sampler,
                                                   const char* debugName);
    void ReleaseLocked(Rendering::DescriptorSetHandle set);

    mutable std::mutex m_Mutex;
    Rendering::IDevice* m_Device = nullptr;
    SlotDefaults m_Defaults{};
    Rendering::SamplerHandle m_DefaultSampler{};
    Rendering::DescriptorSetHandle m_DefaultSet{};

    // Keyed by material GUID, not Material*: the GUID is what the
    // material<->texture ref graph invalidates through, and it survives a
    // material reload that reuses the address.
    std::unordered_map<GUID, Entry> m_Sets;
};

} // namespace Engine::Renderer
} // namespace GameEngine
