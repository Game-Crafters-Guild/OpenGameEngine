#include "TerrainECS/AtlasResidencyController.h"

#include "Logger/Logger.h"

#include <algorithm>
#include <iterator>

namespace GameEngine::TerrainECS
{
namespace
{
const char* LodTag(bool isFull) { return isFull ? "full" : "coarse"; }
} // namespace

void AtlasResidencyController::Configure(uint32 tileRes, uint32 slotCount, uint32 framesInFlight,
                                         uint32 tilesPerAxisX, uint32 tilesPerAxisZ, bool logSignals,
                                         float32 fadeSeconds)
{
    m_LogSignals = logSignals;
    // Applied every call (not part of the sameGrid gate) so a runtime fade toggle never rebuilds.
    m_FadeSeconds = fadeSeconds;

    const bool sameGrid = m_Table.Geometry.TileRes == tileRes &&
                          m_ConfiguredSlotCount == slotCount &&
                          m_ConfiguredFramesInFlight == framesInFlight &&
                          m_Table.Geometry.TilesPerAxisX == tilesPerAxisX &&
                          m_Table.Geometry.TilesPerAxisZ == tilesPerAxisZ;
    if (sameGrid && IsConfigured())
        return;

    m_ConfiguredSlotCount = slotCount;
    m_ConfiguredFramesInFlight = framesInFlight;

    const AtlasGeometry geo = MakeAtlasGeometry(tileRes, slotCount, tilesPerAxisX, tilesPerAxisZ);
    m_Table.Resize(geo);
    m_Pool.Initialize(slotCount, framesInFlight);
    m_PrevDesired.clear();
    m_Fades.clear();
    m_AssignsThisFrame = m_EvictionsThisFrame = m_UploadsThisFrame = m_FallbacksThisFrame = 0;
    m_ActiveFadesThisFrame = 0;
}

uint32 AtlasResidencyController::TickFades(float32 deltaSeconds)
{
    if (m_Fades.empty() || m_FadeSeconds <= 0.0f || deltaSeconds <= 0.0f)
        return static_cast<uint32>(m_Fades.size());
    const float32 step = deltaSeconds / m_FadeSeconds;
    for (auto& [coord, fade] : m_Fades)
        fade = std::min(1.0f, fade + step);
    return static_cast<uint32>(m_Fades.size());
}

float32 AtlasResidencyController::FadeOf(TileCoord coord) const
{
    const auto it = m_Fades.find(coord);
    return it == m_Fades.end() ? 1.0f : it->second; // no active entry -> settled at full detail
}

void AtlasResidencyController::Update(uint64 frameIndex, float32 deltaSeconds,
                                      const std::vector<AtlasResidentTile>& residentTiles,
                                      uint32 maxAssignsPerFrame)
{
    if (!IsConfigured())
        return;

    m_Pool.BeginFrame(frameIndex);
    m_AssignsThisFrame = m_EvictionsThisFrame = m_UploadsThisFrame = m_FallbacksThisFrame = 0;
    m_UploadRequests.clear();
    m_RowTransitions.clear();

    // Advance in-flight upgrade crossfades before this frame's residency diff, so a tile assigned last
    // frame ramps this frame while a tile assigned THIS frame starts at 0 (added below, ticks next).
    TickFades(deltaSeconds);

    // Build the current desired lookup (coord -> IsFull) for the release diff.
    std::unordered_map<TileCoord, bool, TileCoordHash> desired;
    desired.reserve(residentTiles.size());
    for (const AtlasResidentTile& t : residentTiles)
        desired[t.Coord] = t.IsFull;

    // Releases: tiles resident last frame that are no longer wanted.
    for (const auto& [coord, prev] : m_PrevDesired)
    {
        if (desired.find(coord) != desired.end())
            continue;
        const AtlasSlotRef ref = m_Pool.Find(coord);
        if (ref.IsResident() && m_Pool.Release(coord, ref.Generation))
        {
            ++m_EvictionsThisFrame;
            m_Fades.erase(coord); // slot gone -> coarse field; no crossfade state to carry (no downgrade fade)
            m_RowTransitions.push_back(coord); // slot -> coarse: its bisectors must re-evaluate
            if (m_LogSignals)
            {
                Logger::Log::Info("Terrain.AtlasEvict tile=({},{}) slot={}", coord.X, coord.Z, ref.Slot);
                Logger::Log::Info("Terrain.AtlasMap tile=({},{}) slot=free", coord.X, coord.Z);
            }
        }
    }

    // Acquire nearest-first so a nearer tile reclaims a farther one deterministically.
    std::vector<AtlasResidentTile> ordered = residentTiles;
    std::sort(ordered.begin(), ordered.end(),
              [](const AtlasResidentTile& a, const AtlasResidentTile& b)
              { return a.PriorityDistanceSq < b.PriorityDistanceSq; });

    std::unordered_map<TileCoord, PrevTileState, TileCoordHash> nextPrev;
    nextPrev.reserve(ordered.size());

    for (const AtlasResidentTile& t : ordered)
    {
        auto prevIt = m_PrevDesired.find(t.Coord);
        const bool wasDesired = prevIt != m_PrevDesired.end();
        const bool prevResident = wasDesired && prevIt->second.WasResident;
        const bool prevFull = wasDesired && prevIt->second.IsFull;

        // Per-frame assign budget: a tile that is not already resident and would need a NEW slot is
        // deferred once the budget is spent, so the per-frame slot-upload spike stays bounded. It
        // stays in the coarse fallback (continuous base relief) and acquires on a later frame.
        // Nearest-first ordering (above) spends the budget on the tiles under the camera first. An
        // already-resident tile is never deferred — only Acquire (a NEW upload) is budgeted.
        if (m_AssignsThisFrame >= maxAssignsPerFrame && !m_Pool.Find(t.Coord).IsResident())
        {
            ++m_FallbacksThisFrame;
            nextPrev[t.Coord] = PrevTileState{t.IsFull, false};
            continue;
        }

        const AtlasSlotRef ref = m_Pool.Acquire(t.Coord, t.PriorityDistanceSq);
        const bool isResident = ref.IsResident();

        if (isResident && !prevResident)
        {
            ++m_AssignsThisFrame;
            ++m_UploadsThisFrame;
            m_UploadRequests.push_back({t.Coord, ref.Slot, t.IsFull});
            // Start the coarse->slot crossfade at 0 so the newly-resident tile fades its detail in from
            // the coarse field it was rendering a frame ago (the pop killer). fadeSeconds == 0 skips the
            // ramp entirely (instant swap = the pre-fade behavior / kill switch) — leave it settled at 1.
            if (m_FadeSeconds > 0.0f)
                m_Fades[t.Coord] = 0.0f;
            m_RowTransitions.push_back(t.Coord); // coarse -> slot: re-sample the new detail
            if (m_LogSignals)
            {
                Logger::Log::Info("Terrain.AtlasMap tile=({},{}) slot={} lod={}",
                                  t.Coord.X, t.Coord.Z, ref.Slot, LodTag(t.IsFull));
                Logger::Log::Info("Terrain.AtlasUpload tile=({},{}) slot={} lod={}",
                                  t.Coord.X, t.Coord.Z, ref.Slot, LodTag(t.IsFull));
            }
        }
        else if (isResident && prevResident && prevFull != t.IsFull)
        {
            // Coarse->Full (or Full->coarse) upgrade in place: re-patch the slot (content flips).
            ++m_UploadsThisFrame;
            m_UploadRequests.push_back({t.Coord, ref.Slot, t.IsFull});
            m_RowTransitions.push_back(t.Coord);
            if (m_LogSignals)
                Logger::Log::Info("Terrain.AtlasUpload tile=({},{}) slot={} lod={}",
                                  t.Coord.X, t.Coord.Z, ref.Slot, LodTag(t.IsFull));
        }
        else if (!isResident && prevResident)
        {
            // Lost its slot to a nearer tile while still wanted -> coarse fallback.
            ++m_EvictionsThisFrame;
            ++m_FallbacksThisFrame;
            m_Fades.erase(t.Coord); // slot reclaimed -> coarse field; drop any in-flight crossfade
            m_RowTransitions.push_back(t.Coord); // slot -> coarse: its bisectors must re-evaluate
            if (m_LogSignals)
            {
                Logger::Log::Info("Terrain.AtlasEvict tile=({},{}) slot=reclaimed", t.Coord.X, t.Coord.Z);
                Logger::Log::Info("Terrain.AtlasFallback tile=({},{})", t.Coord.X, t.Coord.Z);
            }
        }
        else if (!isResident && !wasDesired)
        {
            // Brand-new in-window tile that could not get a slot this frame.
            ++m_FallbacksThisFrame;
            if (m_LogSignals)
                Logger::Log::Info("Terrain.AtlasFallback tile=({},{})", t.Coord.X, t.Coord.Z);
        }
        // (!isResident && wasDesired && !prevResident): still in fallback — silent.

        nextPrev[t.Coord] = PrevTileState{t.IsFull, isResident};
    }

    m_PrevDesired = std::move(nextPrev);

    // Count the tiles mid-crossfade this frame (newly assigned at 0, ramping, or just settled to 1)
    // BEFORE the settled entries are pruned — every one has a Fade the shader must see updated this
    // frame, so each is a row-content change even without a residency transition.
    m_ActiveFadesThisFrame = static_cast<uint32>(m_Fades.size());
    RebuildTable(ordered);
    // Prune settled fades (Fade == 1) AFTER RebuildTable wrote their final 1.0 into the row, so next
    // frame they hold no state and do zero work (quiescence): the crossfade ticks only while active.
    for (auto it = m_Fades.begin(); it != m_Fades.end();)
        it = (it->second >= 1.0f) ? m_Fades.erase(it) : std::next(it);

    // A slot (re)assignment, release, OR an in-flight crossfade changed the indirection rows this
    // frame -> bump the version so the SSBO is re-uploaded; a parked+settled frame leaves it flat
    // (the #490 quiescence law — a fade is active work, a settled frame is not).
    if (m_AssignsThisFrame > 0u || m_EvictionsThisFrame > 0u || m_ActiveFadesThisFrame > 0u)
        ++m_TableVersion;
}

void AtlasResidencyController::RebuildTable(const std::vector<AtlasResidentTile>& residentTiles)
{
    m_Table.Clear();
    for (const AtlasResidentTile& t : residentTiles)
    {
        if (!m_Table.InBounds(t.Coord.X, t.Coord.Z))
            continue;
        const AtlasSlotRef ref = m_Pool.Find(t.Coord);
        if (!ref.IsResident())
            continue; // stays kAtlasNoSlot -> the sampler uses the coarse fallback
        TileAtlasSlot& row = m_Table.Row(t.Coord.X, t.Coord.Z);
        row.Slot = ref.Slot;
        row.Generation = ref.Generation;
        row.LodBias = t.IsFull ? kAtlasLodBiasFull : kAtlasLodBiasCoarse;
        row.Fade = FadeOf(t.Coord); // 0 (just assigned) .. 1 (settled) — the surface's coarse<->slot mix
    }
}

} // namespace GameEngine::TerrainECS
