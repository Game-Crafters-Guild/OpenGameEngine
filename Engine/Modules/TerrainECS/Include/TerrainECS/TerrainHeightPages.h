#pragma once

#include "PageStreaming/HeightPageOverlay.h"
#include "PageStreaming/PageResidency.h"
#include "TerrainECS/PageLevelRule.h"
#include "TerrainECS/PagedHeightSampler.h"
#include "Mathematics/Vector3.h"
#include "Types/Types.h"

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace JobSystem
{
class WorkStealingThreadPool;
}

namespace GameEngine
{
class AssetIOService;
}

// The height pages of every paged terrain of the process, on the main thread: the one residency
// manager (design D-c: one per process, owned by TerrainService), each terrain's stream, level rule
// and page table, and what the GPU side takes each frame (the slots to upload, the page-table words,
// the terrain-UV rects whose height changed source).
namespace GameEngine::TerrainECS
{

/// What a platform spends on height pages (design D5): the height cache's slots, the largest
/// page table one GPU page-table ring slot may hold, and the largest modifier overlay
/// (PageStreaming::HeightPageOverlay, in system memory) a terrain may hold. The CBT sizes its ring
/// slots to the bound terrain's table, up to the table cap (CBTResources::ProvisionPageTableWords),
/// so the cap costs memory only where a terrain needs it; the overlay is built only for a terrain
/// that pages, over the rect its height modifiers reach. GE_TERRAIN_PAGE_SLOTS overrides the slots
/// for soaks.
struct HeightPageProfile
{
    uint32 Slots = 0;
    uint32 TableWordCap = 0;
    uint64 OverlayByteCap = 0;
};

/// Desktop: 512 slots (33 MiB); tables up to 2^21 words (8 MiB a ring slot), so the 60 x 15 km
/// terrain at 0.25 m (1,174,877 words) pages; an overlay up to 512 MiB, about 6 km^2 of
/// modifiers on a 0.25 m terrain (about 5.3 B per level-0 sample), a few percent of a 16 GiB
/// machine's memory: a 1 km^2 stamp at 0.25 m holds about 82 MiB.
inline constexpr HeightPageProfile kDesktopHeightPages{512u, 1u << 21, 512ull << 20};
/// Steam Deck: half the desktop slots (256, 16.5 MiB) and a quarter of its table cap (2^19 words,
/// 2 MiB a ring slot): about 24 x 6 km at 0.5 m, or 60 x 15 km at 1 m; a quarter of the desktop
/// overlay (128 MiB of its 16 GiB shared with the GPU), which still holds 1 km^2 at 0.25 m.
inline constexpr HeightPageProfile kSteamDeckHeightPages{256u, 1u << 19, 128ull << 20};

/// The profile of the platform this build targets.
HeightPageProfile PlatformHeightPageProfile();

/// The field id of the height pages in the residency manager.
inline constexpr uint32 kHeightPageField = 0u;

/// The words of a field's GPU page table (CBTLayout.h binding-22 layout) for a field of
/// samplesX x samplesZ level-0 samples and a cache of slotCount slots.
uint64 PageTableWordCount(uint32 samplesX, uint32 samplesZ, uint32 slotCount);

/// Why a terrain of samplesX x samplesZ level-0 samples does not page under `profile` (its page
/// table is over the profile's cap), worded with the fix, naming the terrain `name`; empty when
/// it fits.
std::string PageTableRefusal(std::string_view name, uint32 samplesX, uint32 samplesZ, const HeightPageProfile& profile);

/// Why a terrain whose modifier overlay needs `overlayBytes` does not page under `profile` (over
/// the profile's overlay cap), worded with the fix, naming the terrain `name`; empty when it fits.
std::string OverlayRefusal(std::string_view name, uint64 overlayBytes, const HeightPageProfile& profile);

/// The modifier overlay a terrain may hold, set on its TiledTerrainData by the height page driver
/// each frame (TerrainHeightPageDriver).
struct HeightPageOverlayAllowance
{
    uint64 ByteCap = 0;  ///< the profile's overlay cap when the terrain would page but for its modifiers, else 0
    bool OffGrid = false; ///< its pages are not on the tile lattice the overlay is baked on
};

/// The overlay rule: a terrain that would page were no modifier pending (`wouldPage`) may hold an
/// overlay up to the profile's cap. The overlay lies on the tile lattice (latticeX x latticeZ:
/// tiles x (resolution - 1) + 1 per axis); a cooked store on another grid (sourceX x sourceZ, every
/// power-of-two raw heightmap) cannot take it, so it is OffGrid: it pages while no height modifier
/// reaches it, and keeps its height texture (OverlayGridRefusal) once one does.
HeightPageOverlayAllowance PageOverlayAllowance(bool wouldPage, uint32 sourceX, uint32 sourceZ, uint32 latticeX,
                                                uint32 latticeZ, const HeightPageProfile& profile);

/// Why a terrain whose cooked store (storeX x storeZ samples) is off its tile lattice (latticeX x
/// latticeZ) does not page while a height modifier reaches it, worded with the fix, naming the
/// terrain `name`.
std::string OverlayGridRefusal(std::string_view name, uint32 storeX, uint32 storeZ, uint32 latticeX, uint32 latticeZ);

/// Packs `table` into the GPU page-table words (CBTLayout.h binding-22 layout): the field's shape,
/// its levels, every slot's arrival fade and the entries.
void PackPageTableWords(const PageTable& table, const PageCacheGeometry& geometry, std::vector<uint32>& out);

/// One page's samples for its cache slot, uploaded by the GPU side this frame.
struct HeightPageUpload
{
    uint32 Slot = 0;
    std::vector<float32> Samples; ///< kPageSampleCount, row-major, apron included
};

/// A terrain-UV rect whose height changed source this frame: the CBT re-evaluates the bisectors
/// over it.
struct HeightPageDirtyRect
{
    uint32 Terrain = 0;
    uint32 TerrainGeneration = 0;
    float32 MinU = 0.0f;
    float32 MinV = 0.0f;
    float32 MaxU = 0.0f;
    float32 MaxV = 0.0f;
};

/// What a paged terrain asks for this frame.
struct PagedTerrainRequest
{
    uint32 Terrain = 0;           ///< the terrain's render handle (TerrainHandle::Index, ::Generation)
    uint32 TerrainGeneration = 0;
    uint64 SourceIdentity = 0;  ///< changes when the source or the pyramid changes (restarts the stream)
    std::string_view Name;      ///< the terrain entity's name, for the log lines (empty: its render index)
    PageStreaming::PageSource Source;
    PageRequestTerrain Shape;   ///< its pyramid in world meters; CacheSlots is set by TerrainHeightPages
    /// What the terrain's modifiers add to its source's pages (null: nothing), applied to every page
    /// as it is uploaded. A new version rewrites the resident pages that read OverlayChanged when
    /// the pages hold version OverlayChangedSince, else every page either overlay reaches.
    std::shared_ptr<const PageStreaming::HeightPageOverlay> Overlay;
    uint64 OverlayVersion = 0;
    PageStreaming::PageSampleRect OverlayChanged; ///< level-0 samples the version changed from OverlayChangedSince
    uint64 OverlayChangedSince = 0;
};

class TerrainHeightPages
{
public:
    TerrainHeightPages(JobSystem::WorkStealingThreadPool* pool, AssetIOService* io, const HeightPageProfile& profile);

    /// The pages `request`'s terrain needs for `cameras` this frame. A terrain not requested
    /// between two Updates is forgotten at the second.
    void Request(const PagedTerrainRequest& request, std::span<const Mathematics::Vector3> cameras,
                 const PageLevelView& view);

    /// One frame: forgets the terrains not requested since the last Update, runs the residency,
    /// brings each terrain's page table up to date and gathers the uploads and the dirty rects.
    void Update(uint64 frameIndex, float32 deltaSeconds, bool teleport, uint32 framesInFlight);

    /// True when `terrain` is not paged yet or its source identity differs: its next Request must
    /// carry the source (a terrain's source is built only then).
    bool NeedsSource(uint32 terrain, uint64 sourceIdentity) const;

    /// When `deviceEpoch` differs from the last one seen (the GPU cache was lost with its device),
    /// empties the cache and every page table, so every page is assigned and uploaded again.
    void RestartOnDeviceEpoch(uint64 deviceEpoch);

    /// Logs `refusal` (PageTableRefusal) once for `terrain` until the terrain pages or its refusal
    /// changes: the terrain keeps its height texture meanwhile.
    void ReportRefusal(uint32 terrain, const std::string& refusal);

    uint32 SlotCount() const { return m_SlotCount; }
    const HeightPageProfile& Profile() const { return m_Profile; }
    const PageCacheGeometry& Geometry() const { return m_Geometry; }
    /// This frame's slot uploads. The GPU side takes them (moves them out) after every Update, the
    /// frame its page table is published: the next Update clears what was not taken.
    std::vector<HeightPageUpload>& Uploads() { return m_Uploads; }
    /// The rects whose height changed source since the caller last drained them (cleared by the
    /// caller): page assigns, releases, rewrites and arrival-fade steps, and whole terrains switching
    /// between their pages and their texture.
    std::vector<HeightPageDirtyRect>& DirtyRects() { return m_DirtyRects; }
    /// The page-table words of a paged terrain and their version (bumped on every change), or null.
    const std::vector<uint32>* TableWords(uint32 terrain, uint64& version) const;
    /// Every paged terrain (render handle indices), in no order.
    void PagedTerrains(std::vector<uint32>& out) const;
    const PageStreaming::PageResidencyManager& Residency() const { return m_Residency; }

private:
    struct PagedTerrain
    {
        uint32 Stream = 0;
        uint32 Generation = 0;
        uint64 Identity = 0;
        std::string Label;        ///< how the log lines name it: the entity's name, or its render index
        PageRequestTerrain Shape; ///< the placement its requester is configured with
        HeightPageRequester Requester;
        PageTable Table;
        std::shared_ptr<const PageStreaming::HeightPageOverlay> Overlay;
        uint64 OverlayVersion = 0;
        std::vector<uint32> Words;
        uint64 Version = 0;
        bool Requested = false;
        std::vector<uint32> WantedEntries; ///< GE_CBT_VALIDATE: this frame's wanted pages, as table entry indices
        std::string LoggedResidency;       ///< GE_CBT_VALIDATE: the residency line last logged
    };

    void Configure(PagedTerrain& terrain, const PagedTerrainRequest& request);
    void CollectStreamChanges(uint32 terrainKey, PagedTerrain& terrain);
    void LogResidencyChange(PagedTerrain& terrain);
    void AddWholeTerrainDirty(uint32 terrainKey, const PagedTerrain& terrain);
    void AddDirtyPage(uint32 terrainKey, const PagedTerrain& terrain, const PageStreaming::PageAddress& address,
                      HeightPageDirtyRect& rect, bool& any) const;

    PageStreaming::PageResidencyManager m_Residency;
    HeightPageProfile m_Profile;
    uint32 m_SlotCount = 0;
    PageCacheGeometry m_Geometry;
    uint32 m_FramesInFlight = 0;
    uint64 m_DeviceEpoch = 0;
    uint32 m_NextStream = 1;
    // Every table version comes from this one counter, so a terrain paged again at a render index
    // never repeats a version the CBT's per-ring-slot upload check has seen at that index.
    uint64 m_TableVersion = 0;
    std::unordered_map<uint32, PagedTerrain> m_Terrains;
    std::vector<PageStreaming::PageWant> m_Wants; // reused per request
    std::vector<HeightPageUpload> m_Uploads;
    std::vector<HeightPageDirtyRect> m_DirtyRects;
    std::vector<uint32> m_Forgotten; // reused per Update
    std::unordered_map<uint32, std::string> m_Refusals; // per terrain, the refusal last logged
};

} // namespace GameEngine::TerrainECS
