// TerrainModifierSystem's height page overlay: what the modifiers add to a tiled terrain's base height
// pages (design page-streaming 3.8, the baked-page cache), kept beside the tile bake so a modified
// terrain renders every modifier from its first paged frame.

#include "TerrainECS/TerrainModifierSystem.h"

#include "PageStreaming/HeightPageOverlay.h"
#include "PageStreaming/PageStoreFormat.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <optional>
#include <vector>

namespace GameEngine::TerrainECS
{

namespace
{

using PageStreaming::PageSampleRect;

// The terrain's global tile lattice: level 0 of its height pages.
struct TileLattice
{
    uint32 SamplesX = 0;
    uint32 SamplesZ = 0;
    float32 SpacingX = 0.0f;
    float32 SpacingZ = 0.0f;
    float32 OriginX = 0.0f;
    float32 OriginZ = 0.0f;
};

std::optional<TileLattice> MakeTileLattice(const TiledTerrainData& tiled)
{
    const uint32 tileW = tiled.Config.TileConfig.HeightmapWidth;
    const uint32 tileH = tiled.Config.TileConfig.HeightmapHeight;
    if (tileW < 2u || tileH < 2u || tiled.Config.TilesPerAxisX == 0u || tiled.Config.TilesPerAxisZ == 0u ||
        !(tiled.Config.TileWorldSize > 0.0f))
        return std::nullopt;
    TileLattice lattice;
    lattice.SamplesX = tiled.Config.TilesPerAxisX * (tileW - 1u) + 1u;
    lattice.SamplesZ = tiled.Config.TilesPerAxisZ * (tileH - 1u) + 1u;
    lattice.SpacingX = tiled.Config.TileWorldSize / static_cast<float32>(tileW - 1u);
    lattice.SpacingZ = tiled.Config.TileWorldSize / static_cast<float32>(tileH - 1u);
    lattice.OriginX = tiled.WorldOriginX;
    lattice.OriginZ = tiled.WorldOriginZ;
    return lattice;
}

// The lattice samples a world rect reaches, one sample of margin on each side (the bake's own
// sample mapping rounds both ways), or nothing when it misses the terrain.
std::optional<PageSampleRect> LatticeRect(const TileLattice& lattice, float32 minX, float32 minZ, float32 maxX,
                                          float32 maxZ)
{
    const float64 firstX = std::floor((minX - lattice.OriginX) / lattice.SpacingX) - 1.0;
    const float64 firstZ = std::floor((minZ - lattice.OriginZ) / lattice.SpacingZ) - 1.0;
    const float64 lastX = std::ceil((maxX - lattice.OriginX) / lattice.SpacingX) + 1.0;
    const float64 lastZ = std::ceil((maxZ - lattice.OriginZ) / lattice.SpacingZ) + 1.0;
    if (lastX < 0.0 || lastZ < 0.0 || firstX > lattice.SamplesX - 1.0 || firstZ > lattice.SamplesZ - 1.0)
        return std::nullopt;
    PageSampleRect rect;
    rect.MinX = static_cast<uint32>(std::max(firstX, 0.0));
    rect.MinZ = static_cast<uint32>(std::max(firstZ, 0.0));
    rect.MaxX = static_cast<uint32>(std::min(lastX, lattice.SamplesX - 1.0));
    rect.MaxZ = static_cast<uint32>(std::min(lastZ, lattice.SamplesZ - 1.0));
    return rect;
}

PageSampleRect Union(const PageSampleRect& a, const PageSampleRect& b)
{
    return PageSampleRect{std::min(a.MinX, b.MinX), std::min(a.MinZ, b.MinZ), std::max(a.MaxX, b.MaxX),
                          std::max(a.MaxZ, b.MaxZ)};
}

std::optional<PageSampleRect> Intersect(const PageSampleRect& a, const PageSampleRect& b)
{
    const PageSampleRect r{std::max(a.MinX, b.MinX), std::max(a.MinZ, b.MinZ), std::min(a.MaxX, b.MaxX),
                           std::min(a.MaxZ, b.MaxZ)};
    if (r.MinX > r.MaxX || r.MinZ > r.MaxZ)
        return std::nullopt;
    return r;
}

// The rects of `reach` an overlay rebuild composes when `kept` (the previous overlay's rect inside
// `reach`) keeps its deltas but for `dirty`: the dirty part of `kept` and the strips of `reach`
// outside `kept`. Disjoint, so the row bands that compose them never write one sample twice.
std::vector<PageSampleRect> RebuildRegions(const PageSampleRect& reach, const PageSampleRect& kept,
                                           const std::optional<PageSampleRect>& dirty)
{
    std::vector<PageSampleRect> regions;
    const auto add = [&](uint32 minX, uint32 minZ, uint32 maxX, uint32 maxZ) {
        regions.push_back(PageSampleRect{minX, minZ, maxX, maxZ});
    };
    if (kept.MinZ > reach.MinZ)
        add(reach.MinX, reach.MinZ, reach.MaxX, kept.MinZ - 1u);
    if (kept.MaxZ < reach.MaxZ)
        add(reach.MinX, kept.MaxZ + 1u, reach.MaxX, reach.MaxZ);
    if (kept.MinX > reach.MinX)
        add(reach.MinX, kept.MinZ, kept.MinX - 1u, kept.MaxZ);
    if (kept.MaxX < reach.MaxX)
        add(kept.MaxX + 1u, kept.MinZ, reach.MaxX, kept.MaxZ);
    if (dirty)
        if (const auto inside = Intersect(*dirty, kept))
            add(inside->MinX, inside->MinZ, inside->MaxX, inside->MaxZ);
    return regions;
}

bool Holds(const PageSampleRect& outer, const PageSampleRect& inner)
{
    return inner.MinX >= outer.MinX && inner.MinZ >= outer.MinZ && inner.MaxX <= outer.MaxX &&
           inner.MaxZ <= outer.MaxZ;
}

// Publishes `overlay` as the terrain's overlay, `changed` the level-0 rect it changed from the
// previous one. The version is unique across every terrain's data, so the height pages tell an
// overlay derived from the one they hold from an overlay of rebuilt modifier data (a scene load
// replaces the terrain's data while its pages stay resident).
void PublishPageOverlay(TiledTerrainData& tiled, std::shared_ptr<PageStreaming::HeightPageOverlay> overlay,
                        const PageSampleRect& changed)
{
    static std::atomic<uint64> s_LastVersion{0};
    tiled.PageOverlay = std::move(overlay);
    tiled.PageOverlayChanged = changed;
    tiled.PageOverlayChangedSince = tiled.PageOverlayVersion;
    tiled.PageOverlayVersion = ++s_LastVersion;
}

// Drops the terrain's overlay: it pages no more until a new one is built.
void ReleaseHeightPageOverlay(TiledTerrainData& tiled)
{
    if (tiled.PageOverlay)
        PublishPageOverlay(tiled, nullptr, tiled.PageOverlay->Level0Rect());
    tiled.PageOverlayCurrent = false;
    tiled.PageOverlayRefusedBytes = 0;
    tiled.PageOverlayRefusedOffGrid = false;
}

} // namespace

void TerrainModifierSystem::BakeHeightPageOverlay(TiledTerrainData& tiled, float32 heightScale,
                                                  float32 terrainOriginY,
                                                  const std::vector<ResolvedModifier>& modifiers, bool fullBake,
                                                  const DirtyUnion& heightDirty)
{
    const std::optional<TileLattice> lattice = MakeTileLattice(tiled);
    if (!lattice || tiled.PageOverlayByteCap == 0)
    {
        // A terrain that keeps its height texture holds no overlay; it is built when it would page.
        ReleaseHeightPageOverlay(tiled);
        return;
    }
    std::optional<PageSampleRect> reach;
    for (const ResolvedModifier& mod : modifiers)
    {
        if (mod.IsSplatOnlyModifier())
            continue;
        if (const auto rect = LatticeRect(*lattice, mod.BoundsMinX, mod.BoundsMinZ, mod.BoundsMaxX, mod.BoundsMaxZ))
            reach = reach ? Union(*reach, *rect) : *rect;
    }
    PageStreaming::HeightPageOverlay* previous = tiled.PageOverlay.get();
    if (!reach)
    {
        // No height modifier reaches the terrain: its pages are its base.
        if (previous)
            PublishPageOverlay(tiled, nullptr, previous->Level0Rect());
        tiled.PageOverlayCurrent = true;
        tiled.PageOverlayRefusedBytes = 0;
        tiled.PageOverlayRefusedOffGrid = false;
        return;
    }
    if (tiled.PageOverlayOffGrid)
    {
        // Its pages are not on the lattice the overlay is baked on: the terrain keeps its height
        // texture while a modifier reaches it (the driver logs why).
        ReleaseHeightPageOverlay(tiled);
        tiled.PageOverlayRefusedOffGrid = true;
        return;
    }

    const std::vector<PageStreaming::PageStoreLevel> levels =
        PageStreaming::BuildPageStoreLevels(lattice->SamplesX, lattice->SamplesZ);
    const uint64 bytes = PageStreaming::HeightPageOverlay::Bytes(levels, *reach);
    if (bytes > tiled.PageOverlayByteCap)
    {
        // Over the platform's cap: the terrain keeps its height texture (the driver logs why).
        ReleaseHeightPageOverlay(tiled);
        tiled.PageOverlayRefusedBytes = bytes;
        return;
    }
    tiled.PageOverlayRefusedBytes = 0;

    std::optional<PageSampleRect> dirty;
    if (heightDirty.Any)
        dirty = LatticeRect(*lattice, heightDirty.MinX, heightDirty.MinZ, heightDirty.MaxX, heightDirty.MaxZ);
    const bool rebakeInPlace = !fullBake && previous && tiled.PageOverlayCurrent &&
                               Holds(previous->Level0Rect(), *reach) &&
                               (!dirty || Holds(previous->Level0Rect(), *dirty));
    if (rebakeInPlace && !dirty)
        return; // nothing under the terrain's height changed

    const PageSampleRect bake = rebakeInPlace ? *dirty : *reach;
    std::vector<float32> delta(static_cast<std::size_t>(bake.Width()) * bake.Height(), 0.0f);
    // A rebuild whose modifiers moved keeps the previous overlay's deltas wherever they still hold
    // (its rect inside the new reach, but for the dirty rect) and composes only the rest.
    std::vector<PageSampleRect> composed{bake};
    const std::optional<PageSampleRect> kept =
        !rebakeInPlace && !fullBake && previous && tiled.PageOverlayCurrent ? Intersect(previous->Level0Rect(), bake)
                                                                            : std::nullopt;
    if (kept)
    {
        for (uint32 z = kept->MinZ; z <= kept->MaxZ; ++z)
            for (uint32 x = kept->MinX; x <= kept->MaxX; ++x)
                delta[static_cast<std::size_t>(z - bake.MinZ) * bake.Width() + (x - bake.MinX)] =
                    previous->Delta(0, x, z);
        composed = RebuildRegions(bake, *kept, dirty);
    }
    std::vector<BakeBandRegion> regions;
    for (const PageSampleRect& r : composed)
        regions.push_back(BakeBandRegion{static_cast<int32>(r.MinX), static_cast<int32>(r.MinZ),
                                         static_cast<int32>(r.MaxX), static_cast<int32>(r.MaxZ)});
    static const std::vector<ResolvedModifier> kNoModifiers;
    RunBakeRowBands(regions, [&](std::size_t, int32 x0, int32 x1, int32 z0, int32 z1) {
        const uint32 countX = static_cast<uint32>(x1 - x0 + 1);
        const uint32 countZ = static_cast<uint32>(z1 - z0 + 1);
        std::vector<float32> baked;
        std::vector<float32> base;
        if (!ComposeLatticeBlock(tiled, heightScale, terrainOriginY, modifiers, x0, z0, countX, countZ, baked) ||
            !ComposeLatticeBlock(tiled, heightScale, terrainOriginY, kNoModifiers, x0, z0, countX, countZ, base))
            return;
        for (uint32 z = 0; z < countZ; ++z)
        {
            float32* out = delta.data() + static_cast<std::size_t>(z0 - static_cast<int32>(bake.MinZ) + z) * bake.Width() +
                           static_cast<std::size_t>(x0 - static_cast<int32>(bake.MinX));
            for (uint32 x = 0; x < countX; ++x)
                out[x] = baked[static_cast<std::size_t>(z) * countX + x] - base[static_cast<std::size_t>(z) * countX + x];
        }
    });

    if (rebakeInPlace)
    {
        previous->Rebake(bake, delta);
        PublishPageOverlay(tiled, tiled.PageOverlay, bake);
    }
    else
    {
        PageSampleRect changed = previous ? Union(previous->Level0Rect(), bake) : bake;
        if (dirty)
            changed = Union(changed, *dirty);
        PublishPageOverlay(
            tiled, std::make_shared<PageStreaming::HeightPageOverlay>(levels, bake, std::move(delta)), changed);
    }
    tiled.PageOverlayCurrent = true;
}

} // namespace GameEngine::TerrainECS
