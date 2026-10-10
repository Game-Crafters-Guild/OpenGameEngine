#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "Rendering/Core/Handle.h"

namespace GameEngine::Rendering
{
class IDevice;
}

namespace GameEngine::Engine::Renderer
{

class MaterialSystem;

// Owns DDGI's material map atlas: one square array-texture layer per distinct
// base-colour, emissive or metallic-roughness texture bound by a GPU-scene
// material, plus the material -> (layers, uv transform) lookup both trace
// lanes shade with.
// Without it a probe ray sees only the flat uBaseColor / emissive-tint
// factors, which on real authored content are near-uniform grey (all 25 Sponza
// materials share baseColorFactor 0.588) — so there is no colour in the signal
// to bleed, and a screen-bright textured emitter glows into GI only as much as
// its (often black) tint says.
//
// ONE array texture holds every map kind, deduplicated by source texture
// across slots: a texture bound as base colour on one material and as emissive
// on another costs one layer, not two. All kinds are [0,1] data at the same
// square size, sampled by the same kernels through the same sampler, so
// splitting them would buy nothing and cost a descriptor binding in both
// trace kernels. The metallic-roughness map is there because the hit shader's
// Lambert term is albedo * (1 - metalness): a glTF material leaves
// metallicFactor at its default 1 and puts the real value in the map's blue
// channel, so the flat factor alone reads every such surface as a metal and
// bounces nothing — and ddgi_trace_sw.comp's own header records that it is
// already one SSBO over the web/compat budget.
//
// Why an atlas rather than the engine's bindless texture array: the software
// lane exists precisely for backends without bindless (web/compat, Metal
// before the ray-query lane lands), and the two lanes must shade a hit
// identically or their images stop being comparable. One array texture is the
// narrowest thing both can sample.
//
// Layers are filled on the GPU (ddgi_map_atlas_blit.comp) rather than by CPU
// canvas resampling as the ported reference does: a Material carries a
// Rendering::TextureHandle, not an asset GUID, so a CPU path would mean
// re-decoding the source file off disk on the render thread, and would fail
// outright on block-compressed payloads. Sampling the live texture instead
// gets filtering and the sRGB->linear decode from the hardware, so atlas
// contents are LINEAR and the trace kernels sample them with no decode.
class DDGIMaterialMapAtlas
{
  public:
    // Mirrors the reference's TEXTURE_ATLAS_SIZE.
    static constexpr uint32_t kLayerSize = 256;

    // Ceiling on distinct source textures across both map kinds. 256x256 RGBA8
    // is 256 KiB per layer, so this caps the atlas at 32 MiB. Materials past
    // the ceiling fall back to their flat factors rather than failing the build.
    static constexpr uint32_t kMaxLayers = 128;

    // "No map bound" — the same negative-layer encoding the reference packer
    // uses, carried as a float because the software lane reads it out of a
    // float record.
    static constexpr float kNoLayer = -1.0f;
    // "Map assigned but not resident": the emissive layer of a material whose
    // emissive slot is awaited (Material::IsTextureAwaited). The trace lanes
    // shade it as no emission, as the raster surface does with its black
    // awaited default (GE_DDGIApplyEmissiveMap).
    static constexpr float kAwaitedLayer = -2.0f;

    // One uv transform per material, not one per map kind: the software lane's
    // uber-material record has exactly one (SceneBvh/UberMaterial.h slots
    // 18..21, carried from the reference packer), and the two lanes must agree
    // on where a map is sampled. It comes from the base-colour slot, which is
    // the transform authoring tools drive on the whole surface.
    struct MaterialRecord
    {
        float AlbedoLayer   = kNoLayer;
        float EmissiveLayer = kNoLayer;
        float MetallicLayer = kNoLayer;  // metallic-roughness map; metalness in .b
        float ScaleX        = 1.0f;
        float ScaleY        = 1.0f;
        float OffsetX       = 0.0f;
        float OffsetY       = 0.0f;
    };

    // One layer still waiting for its contents. `Recorded` is the caller's
    // acknowledgement channel: the blit pass sets it once the dispatch has
    // actually been recorded into a command list, and only then does the layer
    // stop being re-armed. Declaring a pass is NOT the acknowledgement —
    // a pipeline variant that fails to compile, a missing shader reflection or
    // a culled pass all leave the layer black, and the layout hash would never
    // move again to trigger a retry.
    struct PendingBlit
    {
        Rendering::TextureHandle Source;
        uint32_t Layer = 0;
        std::shared_ptr<std::atomic<bool>> Recorded;
    };

    DDGIMaterialMapAtlas(Rendering::IDevice* device, MaterialSystem* materials);
    ~DDGIMaterialMapAtlas();

    DDGIMaterialMapAtlas(const DDGIMaterialMapAtlas&) = delete;
    DDGIMaterialMapAtlas& operator=(const DDGIMaterialMapAtlas&) = delete;

    // Re-derives the layer assignment from the material registry. Deliberately
    // not epoch-gated: a texture finishing its stream-in changes the answer
    // without bumping any material epoch. Cheap enough for every tick — a walk
    // of the registry plus a hash, no asset or file access.
    void Refresh();

    // Bumped whenever the layer assignment changes. Consumers that bake a layer
    // index into their own data (DDGISceneService's uber-material records)
    // re-bake when this moves.
    uint64_t GetLayoutEpoch() const { return m_LayoutEpoch; }

    // Layers whose contents have not been written yet, re-derived every
    // Refresh. The caller declares one blit dispatch each and signals each
    // entry's `Recorded` flag from inside the pass; anything it fails to record
    // comes back on the next Refresh.
    std::span<const PendingBlit> GetPendingBlits() const { return m_PendingBlits; }

    // Never fails: an unknown slot resolves to the default record, whose
    // kNoLayer layers keep the caller on the flat factors.
    const MaterialRecord& GetRecordForMaterialSlot(uint32_t gpuSceneMaterialIndex) const;

    Rendering::TextureHandle GetTexture() const { return m_Texture; }
    // Array layers currently allocated. The render graph needs it to import
    // the atlas with its true layer count so a per-layer write and a
    // whole-texture sampled read resolve against the same subresource range.
    uint32_t GetLayerCount() const
    {
        return static_cast<uint32_t>(m_LayerSources.size());
    }
    Rendering::SamplerHandle GetSampler() const { return m_Sampler; }

    // Per-material table for the hardware lane, indexed by
    // GPUInstance.materialIndex — two vec4s per row, matching
    // ddgi_hit_shade.glsl's GE_DDGIMaterialMapRecord. Invalid until the first
    // Refresh that finds a material.
    Rendering::BufferHandle GetTableBuffer() const { return m_TableBuffer; }
    uint64_t GetTableBytes() const { return m_TableBytes; }

  private:
    struct RetiredResources
    {
        Rendering::TextureHandle Texture;
        Rendering::BufferHandle Buffer;
        uint64_t FrameStamp = 0;
    };

    // Per-layer contents state. A layer counts as filled only once a blit for
    // it was recorded; until then it is re-armed, so a texture that becomes
    // sampleable after its layer was assigned still gets its pixels.
    struct LayerFill
    {
        bool Filled = false;
        std::shared_ptr<std::atomic<bool>> Recorded;
    };

    void RecreateTexture(uint32_t layerCount);
    void RearmPendingBlits();
    void UploadTable();
    void CollectRetired();
    void ReleaseAll();

    Rendering::IDevice* m_Device = nullptr;
    MaterialSystem* m_Materials = nullptr;

    Rendering::TextureHandle m_Texture;
    Rendering::SamplerHandle m_Sampler;
    Rendering::BufferHandle m_TableBuffer;
    uint64_t m_TableBytes = 0;

    // Source texture per layer, in layer order — also the identity the layout
    // hash is taken over.
    std::vector<Rendering::TextureHandle> m_LayerSources;
    std::vector<LayerFill> m_LayerFill;
    std::vector<MaterialRecord> m_RecordsByMaterialSlot;
    std::vector<PendingBlit> m_PendingBlits;

    uint64_t m_LayoutHash = 0;
    uint64_t m_LayoutEpoch = 0;
    uint64_t m_FrameClock = 0;
    std::vector<RetiredResources> m_Retired;
};

}  // namespace GameEngine::Engine::Renderer
