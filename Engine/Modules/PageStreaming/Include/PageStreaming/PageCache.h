#pragma once

#include "PageStreaming/PageAddress.h"
#include "PageStreaming/ResidentSlotPool.h"

#include <cstddef>
#include <span>
#include <vector>

namespace GameEngine::PageStreaming
{

/// Frames a resident page stays after the residency stops wanting it, so a camera hovering at a
/// ring boundary does not release and re-assign the same pages every frame (the unload hysteresis
/// the tile streaming had). Half a second at 60 frames a second.
inline constexpr uint32 kPageReleaseHysteresisFrames = 30;

/// Hash of a page address for the cache's maps.
struct PageAddressHash
{
    std::size_t operator()(const PageAddress& address) const noexcept;
};

/// A page of a cache shared by several streams (each one terrain's field): the stream and the
/// page's address in that stream's pyramid.
struct CachedPage
{
    uint32 Stream = 0;
    PageAddress Address;

    CachedPage Parent() const { return CachedPage{Stream, Address.Parent()}; }

    bool operator==(const CachedPage&) const = default;
};

/// Hash of a cached page for the cache's maps.
struct CachedPageHash
{
    std::size_t operator()(const CachedPage& page) const noexcept;
};

/// A page the residency wants this frame: the level rule's request, the residency manager's load
/// order and the cache allocator's assignment order.
struct PageWant
{
    PageAddress Address;
    float32 Distance = 0.0f; ///< meters from the nearest camera (nearer pages load and assign first)
    bool Pinned = false;     ///< a pinned level: kept whatever the camera does
    uint32 Stream = 0;       ///< the stream the page belongs to (the residency manager sets it)
    bool Top = false;        ///< its pyramid's top level, with no parent to wait for or fade from
                             ///< (the residency manager sets it)

    CachedPage Page() const { return CachedPage{Stream, Address}; }

    bool operator==(const PageWant&) const = default;
};

/// A slot whose texels must be written this frame: a page just assigned to it, or a resident page
/// whose content changed (PageCache::Rewrite).
struct PageUploadRequest
{
    CachedPage Page;
    uint32 Slot = 0;
};

/// The physical-cache allocator of one field, shared by every stream of that field (each terrain's
/// pages told apart by their stream): which page each slot of the GPU cache texture holds. Each
/// frame it takes the pages the residency wants whose samples are loaded or already resident, and
/// keeps the cache to them under three rules:
///
/// - Parent first: a page is assigned only while its parent is resident, and released only after
///   every resident child, so the fallback walk and the level blend always find the parent, and the
///   pinned levels end every walk. A full cache therefore degrades by level (the finest wanted pages
///   go without and resolve to their parent), never by holes.
/// - Hysteresis and priority: a page no longer wanted stays kPageReleaseHysteresisFrames frames
///   before its slot is released, so a camera hovering at a ring's edge does not churn the cache.
///   When the cache is full, a wanted page takes the slot of the farthest settled page of its level
///   or finer that has no resident child and is farther from the camera (or no longer wanted), so
///   which pages a full cache keeps follows the camera, not the order they arrived in.
/// - Arrival fade: a page assigned this frame fades in from its parent over fadeSeconds, so its
///   arrival never pops; a released page falls back to its parent at once (its slot is quarantined
///   for the frames in flight).
///
/// Every output is edge-triggered: a parked camera whose fades have settled assigns, releases,
/// uploads and transitions nothing, and allocates nothing (the quiescence law).
class PageCache
{
public:
    /// (Re)configure for `slotCount` slots, the renderer's frames in flight and the arrival fade
    /// window (0 = no fade). Clears the cache.
    void Configure(uint32 slotCount, uint32 framesInFlight, float32 fadeSeconds);

    /// One frame: release the pages unwanted for longer than the hysteresis (finest first, none
    /// with a resident child), advance the arrival fades by deltaSeconds, then assign up to
    /// maxAssigns of the wanted pages that are not resident, coarsest level first and nearest first
    /// within a level, each only when its parent is resident (or it is its pyramid's top level).
    void Update(uint64 frameIndex, float32 deltaSeconds, std::span<const PageWant> wanted, uint32 maxAssigns);

    /// Releases every resident page of `stream` at once (a stream restarted or forgotten): its
    /// slots are quarantined. No transitions are reported: the stream's owner drops its whole page
    /// table with it.
    void ReleaseStream(uint32 stream);

    /// The slot holding `page`, or kNoResidentSlot.
    uint32 SlotOf(const CachedPage& page) const { return m_Pool.SlotOf(page); }
    /// The arrival fade of a resident page: 0 just assigned, 1 settled.
    float32 FadeOf(const CachedPage& page) const;
    uint32 SlotCount() const { return m_Pool.SlotCount(); }

    /// Writes resident page `page`'s slot again this frame because its content changed (a modifier
    /// edit under it): it is listed in Uploads() and Rewrites() after this frame's Update, its slot,
    /// fade and children unchanged. Returns false when the page is not resident.
    bool Rewrite(const CachedPage& page);

    /// Pages assigned or rewritten this frame, whose slot texels must be written.
    const std::vector<PageUploadRequest>& Uploads() const { return m_Uploads; }
    /// Resident pages rewritten this frame (Rewrite): the height under them changed in place.
    const std::vector<CachedPage>& Rewrites() const { return m_Rewrites; }
    /// Pages whose residency changed this frame (assigned or released): the height under them
    /// changed source, so the CBT re-evaluates their footprint.
    const std::vector<CachedPage>& Transitions() const { return m_Transitions; }

    uint32 ResidentCount() const { return m_Pool.ResidentCount(); }
    uint32 AssignsThisFrame() const { return static_cast<uint32>(m_Uploads.size() - m_Rewrites.size()); }
    uint32 ReleasesThisFrame() const { return m_Releases; }
    /// Every resident page, finest level first.
    const std::vector<CachedPage>& Resident() const { return m_Resident; }
    /// Resident pages whose arrival fade changed this frame, the frame it settles to 1 included.
    const std::vector<CachedPage>& FadeChanges() const { return m_FadeChanges; }
    /// Pages whose arrival fade advanced this frame; zero once every fade has settled.
    uint32 ActiveFadesThisFrame() const { return m_ActiveFades; }
    /// Wanted, loaded pages left without a slot this frame (the cache full or the cap reached).
    uint32 DeferredThisFrame() const { return m_Deferred; }

private:
    using Pool = ResidentSlotPool<CachedPage, CachedPageHash>;

    // What the cache knows of the page in a slot.
    struct SlotPage
    {
        uint64 LastWantedFrame = 0;
        float32 Distance = 0.0f;
        float32 Fade = 1.0f;
        uint32 ResidentChildren = 0;
    };

    void ReleaseStale(uint64 frameIndex);
    void Release(const CachedPage& page);
    void TickFades(float32 deltaSeconds);
    void SortCandidates(std::span<const PageWant> wanted);
    bool EvictFor(const PageWant& want, uint64 frameIndex);
    void Assign(const PageWant& want, uint32 slot, uint64 frameIndex);

    Pool m_Pool;
    std::vector<SlotPage> m_Slots;      // per slot, meaningful while the slot is live
    std::vector<CachedPage> m_Resident; // every resident page, finest level first
    std::vector<PageWant> m_LastWanted; // the want set the candidate order was sorted for
    std::vector<uint32> m_Order;        // indices of the want set, coarsest level then nearest first
    std::vector<PageUploadRequest> m_Uploads;
    std::vector<CachedPage> m_Transitions;
    std::vector<CachedPage> m_Rewrites;
    std::vector<CachedPage> m_FadeChanges;
    float32 m_FadeSeconds = 0.0f;
    uint32 m_Releases = 0;
    uint32 m_ActiveFades = 0;
    uint32 m_Deferred = 0;
};

} // namespace GameEngine::PageStreaming
