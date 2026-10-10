#include "TerrainECS/TerrainHeightPages.h"

#include "TerrainECS/TerrainAtlas.h"
#include "CBTTerrain/CBTLayout.h"
#include "PageStreaming/PageCache.h"
#include "PageStreaming/PageTableEntry.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <format>

namespace GameEngine::TerrainECS
{

namespace
{

// The last level-0 sample a page of `level` at index `page` reads along an axis of `samples`,
// over (samples - 1): the page's footprint in terrain UV, one level-`level` sample of margin on
// each side for the bilinear tap that straddles its edge.
void PageAxisUV(uint32 page, uint32 level, uint32 samples, float32& outMin, float32& outMax)
{
    const float32 span = static_cast<float32>(PageStreaming::kPageOwnedSamples << level);
    const float32 margin = static_cast<float32>(1u << level);
    const float32 inv = samples > 1u ? 1.0f / static_cast<float32>(samples - 1u) : 0.0f;
    outMin = std::max(0.0f, (static_cast<float32>(page) * span - margin) * inv);
    outMax = std::min(1.0f, (static_cast<float32>(page + 1u) * span + margin) * inv);
}

// True when the requester of a terrain configured with `a` asks for the same pages as one
// configured with `b`: the same pyramid placed at the same world position and height range.
bool SamePlacement(const PageRequestTerrain& a, const PageRequestTerrain& b)
{
    return a.OriginX == b.OriginX && a.OriginZ == b.OriginZ && a.Level0TexelX == b.Level0TexelX &&
           a.Level0TexelZ == b.Level0TexelZ && a.SamplesX == b.SamplesX && a.SamplesZ == b.SamplesZ &&
           a.MinHeight == b.MinHeight && a.MaxHeight == b.MaxHeight;
}

// GE_CBT_VALIDATE: log each paged terrain's wanted and resident pages per level whenever they change.
bool LogResidency()
{
    static const bool kLogResidency = std::getenv("GE_CBT_VALIDATE") != nullptr;
    return kLogResidency;
}

// The pages of the residency line's address lists, at most; the rest are counted.
constexpr std::size_t kLoggedAddressesPerList = 32;

// The level, page X and page Z of table entry index `entry`.
void EntryAddress(const PageTable& table, uint32 entry, uint32& level, uint32& x, uint32& z)
{
    level = 0;
    while (level + 1u < table.Levels.size() && entry >= table.Levels[level + 1u].FirstEntry)
        ++level;
    const uint32 local = entry - table.Levels[level].FirstEntry;
    x = local % table.Levels[level].PagesX;
    z = local / table.Levels[level].PagesX;
}

// " L<level>(x,z)" for each of `entries`, the first `limit`, then the count left.
std::string AddressList(const PageTable& table, const std::vector<uint32>& entries, std::size_t limit)
{
    std::string out;
    for (std::size_t i = 0; i < entries.size() && i < limit; ++i)
    {
        uint32 level = 0, x = 0, z = 0;
        EntryAddress(table, entries[i], level, x, z);
        out += std::format(" L{}({},{})", level, x, z);
    }
    if (entries.size() > limit)
        out += std::format(" +{} more", entries.size() - limit);
    return out.empty() ? std::string(" none") : out;
}

} // namespace

uint64 PageTableWordCount(uint32 samplesX, uint32 samplesZ, uint32 slotCount)
{
    const std::vector<PageStreaming::PageStoreLevel> levels = PageStreaming::BuildPageStoreLevels(samplesX, samplesZ);
    if (levels.empty() || levels.size() > CBTTerrain::kCBTPageMaxLevels)
        return ~0ull;
    const uint64 entries = static_cast<uint64>(levels.back().FirstEntry) + levels.back().PagesX * levels.back().PagesZ;
    return CBTTerrain::kCBTPageFadeOffsetWords + static_cast<uint64>(slotCount) + entries;
}

HeightPageProfile PlatformHeightPageProfile()
{
#if defined(PLATFORM_STEAMDECK)
    return kSteamDeckHeightPages;
#else
    return kDesktopHeightPages;
#endif
}

std::string PageTableRefusal(std::string_view name, uint32 samplesX, uint32 samplesZ, const HeightPageProfile& profile)
{
    const uint64 words = PageTableWordCount(samplesX, samplesZ, profile.Slots);
    if (words <= profile.TableWordCap)
        return {};
    constexpr float64 kWordsPerMiB = 1024.0 * 1024.0 / sizeof(uint32);
    return std::format("{} ({} x {} samples) does not stream its height on this platform: its page table needs {} "
                       "words ({:.1f} MiB), over the platform's cap of {} words ({:.1f} MiB). It draws from its "
                       "height texture. Lower its samples per meter or its size to stream it.",
                       name, samplesX, samplesZ, words, static_cast<float64>(words) / kWordsPerMiB,
                       profile.TableWordCap, static_cast<float64>(profile.TableWordCap) / kWordsPerMiB);
}

std::string OverlayRefusal(std::string_view name, uint64 overlayBytes, const HeightPageProfile& profile)
{
    if (overlayBytes <= profile.OverlayByteCap)
        return {};
    constexpr float64 kBytesPerMiB = 1024.0 * 1024.0;
    return std::format("{} does not stream its height on this platform: its height modifiers reach so much of it "
                       "that their page overlay needs {} bytes ({:.1f} MiB), over the platform's cap of {} bytes "
                       "({:.1f} MiB). It draws from its height texture. Shrink the modifiers' bounds (a "
                       "terrain-wide modifier reaches every sample) or lower the terrain's samples per meter to "
                       "stream it.",
                       name, overlayBytes, static_cast<float64>(overlayBytes) / kBytesPerMiB, profile.OverlayByteCap,
                       static_cast<float64>(profile.OverlayByteCap) / kBytesPerMiB);
}

HeightPageOverlayAllowance PageOverlayAllowance(bool wouldPage, uint32 sourceX, uint32 sourceZ, uint32 latticeX,
                                                uint32 latticeZ, const HeightPageProfile& profile)
{
    if (!wouldPage)
        return {};
    return {profile.OverlayByteCap, sourceX != latticeX || sourceZ != latticeZ};
}

std::string OverlayGridRefusal(std::string_view name, uint32 storeX, uint32 storeZ, uint32 latticeX, uint32 latticeZ)
{
    return std::format("{} does not stream its height while height modifiers reach it: its heightmap's grid ({} x {} "
                       "samples) is not the terrain's tile lattice ({} x {} samples), on which the modifiers are "
                       "baked. It draws from its height texture. Import the heightmap at {} x {} samples to stream "
                       "it with its modifiers.",
                       name, storeX, storeZ, latticeX, latticeZ, latticeX, latticeZ);
}

void PackPageTableWords(const PageTable& table, const PageCacheGeometry& geometry, std::vector<uint32>& out)
{
    out.assign(CBTTerrain::kCBTPageFadeOffsetWords + table.SlotFade.size() + table.Entries.size(), 0u);
    if (table.Levels.empty())
        return;
    out[0] = table.Levels.front().SamplesX;
    out[1] = table.Levels.front().SamplesZ;
    out[2] = static_cast<uint32>(table.Levels.size());
    out[3] = geometry.SlotsPerRow;
    out[4] = static_cast<uint32>(table.SlotFade.size());
    for (std::size_t level = 0; level < table.Levels.size(); ++level)
    {
        uint32* shape = out.data() + CBTTerrain::kCBTPageHeaderWords + 4u * level;
        shape[0] = table.Levels[level].FirstEntry;
        shape[1] = table.Levels[level].PagesX;
        shape[2] = table.Levels[level].PagesZ;
    }
    uint32* fades = out.data() + CBTTerrain::kCBTPageFadeOffsetWords;
    for (std::size_t slot = 0; slot < table.SlotFade.size(); ++slot)
        fades[slot] = std::bit_cast<uint32>(table.SlotFade[slot]);
    std::memcpy(fades + table.SlotFade.size(), table.Entries.data(), table.Entries.size() * sizeof(uint32));
}

TerrainHeightPages::TerrainHeightPages(JobSystem::WorkStealingThreadPool* pool, AssetIOService* io,
                                       const HeightPageProfile& profile)
    : m_Residency(pool, io, PageStreaming::PageResidencyBudget{})
    , m_Profile(profile)
    , m_SlotCount(profile.Slots)
    , m_Geometry(MakePageCacheGeometry(profile.Slots))
{
}

void TerrainHeightPages::ReportRefusal(uint32 terrain, const std::string& refusal)
{
    std::string& logged = m_Refusals[terrain];
    if (logged == refusal)
        return;
    logged = refusal;
    Logger::Log::Warning("Terrain pages: {}", refusal);
}

void TerrainHeightPages::Configure(PagedTerrain& terrain, const PagedTerrainRequest& request)
{
    if (terrain.Stream == 0)
        terrain.Stream = m_NextStream++;
    terrain.Identity = request.SourceIdentity;
    terrain.Label = request.Name.empty() ? std::format("terrain {}", request.Terrain) : std::string(request.Name);
    terrain.Shape = request.Shape;
    terrain.Shape.CacheSlots = m_SlotCount;
    terrain.Requester.Configure(terrain.Shape);
    const PageRequestTerrain& shape = terrain.Shape;
    terrain.Table.Reset(shape.SamplesX, shape.SamplesZ, m_SlotCount);
    PackPageTableWords(terrain.Table, m_Geometry, terrain.Words);
    terrain.Version = ++m_TableVersion;
    m_Residency.Configure(terrain.Stream, kHeightPageField, request.Source);
    terrain.Overlay = request.Overlay;
    terrain.OverlayVersion = request.OverlayVersion;
    // The intent: the CBT logs the frame from which it draws the pages (CBTRenderFeature).
    Logger::Log::Info("Terrain pages: {} (terrain {} generation {}) streams its height pages ({} x {} samples, {} "
                      "levels)",
                      terrain.Label, request.Terrain, request.TerrainGeneration, shape.SamplesX, shape.SamplesZ,
                      terrain.Table.Levels.size());
    // The terrain's height changes source (from its texture, or from its previous pages).
    AddWholeTerrainDirty(request.Terrain, terrain);
}

void TerrainHeightPages::AddWholeTerrainDirty(uint32 terrainKey, const PagedTerrain& terrain)
{
    m_DirtyRects.push_back(HeightPageDirtyRect{terrainKey, terrain.Generation, 0.0f, 0.0f, 1.0f, 1.0f});
}

bool TerrainHeightPages::NeedsSource(uint32 terrain, uint64 sourceIdentity) const
{
    const auto it = m_Terrains.find(terrain);
    return it == m_Terrains.end() || it->second.Identity != sourceIdentity;
}

void TerrainHeightPages::Request(const PagedTerrainRequest& request, std::span<const Mathematics::Vector3> cameras,
                                 const PageLevelView& view)
{
    if (m_FramesInFlight == 0)
        return; // the field's cache is configured by the first Update
    auto [it, inserted] = m_Terrains.try_emplace(request.Terrain);
    PagedTerrain& terrain = it->second;
    terrain.Generation = request.TerrainGeneration;
    if (inserted || terrain.Identity != request.SourceIdentity || request.Shape.SamplesX != terrain.Shape.SamplesX ||
        request.Shape.SamplesZ != terrain.Shape.SamplesZ)
    {
        Configure(terrain, request);
    }
    else if (!SamePlacement(request.Shape, terrain.Shape))
    {
        // Moved or rescaled: the resident pages are keyed by terrain UV and stay valid, but the
        // requester asks for the pages under the cameras at the new placement, and every bisector's
        // world corners move with it.
        terrain.Shape = request.Shape;
        terrain.Shape.CacheSlots = m_SlotCount;
        terrain.Requester.Configure(terrain.Shape);
        AddWholeTerrainDirty(request.Terrain, terrain);
    }
    if (request.OverlayVersion != terrain.OverlayVersion)
    {
        // A modifier edit: the pages over the changed rect are rewritten with the new overlay as
        // they reload, keeping their old content until then. An overlay not derived from the one
        // the pages hold (the terrain's modifier data was rebuilt, as a scene load does) rewrites
        // every page either overlay reaches.
        if (request.OverlayChangedSince == terrain.OverlayVersion)
        {
            m_Residency.Refresh(terrain.Stream, request.OverlayChanged);
        }
        else
        {
            if (terrain.Overlay)
                m_Residency.Refresh(terrain.Stream, terrain.Overlay->Level0Rect());
            if (request.Overlay)
                m_Residency.Refresh(terrain.Stream, request.Overlay->Level0Rect());
        }
        terrain.Overlay = request.Overlay;
        terrain.OverlayVersion = request.OverlayVersion;
    }
    m_Refusals.erase(request.Terrain);
    terrain.Requested = true;
    terrain.Requester.Build(cameras, view, m_Wants);
    m_Residency.Request(terrain.Stream, m_Wants);
    if (LogResidency())
    {
        terrain.WantedEntries.clear();
        for (const PageStreaming::PageWant& want : m_Wants)
        {
            const PageStreaming::PageStoreLevel& level = terrain.Table.Levels[want.Address.Level];
            terrain.WantedEntries.push_back(level.FirstEntry + want.Address.Z * level.PagesX + want.Address.X);
        }
        std::sort(terrain.WantedEntries.begin(), terrain.WantedEntries.end());
    }
}

void TerrainHeightPages::LogResidencyChange(PagedTerrain& terrain)
{
    const PageTable& table = terrain.Table;
    std::vector<uint32> missing; // wanted, not resident
    std::vector<uint32> extra;   // resident, not wanted
    std::vector<uint32> resident;
    std::string counts;
    for (std::size_t level = 0; level < table.Levels.size(); ++level)
    {
        const uint32 first = table.Levels[level].FirstEntry;
        const uint32 end = first + table.Levels[level].PagesX * table.Levels[level].PagesZ;
        uint32 wanted = 0, residentCount = 0, both = 0;
        for (uint32 entry = first; entry < end; ++entry)
        {
            const bool isWanted = std::binary_search(terrain.WantedEntries.begin(), terrain.WantedEntries.end(), entry);
            const bool isResident = table.Entries[entry] != PageStreaming::kNoPage;
            wanted += isWanted ? 1u : 0u;
            residentCount += isResident ? 1u : 0u;
            both += isWanted && isResident ? 1u : 0u;
            if (isWanted && !isResident)
                missing.push_back(entry);
            if (isResident && !isWanted)
                extra.push_back(entry);
            if (isResident)
                resident.push_back(entry);
        }
        counts += std::format(" L{} {}/{}/{}", level, wanted, residentCount, both);
    }
    std::string line = std::format("wanted/resident/both per level:{}; wanted not resident{}; resident not wanted{}",
                                   counts, AddressList(table, missing, kLoggedAddressesPerList),
                                   AddressList(table, extra, kLoggedAddressesPerList));
    if (line == terrain.LoggedResidency)
        return;
    Logger::Log::Info("Terrain pages: {} {}", terrain.Label, line);
    // Once the residency matches the request, the whole resident set, so a settled pose's level map can be
    // rebuilt from the log.
    if (missing.empty() && extra.empty())
        Logger::Log::Info("Terrain pages: {} resident:{}", terrain.Label, AddressList(table, resident, resident.size()));
    terrain.LoggedResidency = std::move(line);
}

void TerrainHeightPages::Update(uint64 frameIndex, float32 deltaSeconds, bool teleport, uint32 framesInFlight)
{
    m_Uploads.clear();
    if (framesInFlight != m_FramesInFlight)
    {
        // The first frame, or the renderer's frames in flight changed: (re)start the field's cache.
        m_FramesInFlight = framesInFlight;
        m_Residency.ConfigureField(kHeightPageField, m_SlotCount, framesInFlight, kAtlasUpgradeFadeSeconds);
        for (auto& [key, terrain] : m_Terrains)
            terrain.Identity = ~terrain.Identity; // reconfigured by its next Request
    }

    m_Forgotten.clear();
    for (auto& [key, terrain] : m_Terrains)
    {
        if (!terrain.Requested)
            m_Forgotten.push_back(key);
        terrain.Requested = false;
    }
    for (uint32 key : m_Forgotten)
    {
        AddWholeTerrainDirty(key, m_Terrains[key]); // back to its texture
        Logger::Log::Info("Terrain pages: {} (terrain {} generation {}) stops streaming its height pages",
                          m_Terrains[key].Label, key, m_Terrains[key].Generation);
        m_Residency.Forget(m_Terrains[key].Stream);
        m_Terrains.erase(key);
    }

    m_Residency.Update(frameIndex, deltaSeconds, teleport);
    for (auto& [key, terrain] : m_Terrains)
    {
        CollectStreamChanges(key, terrain);
        if (LogResidency())
            LogResidencyChange(terrain);
    }
}

void TerrainHeightPages::AddDirtyPage(uint32 terrainKey, const PagedTerrain& terrain,
                                      const PageStreaming::PageAddress& address, HeightPageDirtyRect& rect,
                                      bool& any) const
{
    const PageStreaming::PageStoreLevel& level0 = terrain.Table.Levels.front();
    float32 minU = 0.0f, maxU = 0.0f, minV = 0.0f, maxV = 0.0f;
    PageAxisUV(address.X, address.Level, level0.SamplesX, minU, maxU);
    PageAxisUV(address.Z, address.Level, level0.SamplesZ, minV, maxV);
    if (!any)
    {
        rect = HeightPageDirtyRect{terrainKey, terrain.Generation, minU, minV, maxU, maxV};
        any = true;
        return;
    }
    rect.MinU = std::min(rect.MinU, minU);
    rect.MinV = std::min(rect.MinV, minV);
    rect.MaxU = std::max(rect.MaxU, maxU);
    rect.MaxV = std::max(rect.MaxV, maxV);
}

void TerrainHeightPages::CollectStreamChanges(uint32 terrainKey, PagedTerrain& terrain)
{
    const PageStreaming::PageCache& cache = m_Residency.FieldCache(kHeightPageField);
    if (UpdatePageTable(cache, terrain.Stream, terrain.Table) != 0u)
    {
        PackPageTableWords(terrain.Table, m_Geometry, terrain.Words);
        terrain.Version = ++m_TableVersion;
    }

    HeightPageDirtyRect rect;
    bool any = false;
    for (const PageStreaming::CachedPage& page : cache.Transitions())
        if (page.Stream == terrain.Stream)
            AddDirtyPage(terrainKey, terrain, page.Address, rect, any);
    for (const PageStreaming::CachedPage& page : cache.FadeChanges())
        if (page.Stream == terrain.Stream)
            AddDirtyPage(terrainKey, terrain, page.Address, rect, any);
    for (const PageStreaming::CachedPage& page : cache.Rewrites())
        if (page.Stream == terrain.Stream)
            AddDirtyPage(terrainKey, terrain, page.Address, rect, any);
    if (any)
        m_DirtyRects.push_back(rect);

    for (const PageStreaming::PageUploadRequest& upload : cache.Uploads())
    {
        if (upload.Page.Stream != terrain.Stream)
            continue;
        const std::vector<float32>* samples = m_Residency.Samples(terrain.Stream, upload.Page.Address);
        if (samples == nullptr)
            continue;
        HeightPageUpload& out = m_Uploads.emplace_back(HeightPageUpload{upload.Slot, *samples});
        if (terrain.Overlay)
            terrain.Overlay->ApplyToPage(upload.Page.Address, out.Samples);
    }
}

void TerrainHeightPages::RestartOnDeviceEpoch(uint64 deviceEpoch)
{
    if (deviceEpoch == m_DeviceEpoch)
        return;
    m_DeviceEpoch = deviceEpoch;
    if (m_FramesInFlight == 0)
        return;
    m_Residency.ConfigureField(kHeightPageField, m_SlotCount, m_FramesInFlight, kAtlasUpgradeFadeSeconds);
    for (auto& [key, terrain] : m_Terrains)
    {
        // Its streams' loaded pages stay; their slots are gone: no entry may name one.
        const PageStreaming::PageStoreLevel& level0 = terrain.Table.Levels.front();
        terrain.Table.Reset(level0.SamplesX, level0.SamplesZ, m_SlotCount);
        PackPageTableWords(terrain.Table, m_Geometry, terrain.Words);
        terrain.Version = ++m_TableVersion;
        AddWholeTerrainDirty(key, terrain);
    }
}

void TerrainHeightPages::PagedTerrains(std::vector<uint32>& out) const
{
    out.clear();
    for (const auto& [key, terrain] : m_Terrains)
        out.push_back(key);
}

const std::vector<uint32>* TerrainHeightPages::TableWords(uint32 terrain, uint64& version) const
{
    const auto it = m_Terrains.find(terrain);
    if (it == m_Terrains.end())
        return nullptr;
    version = it->second.Version;
    return &it->second.Words;
}

} // namespace GameEngine::TerrainECS
