#pragma once

// Cross-frame persistent resource pool (UE FRDGPooled* model), over the engine's
// real IDevice. History / hi-Z / shadow-atlas / readback resources live HERE, not
// as retained graph state; the graph imports them each frame via GetOrCreate.
// Keyed by a stable name + desc:
//   * same desc      -> reuse the handle (carries its ResourceState across frames)
//   * changed desc   -> defer the old handle for destruction, allocate anew
//   * unused N frames -> aged out by TickPoolElements (how an invisible view's
//                        per-view resources reclaim themselves)
// Name-keyed maps are fine here: touched a handful of times per frame, never in
// the per-pass hot path.
//
// NAMESPACING CONTRACT: names are a global keyspace. Per-view resources MUST be
// namespaced by their view (e.g. "SceneView.TAA.History", "GameView.TAA.History")
// — two views importing a bare "TAA.History" would silently share one physical
// (or realloc-thrash on desc mismatch). The Stage-2 per-view integration derives
// the prefix from the view automatically.

#include "Rendering/Core/Device.h"
#include "Types/Types.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine::Rendering::RenderGraph
{

class RGResourcePool
{
  public:
    explicit RGResourcePool(IDevice* device) : m_Device(device) {}
    ~RGResourcePool();
    RGResourcePool(const RGResourcePool&) = delete;
    RGResourcePool& operator=(const RGResourcePool&) = delete;

    // outNeedsFreshInit (optional) reports that the physical is fresh — first
    // use, desc-change realloc, the usage-widening realloc RequireTextureUsage
    // triggers, idle age-out, or device rebuild — so no frame this consumer
    // wrote is still resident. Pool memory is never zeroed and is
    // recycled from evicted entries, so a consumer that trusts prior contents
    // (cross-frame history) MUST treat true as "history absent". The arm
    // persists across declared-but-abandoned frames: only MarkTextureInitialized,
    // which RGFrame calls after a frame that declared the write actually
    // executed, clears it.
    TextureHandle GetOrCreateTexture(const std::string& name, const TextureDesc& desc,
                                     uint64_t frameIndex, bool* outNeedsFreshInit = nullptr);
    void MarkTextureInitialized(const std::string& name)
    {
        if (auto it = m_Tex.find(name); it != m_Tex.end())
            it->second.NeedsFreshInit = false;
    }
    // outNeedsZeroInit (optional) reports that the physical was created (first
    // use OR desc-change realloc) and no EXECUTED frame has zero-filled it yet.
    // Pool memory is never zero-initialized, so a caller that trusts prior
    // contents must schedule a clear whenever this is true. The arm persists
    // across declared-but-abandoned frames (swapchain-acquire failure, HDR
    // recheck): it is cleared only by MarkBufferZeroFilled, which RGFrame calls
    // after a frame that declared the fill actually executed.
    BufferHandle GetOrCreateBuffer(const std::string& name, const BufferDesc& desc, uint64_t frameIndex,
                                   bool* outNeedsZeroInit = nullptr);
    void MarkBufferZeroFilled(const std::string& name)
    {
        if (auto it = m_Buf.find(name); it != m_Buf.end())
            it->second.NeedsZeroInit = false;
    }

    // ResourceState carried across frames so barrier generation starts from the
    // real current state rather than assuming Undefined (no execute-time staleness).
    ResourceState GetState(const std::string& name) const;
    void SetState(const std::string& name, ResourceState state);

    // Usage the named texture's physical must carry beyond the desc its importer
    // states, from its next GetOrCreateTexture on. An import's physical is
    // realized before any pass declares, so the transfer usage a frame derives
    // from its declared copies can only reach the physical one materialization
    // late: one realloc (the freshness arm reports it), after which the widened
    // desc matches the importer's unwidened one every frame.
    void RequireTextureUsage(const std::string& name, TextureUsage usage);

    // Single-mip view of a pooled texture (mip-chain compute writes bind one
    // level at a time) — 2D for a single-layer image, 2D array covering every
    // layer for an array image. The POOL owns these because it owns the image: a view is
    // released on the same transition that releases its texture — desc-change
    // realloc, idle age-out, teardown — so a cached view can never outlive the
    // image it was created from, and no consumer holds device-object lifetime
    // across frames. Consumers ask per use; the handle is stable for as long as
    // the physical is. Returns an invalid handle for a texture this pool does
    // not currently own, since it could not guarantee that lifetime.
    TextureViewHandle GetOrCreateMipView(TextureHandle texture, uint32_t mip);

    // Age out entries unused for more than maxIdleFrames and flush deferred
    // destroys whose in-flight retire window has elapsed. Call once per frame.
    void TickPoolElements(uint64_t frameIndex, uint64_t maxIdleFrames, uint64_t framesInFlight);

    // Q6 slice 4 (§8-completion): an in-place device rebuild freed every pooled
    // texture/buffer, but the by-name cache still holds their handles — so on a desc
    // MATCH GetOrCreateTexture early-returns the DEAD handle instead of recreating
    // (the world pass's depth attachment came back with a null image view,
    // and the MSAA color target's queried sample count no longer matched the pipeline,
    // aborting the first resumed frame). Forget every cached + deferred handle so the
    // next GetOrCreate recreates it fresh. (Skipping DestroyTexture/Buffer here is not
    // about avoiding a double-free — a stale Destroy* is a generational no-op — it is
    // simply that the physical is already gone; the fix is not USING the stale handle.)
    // Entry-owned mip views go with their entries for the same reason.
    void DropAllAfterDeviceRebuild()
    {
        m_Tex.clear();
        m_TexNameByHandle.clear();
        m_Buf.clear();
        m_DeferredTex.clear();
        m_DeferredBuf.clear();
    }

    size_t Size() const { return m_Tex.size() + m_Buf.size(); }
    uint64_t BytesInPool() const;
    size_t DeferredCount() const { return m_DeferredTex.size() + m_DeferredBuf.size(); }

    // Introspection (VRAM panel / MCP resource listing): one row per entry.
    // Name pointers are valid only during the callback.
    struct EntryInfo
    {
        const char* Name;
        bool IsTexture;
        uint64_t Bytes;
        uint64_t LastUsedFrame;
        ResourceState State; // textures only; Undefined for buffers
    };
    template <class Fn>
    void ForEachEntry(Fn&& fn) const
    {
        for (const auto& [name, e] : m_Tex)
            fn(EntryInfo{name.c_str(), true, e.Bytes, e.LastUsedFrame, e.State});
        for (const auto& [name, e] : m_Buf)
            fn(EntryInfo{name.c_str(), false, e.Bytes, e.LastUsedFrame, ResourceState::Undefined});
    }

  private:
    struct TexEntry
    {
        TextureHandle Handle;
        TextureDesc Desc;
        uint64_t Bytes;
        uint64_t LastUsedFrame;
        ResourceState State = ResourceState::Undefined;
        // Fresh physical no executed frame has written yet.
        bool NeedsFreshInit = false;
        // Usage bits the physical carries beyond the importer's desc: transfer
        // usage a frame derived from its declared copies, and requirements
        // stated for graph-external consumers. Folded into the importer's desc
        // before the identity check, so a widened entry stays a match for the
        // desc the importer restates every frame. Survives a resize realloc;
        // forgotten with the entry (age-out, device rebuild) and re-learned
        // by the next frame that copies or requires it.
        uint32_t RequiredUsage = 0;
        // Single-mip views of Handle, indexed by mip. Owned here so they are
        // released by the same code path that releases Handle.
        std::vector<TextureViewHandle> MipViews;
    };
    struct BufEntry
    {
        BufferHandle Handle;
        BufferDesc Desc;
        uint64_t Bytes;
        uint64_t LastUsedFrame;
        ResourceState State = ResourceState::Undefined;
        // Fresh physical whose contents no executed frame has zero-filled yet.
        bool NeedsZeroInit = false;
    };
    template <class H>
    struct Deferred
    {
        H Handle;
        uint64_t RetireFrame;
    };
    // A realloc'd texture carries its views into the retire window: destroying
    // them earlier would strand in-flight frames that still reference them,
    // destroying them later would outlive the image.
    struct DeferredTexture
    {
        TextureHandle Handle;
        uint64_t RetireFrame;
        std::vector<TextureViewHandle> MipViews;
    };

    // Views first, then the image they were created from.
    void ReleaseTextureAndViews(TextureHandle handle, std::vector<TextureViewHandle>& views);

    IDevice* m_Device = nullptr;
    std::unordered_map<std::string, TexEntry> m_Tex;
    // Reverse index onto m_Tex's key, so the mip-view path resolves a physical
    // handle without scanning: the pool is name-keyed, but consumers arrive
    // holding only the handle the graph already resolved for them.
    //
    // Invariant: an entry's own Handle stays the authority. Every lookup
    // re-checks it, so an entry reached through a stale key is refused rather
    // than served — a handle that is no longer the live physical (realloc'd,
    // aged out, foreign) can never be vended a view.
    FastHashMap<TextureHandle, std::string> m_TexNameByHandle;
    std::unordered_map<std::string, BufEntry> m_Buf;
    std::vector<DeferredTexture> m_DeferredTex;
    std::vector<Deferred<BufferHandle>> m_DeferredBuf;
};

} // namespace GameEngine::Rendering::RenderGraph
