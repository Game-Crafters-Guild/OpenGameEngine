// GPU texture residency and binding service, owned by RenderServices and
// reached via RenderServices::Textures(). Owns the GUID-keyed GPU texture
// cache, the async load/upload pump, embedded-image decode + upload, the
// bindless SAMPLED_IMAGE array (manager, descriptor set, index cache), the
// standard sampler cache, the default fallback textures, the .cube LUT GPU
// cache, and the material<->texture reference tracking that drives hot-reload
// rebinds.

#pragma once

#include "AssetCore/Asset.h"         // SharedPtr<Asset> in PendingGpuUpload
#include "Assets/AssetManager.h"     // AssetLoadHandle in m_TextureLoadHandles
#include "Engine/Rendering/EmbeddedImageDecoder.h"
#include "Engine/Rendering/CpuTextureSource.h"
#include "Engine/Rendering/MaterialBindingCache.h"
#include "Engine/Rendering/TextureGPUCache.h"
#include "JobSystem/JobCounter.h"
#include "Rendering/Core/BindlessResourceManager.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RendererProfile.h"
#include "Types/Types.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GameEngine
{
struct EmbeddedImage;
struct MaterialDocument;
// Forward-declared, not included: Assets/TextureAsset.h drags in GameEngine::TextureFormat, which
// collides with GameEngine::Rendering::TextureFormat in every TU that reaches RenderServices.h.
// Definitions live in Assets/TextureAsset.h and Assets/TextureCook.h.
enum class TextureColorSpace : uint8;
enum class TextureCookUsage : uint8;
class TextureAsset;

namespace Rendering
{
class NamedPushConstantWriter;
}

namespace Engine::Renderer
{
using ::GameEngine::Rendering::BindlessResourceManager;
using ::GameEngine::Rendering::BindlessTextureDesc;
using ::GameEngine::Rendering::BindlessTextureHandle;
using ::GameEngine::Rendering::DescriptorSetHandle;
using ::GameEngine::Rendering::IDevice;
using ::GameEngine::Rendering::SamplerHandle;
using ::GameEngine::Rendering::SamplerPreset;
using ::GameEngine::Rendering::TextureAspect;
using ::GameEngine::Rendering::TextureHandle;
using ::GameEngine::Rendering::TextureViewHandle;

class Material;
enum class SlotDefaultTexture : uint8_t;
class MaterialRegistry;
struct PostProcessSettings;

// GPU-backed Resolve .cube LUT (3D + optional 1D strip). Used by fullscreen passes.
struct CubeLutGpuBindingState
{
    TextureHandle Lut3D{};
    TextureHandle Lut1DStrip{};
    int32 Size3D = 0;
    int32 Size1D = 0;
    int32 Flags = 0; // bit0 = has 3D, bit1 = has 1D
    float In1DMin = 0.0f;
    float In1DMax = 1.0f;
    float In3DMin = 0.0f;
    float In3DMax = 1.0f;
};

class TextureService
{
  public:
    TextureService();
    ~TextureService();

    // Creates the bindless manager + global texture set (sized from device
    // caps), the default fallback textures, and the identity LUTs. `materials`
    // is the registry the async bind sweep resolves Material* from; it must
    // outlive this service. `profile` is the renderer profile resolved from
    // `device`; the preset samplers take their descriptions from it.
    bool Initialize(IDevice* device, MaterialRegistry& materials,
                    const Rendering::RendererProfile& profile);

    // Destroys every GPU resource this service owns (cached textures, embedded
    // uploads, LUTs, defaults, samplers, bindless set/manager). Pending async
    // loads are cancelled. Call before device teardown.
    void Shutdown();

    // Cancel in-flight AssetManager loads and drop the pending queues + ref maps.
    // Early-shutdown phase: call before the material registry tears down;
    // Shutdown() also runs it.
    void CancelPendingLoads();

    // Q6 device-lost re-provision (design §8 bindless row / F6). The in-place
    // device rebuild freed every GPU object this service created (bindless set,
    // sampler array, default textures + LUTs, cached uploads). Recreate them on
    // the rebuilt device, CLEAR the id-keyed bindless cache (a stale entry would
    // COLLIDE with a reissued handle id, not miss), re-bake every material's
    // default bindless indices, and replay the retained material<->texture ref
    // graph to re-upload real textures from the (RAM-resident) AssetManager. The
    // ref graph and CPU decode cache are PRESERVED (unlike Shutdown). Runs on the
    // render thread inside the rebuild callback, with the device usable.
    void ReprovisionAfterDeviceRebuild();

    // Engine-owned CPU image publication for pipeline inputs. Duplicate live
    // (world, reset, name) publishers are rejected. Creation/publication do not
    // touch the GPU; the ordinary upload drain commits complete revisions.
    CpuTextureSource CreateCpuTextureSource(const CpuTextureSourceDesc& desc);
    // World-owning render-loop boundary, before BeginWorldDrawFrame. New leases
    // stay unavailable until this acknowledges their exact world/reset scope.
    void ObserveCpuTextureWorld(CpuTextureScope scope);
    // Engine post-simulation boundary only, before graph declaration. Commits
    // CPU revisions published by late hooks without re-running asset reloads.
    // A captured frame binding must never outlive a subsequent drain.
    void FlushCpuTextureUploads();
    // Render-thread only. Borrowed for this frame; never retain across a drain.
    CpuTextureBinding GetCpuTextureForView(uint64_t worldId, StringId name) const;

    // ── Material indexing ─────────────────────────────────────────────────
    // How material textures reach shaders on this device. Bindless indexes one
    // global unbounded array per instance; Classic (the compatibility profile,
    // no descriptor indexing) binds a per-material set built by
    // MaterialBindingCache. Material params ride the shared SSBO either way.
    enum class MaterialIndexingMode { Bindless, Classic };
    static MaterialIndexingMode SelectMaterialIndexingMode(const Rendering::RenderingDeviceCapabilities& caps);
    MaterialIndexingMode GetMaterialIndexingMode() const { return m_IndexingMode; }

    bool IsBindlessEnabled() const { return m_BindlessEnabled.load(std::memory_order_relaxed); }
    DescriptorSetHandle BindlessTextureSet() const { return m_BindlessTextureSet; }
    static Rendering::DescriptorSetLayoutDesc GetBindlessTextureSetLayout();
    static Rendering::DescriptorSetLayoutDesc GetBindlessTextureSetLayout(uint32_t maxTextures);
    uint32_t ResolveDefaultBindlessIndex(StringId slotName) const;
    // The same per-slot default as a handle, for the classic (non-bindless) path
    // where a descriptor write needs the texture itself rather than its index.
    // Takes the raw binding name: unlike the bindless path this also serves shader
    // bindings that are not well-known material slots.
    TextureHandle ResolveDefaultTexture(std::string_view bindingName) const;

    // Classic mode: the set-1 bind group `material` samples its textures
    // through, built on first use and rebuilt after any slot rebind. Returns an
    // invalid handle in Bindless mode — callers there bind BindlessTextureSet().
    DescriptorSetHandle MaterialTextureSet(const Material& material);
    // Classic mode: the all-defaults set, for draws with no material in hand.
    DescriptorSetHandle DefaultMaterialTextureSet();

    // Register a texture in the global bindless SAMPLED_IMAGE array and return its index.
    // Deduped by texture (+ view: arraySlice/mipLevel/aspect); the sampler is selected
    // per-draw shader-side from the shared sampler array, so it's not part of the key.
    // Returns 0 on failure (bindless disabled, invalid handle).
    uint32_t GetBindlessIndex(TextureHandle tex);
    // Advanced: full BindlessTextureDesc (array slices, mip levels, aspect).
    uint32_t GetBindlessIndex(const BindlessTextureDesc& desc);

    // Evict all bindless cache entries for a texture (all view variants) and free
    // the descriptor slots. Thread-safe.
    //
    // PRECONDITION: callers must FIRST repair every Material::m_BindlessTextureIndices
    // entry that points at a slot owned by this texture, redirecting it to a
    // default via ResolveDefaultBindlessIndex (or a freshly-uploaded replacement).
    // Slots are parked for delayed reuse but eventually get re-issued; a material
    // still holding the old index would then sample the new occupant. Evict()
    // honours this contract by walking the material bindings first — any new
    // caller must do the same.
    void InvalidateBindless(TextureHandle tex);

    // Park a dynamically created texture — plus every bindless cache entry that
    // references it — for destruction on the render thread's FlushPendingUploads
    // drain. The worker-safe counterpart of InvalidateBindless + IDevice
    // DestroyTexture for textures the caller created directly (video frames):
    // it takes only m_BindlessMutex and appends to a queue, so ECS wave workers
    // may call it.
    //
    // What deferral buys is ORDER, not mutual exclusion — IDevice's own destroy
    // entry points are already deferred and mutex-guarded. Retiring the bindless
    // slot is the part that cannot happen mid-tick: DestroyBindlessTexture parks
    // the descriptor index against BindlessResourceManager's generation counter,
    // and the drain runs on the render thread AFTER the tick that repaired the
    // materials pointing at it. Same PRECONDITION as InvalidateBindless: repair
    // every material bindless index pointing at this texture FIRST.
    void DiscardTextureDeferred(TextureHandle tex);

    // ── Samplers + defaults ───────────────────────────────────────────────
    // Retrieve (or lazily create) the cached sampler for a standard preset, built from
    // ResolveSamplerPreset(preset, profile) with the profile Initialize received.
    SamplerHandle GetSampler(SamplerPreset preset);

    // Default textures (1x1 fallbacks bound when a material slot has no texture assigned).
    TextureHandle GetDefaultWhiteTexture() const { return m_DefaultWhiteTexture; }
    TextureHandle GetDefaultBlackTexture() const { return m_DefaultBlackTexture; }
    TextureHandle GetDefaultFlatNormalTexture() const { return m_DefaultFlatNormalTexture; }
    TextureHandle GetDefaultLensFlareTexture() const { return m_DefaultLensFlareTexture; }
    // Shape defaults for a sampled-image slot nothing provides (MaterialBinder): a slot's default
    // has the slot's view dimension, or a backend that validates the whole bind group (WebGPU)
    // rejects it. A 1x1 white one-layer 2D-array view, and a 1x1 black cube (a missing
    // environment adds no light).
    TextureHandle GetDefaultWhiteArrayTexture() const { return m_DefaultWhiteArrayTexture; }
    TextureHandle GetDefaultBlackCubeTexture() const { return m_DefaultBlackCubeTexture; }
    // Bindless indices of the default textures (0 when bindless is disabled).
    uint32_t DefaultWhiteBindlessIndex() const { return m_DefaultWhiteBindlessIndex; }
    uint32_t DefaultBlackBindlessIndex() const { return m_DefaultBlackBindlessIndex; }
    uint32_t DefaultFlatNormalBindlessIndex() const { return m_DefaultFlatNormalBindlessIndex; }
    // Bindless index of the default an UNASSIGNED slot at `ordinal` samples
    // (UnassignedSlotDefault in Material.h).
    uint32_t UnassignedSlotBindlessIndex(uint32_t ordinal) const;

    // ── GPU texture cache ─────────────────────────────────────────────────
    // Declare what a texture IS — its color space and cook usage — for a consumer that
    // resolves textures by GUID instead of through a material slot, such as the terrain
    // material table and the ocean's normal and foam textures: they hold bindless indices and
    // never build a Material, so the slot-name ladder that classifies every other texture in
    // the engine never sees them, and an untagged normal map would upload sRGB and decode to
    // garbage.
    //
    // Call it BEFORE GetOrUpload: the tag has to reach the AssetDatabase ahead of the upload
    // that reads it. Idempotent: an explicit colour space from the importer or the texture
    // inspector wins; the usage tag widens when one wider usage serves this declaration and
    // every earlier binding (WidenTextureCookUsage), never narrows, and is kept with one
    // warning when no single cook serves both. Calling it again costs two metadata lookups and
    // nothing else. TextureColorSpace::Unknown / TextureCookUsage::Auto mean "no opinion".
    //
    // Callable from ECS extraction workers: a newly tagged texture that is already resident is
    // refreshed through RequestReupload (parked for the render thread), never through Evict,
    // whose material-registry walk and DestroyTexture are render-thread only.
    void DeclareTextureClassification(const GUID& textureGuid, TextureColorSpace colorSpace,
                                      TextureCookUsage usage);

    // Upload a texture asset to the GPU (cached by GUID). Invalid handle on failure.
    TextureHandle GetOrUpload(const GUID& textureGuid);

    // True when this texture's sampled alpha is provably 1.0 over every texel, so a consumer's
    // alpha path (cutout, screen-door, alpha-to-coverage) has nothing to resolve and can be
    // skipped outright. Answers the cutoff-INDEPENDENT question: "is there any alpha here at
    // all", not "does it discard at threshold T".
    //
    // Conservative on every uncertainty — an unresolvable GUID, an undecodable container, or a
    // decode deferred by the probe's concurrency cap all report false, which keeps the caller's
    // alpha path. Only a definite answer turns it off, so a wrong reading can cost work, never
    // correctness.
    //
    // Cached per (GUID, load epoch) so a hot-reloaded texture is re-probed rather than answered
    // from the pre-edit file; a deferred probe is never cached. Callable from ECS extraction
    // workers.
    bool AlphaIsUniformlyOpaque(const GUID& textureGuid);

    // Resolve a material texture reference (authored GUID string + optional
    // source-relative path companion) to the canonical asset GUID, mirroring the
    // scene loader's guid-then-path ladder. Non-GUID sentinels ("__embedded:N")
    // yield Null and route to embedded handling.
    GUID ResolveTextureRefGuid(const std::string& guidStr, const std::string& pathStr) const;

    // Upload embedded image data (from GLB models) to the GPU, cached by the
    // derived (modelGuid, imageIndex[, /linear]) GUID. `linear` selects
    // RGBA8_UNORM (data textures) vs RGBA8_SRGB (albedo/emissive).
    TextureHandle UploadEmbeddedImage(const GUID& modelGuid, uint32_t imageIndex,
                                      const EmbeddedImage& image, bool linear = false);

    // Drop a single texture from the GPU + bindless caches so the next
    // GetOrUpload reloads (skybox HDRI swaps, hot-reload paths). Repairs
    // material bindings to defaults first and re-queues them for rebind.
    void Evict(const GUID& textureGuid);
    // Thread-safe, non-blocking request to re-upload a texture (e.g. after an
    // import-setting change). The swap runs on the render thread at the next
    // FlushPendingUploads — materials keep sampling the old texture until the
    // new one is ready (no default-texture gap).
    void RequestReupload(const GUID& textureGuid);
    // Re-run the asset decode + import cook for a loaded texture (async, via
    // AssetManager::StartAsyncReload) so a cook-setting change (compression /
    // usage / mips meta) rebuilds the cached artifact; the reload's
    // AssetReloaded event then evicts + re-uploads. No-op while the asset
    // isn't resident — the next load cooks with fresh meta anyway.
    void RequestRecook(const GUID& textureGuid);
    // Hand a reloaded model's embedded images to the materials that sample it,
    // then release the embedded textures (and decode-cache entries) no model
    // references any more. Every material slot ResolveEmbeddedTextures bound
    // from this model and still binding one of its textures is rebound to the
    // same image index of `images` first, or to its awaited default when
    // `images` lacks it (empty: the model left memory), so no material binds a
    // texture when it is destroyed. A tracked slot still awaiting its image
    // takes it once `images` supplies it, unless an asset texture bind owns the
    // slot. Called by the Model hot-reload subscriber.
    // Render thread only.
    void ReloadEmbeddedForModel(const GUID& modelGuid, std::span<const EmbeddedImage> images);

    // GPU-upload all completed async loads and bind them to waiting materials.
    // Render thread only — called once per frame from BeginWorldDrawFrame.
    void FlushPendingUploads();

    // ── Material texture binding ──────────────────────────────────────────
    // Assign a texture + its bindless index to a material slot in one call. An
    // invalid handle is a texture that failed to upload: the slot takes its
    // awaited default (BindAwaitedTexture).
    void BindMaterialTexture(Material* mat, StringId slotName, TextureHandle tex);
    // An ASSIGNED slot whose texture is not resident (its load is pending, it
    // was evicted for a reload, or it failed): binds AwaitedSlotDefault's
    // bindless index and marks the slot awaited on the material
    // (Material::MarkTextureAwaited), so the Classic bind group and the DDGI map
    // atlas, which read the slot's texture, treat it the same way.
    void BindAwaitedTexture(Material* mat, StringId slotName);

    // Return a slot to its authored binding after a runtime override (video
    // texture) ends: re-binds the texture the material's document declared for
    // the slot (the tracked BindMaterialTextureRef edge), or clears the slot to
    // its bindless default when the document declares none. An authored texture
    // that is not resident binds once its load lands; this never waits for it.
    // Callable from ECS wave workers.
    void RestoreMaterialTextureBinding(Material* mat, const GUID& matGuid, StringId slotName);

    // Queue a (material, slot, texture) bind and kick off an async load if one
    // isn't already in flight. The upload + bind happen on the render thread in
    // FlushPendingUploads.
    void ScheduleMaterialTextureLoad(const GUID& texGuid, const GUID& matGuid, StringId slotName);

    // Resolve + bind one authored texture reference on a material: color-space
    // tagging (+ stale sRGB upload evict), immediate bind when the texture is
    // already GPU-resident, else async load with deferred bind (synchronous
    // upload when no Engine/AssetManager is available). Records the
    // material<->texture edge for hot-reload rebinds. Supersedes an earlier
    // bind of the same slot still waiting on its load.
    void BindMaterialTextureRef(Material* mat, const GUID& matGuid, StringId slotName,
                                const GUID& texGuid);

    // Drop the tracked edge for one material slot (the slot was cleared).
    void UntrackMaterialTextureRef(const GUID& matGuid, StringId slotName);

    // Resolve embedded texture references ("__embedded:N") on a material using
    // the model's embedded images. Call after RegisterMaterialFromDocument.
    void ResolveEmbeddedTextures(Material* mat, const MaterialDocument& doc,
                                 const GUID& modelGuid,
                                 const Vector<EmbeddedImage>& embeddedImages);

    // Update a material's texture bindings from a MaterialDocument without
    // recompiling the pipeline. Binds as registration does (BindMaterialTextureRef):
    // a resident upload at once, otherwise once its worker load lands, so the
    // editor's texture assign never decodes on the calling thread. Render thread.
    void UpdateMaterialTextures(const GUID& materialGuid, const MaterialDocument& doc);

    // True when no texture decode is still pending for this material — every
    // referenced slot either bound synchronously or completed its async decode
    // + bind. Thumbnail consumers poll this to defer GPU readback.
    bool IsMaterialTextureBindingComplete(const GUID& matGuid) const;

    // The same question asked of the whole world rather than one material: how
    // many (material, slot) bindings are still waiting on a decode. Non-zero
    // means at least one material is rendering a bindless default in place of
    // an authored texture, so a frame captured now is a transient state that
    // looks exactly like a binding failure. A first bind tags the texture's
    // cook usage, so on a cold derived cache this stays non-zero for as long as
    // the block-compression encode takes — seconds to minutes, not frames.
    // Reported by get_editor_state and on every take_screenshot response, which
    // is what lets a capture harness tell "not bound yet" from "never binds".
    size_t PendingMaterialTextureBindCount() const;

    // Drop all reference tracking for a material being unregistered (installed
    // as the MaterialRegistry pre-unregister callback).
    void OnMaterialUnregistered(const GUID& matGuid);

    // ── Resolve .cube LUTs ────────────────────────────────────────────────
    // Resolve the LUT GPU textures + domain metadata for a post-process state.
    // Returns false only if the device is missing; out is always usable
    // (identity fallbacks).
    bool TryGetCubeLutGpuBinding(const PostProcessSettings& pp, CubeLutGpuBindingState& out) const;
    // Push-constant names for the LDR stack LUT controls (lutSize3d, lutFlags, ...).
    static bool TryWriteCubeLutPushMember(const std::string& name,
                                          const CubeLutGpuBindingState& lut,
                                          Rendering::NamedPushConstantWriter& pcw);
    // Drop a single LUT so it re-parses/re-uploads on next use.
    void EvictCubeLut(const GUID& lutGuid);

    // Test-only hooks for verifying hot-reload propagation in isolation.
    void TrackMaterialTextureRefForTesting(const GUID& matGuid, StringId slotName, const GUID& texGuid);
    size_t CountMaterialsReferencingTextureForTesting(const GUID& texGuid) const;
    bool MaterialTextureRefForTesting(const GUID& matGuid, StringId slotName, GUID& outTexGuid) const;

  private:
    friend struct TextureCoverageUploadTestAccess;
    // Widen the texture's stored cook usage to serve `usage`, needed by the material slot
    // (materialGuid, slotName) or by a declaration when materialGuid is null (see the
    // definition). Returns true when it wrote a new tag.
    bool EnsureTextureCookUsageTagged(const GUID& texGuid, TextureCookUsage usage, const GUID& materialGuid,
                                      StringId slotName);

    // Creates (or re-creates, after a device-lost rebuild) every device-scoped
    // resource: the active indexing mode's material plumbing plus the sampler
    // cache, default textures and identity LUTs both modes share. Called by
    // Initialize and ReprovisionAfterDeviceRebuild.
    bool CreateDeviceScopedResources();
    void CreateBindlessResources();
    void CreateDefaultTexturesAndLuts(IDevice* dev);
    void PublishBindlessDefaults();
    void InitializeMaterialBindingCache();

    // Single write point for a material texture slot: keeps the CPU handle, the
    // bindless index and the Classic bind group consistent. Every repair path
    // (upload flush, hot swap, eviction) routes through it so no mode can be
    // updated without the others.
    void ApplyMaterialSlotBinding(Material* mat, StringId slotName, TextureHandle tex,
                                  uint32_t bindlessIndex);

    uint32_t DefaultBindlessIndex(SlotDefaultTexture texture) const;
    TextureHandle DefaultTexture(SlotDefaultTexture texture) const;

    // Re-upload a texture and atomically swap every material slot bound to it,
    // then destroy the old texture (no default-texture gap, unlike Evict).
    // Render-thread only; called from FlushPendingUploads.
    void HotSwapTexture(const GUID& textureGuid);

    // Park a GPU texture this thread uploaded but lost the race to publish, for
    // the render thread to destroy in FlushPendingUploads. Callable from any
    // thread; see m_PendingGpuDiscards.
    void ParkDuplicateTexture(TextureHandle texture);

    // Every field the registered view is built from. A field the view depends on
    // but the key omits aliases two different views onto one slot — mipCount is
    // in here because a single-level view and a full-mip-chain view of the same
    // slice are not interchangeable (BindlessTextureDesc::mipCount).
    struct BindlessCacheKey
    {
        uint64_t textureId;
        uint32_t arraySlice = 0;
        uint32_t mipLevel = 0;
        uint32_t mipCount = 1;
        TextureAspect aspect = TextureAspect::Color;
        bool operator==(const BindlessCacheKey& o) const
        {
            return textureId == o.textureId && arraySlice == o.arraySlice &&
                   mipLevel == o.mipLevel && mipCount == o.mipCount && aspect == o.aspect;
        }
    };
    struct BindlessCacheKeyHash
    {
        size_t operator()(const BindlessCacheKey& k) const
        {
            size_t h = std::hash<uint64_t>{}(k.textureId);
            h ^= std::hash<uint32_t>{}((k.arraySlice << 16) | k.mipLevel) * 2246822519u;
            h ^= std::hash<uint32_t>{}(k.mipCount) * 0x85EBCA6Bu;
            h ^= std::hash<uint32_t>{}(static_cast<uint32_t>(k.aspect)) * 0x9E3779B1u;
            return h;
        }
    };
    struct BindlessCacheEntry
    {
        uint32_t bindlessIndex = 0;
        BindlessTextureHandle bindlessHandle = Rendering::kInvalidBindlessTexture;
        // Custom-view path: when slice/aspect/mip diverge from defaults a
        // TextureView is created and destroyed with the cache entry.
        TextureViewHandle bindlessView{};
    };
    BindlessCacheEntry RegisterTextureBindless(const BindlessTextureDesc& desc);

    // Record modelGuid -> derivedGuid (dedup within the parent) and bump the
    // shared-content refcount on first reference. See m_EmbeddedContentRefs.
    void TrackEmbeddedDerived(const GUID& modelGuid, const GUID& derivedGuid);

    // Record that `matGuid`'s slot takes image `imageIndex` of `modelGuid`'s
    // embedded images (see m_EmbeddedSlotsByMaterial).
    void TrackEmbeddedSlot(const GUID& matGuid, const GUID& modelGuid, StringId slotName, uint32_t imageIndex);

    // True when `mat`'s slot awaits a texture and no asset texture bind owns it
    // (m_MaterialTextureRefs), so an embedded image a reload supplies may take it.
    bool AwaitsEmbeddedImage(const Material& mat, const GUID& matGuid, StringId slotName) const;

    // Bind `mat`'s slot to `images[imageIndex]`, or to the slot's awaited
    // default when that image is missing or has no bytes. Returns whether the
    // image bound.
    bool BindEmbeddedImage(Material* mat, StringId slotName, const GUID& modelGuid,
                           uint32_t imageIndex, std::span<const EmbeddedImage> images);

    struct PendingTextureBind
    {
        GUID MaterialGuid;
        StringId SlotName{};
        GUID TextureGuid;
    };
    // The CPU half of a texture upload (TextureService.cpp): an adopted cooked container, or a
    // raw decode with its mip chain built. Built off the render thread for the async paths.
    struct PreparedTextureUpload;
    struct PendingGpuUpload
    {
        GUID TextureGuid;
        SharedPtr<Asset> TextureAsset; // keeps decoded pixels alive until upload runs
        uint32_t Epoch = 0;
        // Set when an upload job already ran the CPU half on a worker, on its own instance; null
        // runs it at the drain (an AssetManager instance, which only the render thread reads).
        std::shared_ptr<const PreparedTextureUpload> Prepared;
        // Log the upload's timing (an assign's upload); an AssetManager load's stays quiet.
        bool LogTiming = false;
    };

    // How a slot bind loads a texture that is not resident on the GPU.
    enum class TextureLoadRoute
    {
        // An AssetManager load: adopts a cooked artifact or cooks on a miss (registration).
        AssetManagerLoad,
        // An upload job: adopts a cooked artifact or decodes and builds mips, never cooks
        // (the editor's texture assign and a restored override), so the texture shows within
        // the decode instead of an encode. The cook for a newly tagged usage happens at the
        // next AssetManager load of the texture.
        UploadJob,
    };
    void BindMaterialTextureSlot(Material* mat, const GUID& matGuid, StringId slotName, const GUID& texGuid,
                                 TextureLoadRoute route);
    // Queues the (material, slot, texture) bind and runs the CPU half of the upload on a job on a
    // texture instance of its own; FlushPendingUploads submits it. A texture the AssetManager
    // already holds is queued for the drain instead, which prepares it on the render thread: its
    // instance is shared, and a reload swaps the payload in place. Thread-safe.
    void ScheduleTextureUploadJob(const GUID& texGuid, const GUID& matGuid, StringId slotName);
    void RunTextureUploadJob(const GUID& texGuid, uint32_t epoch, const std::shared_ptr<std::atomic<bool>>& cancel);
    // Cancels every upload job and waits for the running ones to finish (Shutdown, destructor).
    void DrainUploadJobs();
    // The CPU half: adopt `resident` (or load the texture, adopting a cooked artifact when one
    // matches) and, for a raw decode, expand, swizzle and build the mip chain. Only reads the
    // asset. Any thread for an instance of its own; an AssetManager instance (`resident`) only on
    // the render thread, where reloads swap its payload. Checks `cancel` (may be null) between
    // stages. Null when cancelled.
    static std::shared_ptr<const PreparedTextureUpload> PrepareUpload(const GUID& textureGuid,
                                                                      SharedPtr<TextureAsset> resident,
                                                                      const std::atomic<bool>* cancel,
                                                                      bool logTiming);
    // The upload half: create, upload and publish (or publish the refusal). Render thread, or an
    // extraction worker through GetOrUpload.
    Rendering::TextureHandle SubmitPreparedUpload(const GUID& textureGuid, const PreparedTextureUpload& prepared);

    struct CubeLutGpuCacheKey
    {
        GUID AssetGuid{};
        int32 TextureFormat = 0;
        bool operator==(const CubeLutGpuCacheKey& other) const
        {
            return AssetGuid == other.AssetGuid && TextureFormat == other.TextureFormat;
        }
    };
    struct CubeLutGpuCacheKeyHash
    {
        size_t operator()(const CubeLutGpuCacheKey& key) const
        {
            size_t h = std::hash<GUID>{}(key.AssetGuid);
            h ^= std::hash<int32>{}(key.TextureFormat) * 0x9E3779B1u;
            return h;
        }
    };

    // Record a material-references-texture edge for hot-reload propagation.
    // Caller must hold m_PendingTexturesMutex.
    void TrackMaterialTextureRefLocked(const GUID& matGuid, StringId slotName, const GUID& texGuid);
    void UntrackMaterialTextureRefLocked(const GUID& matGuid, StringId slotName);
    void UntrackAllMaterialTextureRefsLocked(const GUID& matGuid);
    void UntrackTextureFromAllMaterialsLocked(const GUID& texGuid);

    // Withdraw every in-flight texture load so no completion callback runs after
    // the queues it writes into are cleared.
    // Caller must hold m_PendingTexturesMutex.
    void WithdrawTextureLoadsLocked();

    IDevice* m_Device = nullptr;
    std::unique_ptr<CpuTextureSources> m_CpuTextureSources;
    MaterialRegistry* m_Materials = nullptr;

    std::unique_ptr<BindlessResourceManager> m_Bindless;
    // Atomic so an off-render-thread GetBindlessIndex consumer (the ECS extraction
    // wave runs TerrainExtraction and RenderExtraction concurrently on JobSystem
    // workers) reads a well-defined value while Reprovision flips it. It is
    // set false BEFORE the bindless teardown so a concurrent reader bails at its
    // lockless gate instead of dereferencing the half-reset manager, and true again
    // only after CreateBindlessAndDefaults has rebuilt the manager. Relaxed: it
    // gates access, it does not publish other state (m_BindlessMutex does that).
    std::atomic<bool> m_BindlessEnabled{false};
    MaterialIndexingMode m_IndexingMode = MaterialIndexingMode::Bindless;
    // Classic mode only; left uninitialized (and never consulted) on Bindless.
    MaterialBindingCache m_MaterialBindings;
    // Global bindless texture descriptor set (persistent, holds all registered textures).
    DescriptorSetHandle m_BindlessTextureSet{};
    // Bindless index cache keyed by (texture, array slice, mip level, aspect). The
    // sampler is NOT part of the key: post image/sampler split a texture occupies
    // one SAMPLED_IMAGE slot regardless of filter.
    std::unordered_map<BindlessCacheKey, BindlessCacheEntry, BindlessCacheKeyHash> m_BindlessTextureIndexCache;
    // Protects m_BindlessTextureIndexCache and m_SamplerCache (the manager has its own mutex).
    mutable std::mutex m_BindlessMutex;

    // Cached standard samplers, created lazily by GetSampler() from
    // ResolveSamplerPreset(preset, m_Profile). Never carry a mip bias: TAAU
    // texture-sharpness compensation is a per-view uniform applied at the material
    // sampling sites (adapter_forward's GE_MaterialTexture overloads), so these
    // descriptors stay valid across live render-scale changes.
    SamplerHandle m_SamplerCache[static_cast<size_t>(SamplerPreset::kCount)]{};
    // The renderer profile Initialize received; read-only afterwards, so record
    // workers read it without the mutex.
    Rendering::RendererProfile m_Profile{};

    // Duplicates produced when two threads registered the same key concurrently
    // (GetBindlessIndex / GetSampler / GetOrUpload). The losing thread is
    // routinely an ECS extraction worker, and the IDevice destroy paths are
    // render-thread-only, so the loser's objects are parked here and retired by
    // FlushPendingUploads.
    struct PendingGpuDiscard
    {
        BindlessTextureHandle Bindless = Rendering::kInvalidBindlessTexture;
        TextureViewHandle View{};
        SamplerHandle Sampler{};
        TextureHandle Texture{};
    };
    std::vector<PendingGpuDiscard> m_PendingGpuDiscards; // guarded by m_BindlessMutex

    // Thread that owns the device-touching paths (captured in Initialize, which
    // runs on the render thread). Debug asserts only: the registration entry
    // points are deliberately callable from ECS extraction workers, but the
    // retirement drain must land back on this thread.
    std::thread::id m_RenderThreadId{};

    // Default fallback textures (1x1 pixels, created during Initialize).
    TextureHandle m_DefaultWhiteTexture{};
    TextureHandle m_DefaultBlackTexture{};
    TextureHandle m_DefaultFlatNormalTexture{};
    TextureHandle m_DefaultLensFlareTexture{};
    TextureHandle m_DefaultWhiteArrayTexture{};
    TextureHandle m_DefaultBlackCubeTexture{};
    TextureHandle m_IdentityLut3D{};
    TextureHandle m_IdentityLut1DStrip{};
    // Bindless indices for default textures (valid when m_BindlessEnabled).
    uint32_t m_DefaultWhiteBindlessIndex = 0u;
    uint32_t m_DefaultBlackBindlessIndex = 0u;
    uint32_t m_DefaultFlatNormalBindlessIndex = 0u;

    // GPU-uploaded texture cache (GUID -> TextureHandle). Avoids re-uploading.
    // Guarded internally: GetOrUpload runs on ECS extraction workers while the
    // render thread uploads and evicts through the same entries.
    TextureGPUCache m_TextureGPUCache;
    mutable std::unordered_map<CubeLutGpuCacheKey, CubeLutGpuBindingState, CubeLutGpuCacheKeyHash> m_CubeLutGpuCache;
    mutable std::unordered_set<GUID> m_CubeLutWarnedFailureGuids;

    // CPU-decoded RGBA cache for embedded images, keyed by the linearity-free
    // content GUID (hash of the compressed bytes): the same source image used
    // as both sRGB albedo and linear data needs two GPU textures but only one
    // decode, and byte-identical images embedded by different models share one
    // entry (asset packs embed the same atlas into every FBX).
    std::unordered_map<GUID, GameEngine::DecodedImage> m_EmbeddedDecodeCache;
    // Parent ModelAsset GUID -> derived content GUIDs of embedded images it
    // references; ReloadEmbeddedForModel walks this (the parent GUID itself
    // never keys m_TextureGPUCache).
    std::unordered_map<GUID, std::vector<GUID>> m_DerivedTextureGuidsByParent;
    // Distinct-parent refcount per derived content GUID: content-keyed entries
    // are shared across models, so eviction only destroys with the last parent.
    std::unordered_map<GUID, uint32_t> m_EmbeddedContentRefs;
    // Material GUID -> the model whose embedded images ResolveEmbeddedTextures
    // bound on it, and which image each slot took: what a model reload rebinds
    // before it releases the old textures. Used on the render thread; kept
    // under m_PendingTexturesMutex beside the material-texture refs that
    // OnMaterialUnregistered already updates under that lock.
    struct EmbeddedSlot
    {
        StringId SlotName{};
        uint32_t ImageIndex = 0;
    };
    struct EmbeddedMaterialSlots
    {
        GUID ModelGuid;
        std::vector<EmbeddedSlot> Slots;
    };
    std::unordered_map<GUID, EmbeddedMaterialSlots> m_EmbeddedSlotsByMaterial;

    // Textures whose conflicting cook-usage bindings were already reported. A conflict repeats
    // for as long as the content does (every material load binds again, and the ocean declares
    // its textures every frame), so each texture warns once per service lifetime. Written from
    // the main thread and from ECS extraction workers (DeclareTextureClassification).
    std::unordered_set<GUID> m_ReportedUsageConflicts;
    std::mutex m_ReportedUsageConflictsMutex;

    // Async texture loading pipeline:
    //  1. ScheduleMaterialTextureLoad records the (material, slot, texture)
    //     bind and calls AssetManager::LoadAsset (deduped by
    //     m_TextureLoadsKicked).
    //  2. The worker-thread completion callback enqueues a PendingGpuUpload,
    //     holding the loaded asset alive via shared_ptr.
    //  3. FlushPendingUploads (render thread) drains the queue, creates the
    //     GPU texture, then binds anything now resolvable.
    // m_PendingTexturesMutex guards all queues + ref maps below; mutable so
    // const queries (IsMaterialTextureBindingComplete) can lock.
    mutable std::mutex m_PendingTexturesMutex;
    std::vector<PendingGpuUpload> m_PendingGpuUploads;
    std::vector<PendingTextureBind> m_PendingTextureBinds;
    // GUIDs requested for re-upload from another thread; drained + hot-swapped
    // on the render thread at the start of FlushPendingUploads.
    std::vector<GUID> m_PendingTextureReupload;
    // GUIDs with an in-flight AssetManager load (prevents redundant LoadAsset
    // calls when many materials reference the same texture).
    std::unordered_set<GUID> m_TextureLoadsKicked;
    // Live AssetLoadHandle per in-flight load — kept so teardown and eviction can
    // withdraw the request before its completion callback runs.
    std::unordered_map<GUID, AssetLoadHandle> m_TextureLoadHandles;
    // Per-GUID load epoch. Evict bumps this on hot-reload; the upload drain
    // discards results whose captured epoch no longer matches.
    std::unordered_map<GUID, uint32_t> m_TextureEpochByGuid;
    // Cancel flag of each upload job in flight (ScheduleTextureUploadJob), set by Evict,
    // CancelPendingLoads and Shutdown; the job checks it between its stages.
    std::unordered_map<GUID, std::shared_ptr<std::atomic<bool>>> m_UploadJobCancels;
    // Upload jobs still running: DrainUploadJobs waits on it so no job outlives the service.
    JobSystem::JobCounter m_UploadJobCounter;
    bool m_UploadJobsEverSubmitted = false; // the pool is touched only once a job ran
    bool m_UploadJobsStopped = false;       // set by DrainUploadJobs; no new job starts

    // The cutoff-independent alpha fact behind AlphaIsUniformlyOpaque, one decode per texture per
    // load epoch. Stamped with the epoch it was probed at rather than invalidated by Evict, so a
    // hot-reload needs no second bookkeeping path: a stale stamp simply re-probes.
    struct AlphaOpacityEntry
    {
        uint32_t Epoch = 0;
        bool UniformlyOpaque = false;
    };
    std::unordered_map<GUID, AlphaOpacityEntry> m_AlphaOpacityCache;
    // Its own mutex on purpose: a probe DECODES, and holding m_PendingTexturesMutex across that
    // would stall the render thread's upload drain behind an extraction worker's file read.
    std::mutex m_AlphaOpacityMutex;

    // Material<->texture reference tracking for hot-reload propagation:
    //   m_MaterialTextureRefs[matGuid][slotName] = texGuid
    //   m_TextureToMaterialBindings[texGuid] = [(matGuid, slotName), ...]
    // The reverse map lets Evict find every (material, slot) using a reloaded
    // texture in O(refs); each bind is cleared to the bindless default and
    // re-queued so FlushPendingUploads rebinds once the new texture lands.
    std::unordered_map<GUID, std::unordered_map<StringId, GUID>> m_MaterialTextureRefs;
    std::unordered_map<GUID, std::vector<std::pair<GUID, StringId>>> m_TextureToMaterialBindings;
};

} // namespace Engine::Renderer
} // namespace GameEngine
