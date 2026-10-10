#include "TerrainECS/TerrainAtlas.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace GameEngine::TerrainECS
{

AtlasGeometry MakeAtlasGeometry(uint32 tileRes, uint32 slotCount,
                                uint32 tilesPerAxisX, uint32 tilesPerAxisZ)
{
    AtlasGeometry geo{};
    if (tileRes < 2 || slotCount == 0)
        return geo;

    geo.TileRes = tileRes;
    geo.SlotStride = tileRes + 2u * kAtlasApron;
    geo.SlotCount = slotCount;
    geo.TilesPerAxisX = tilesPerAxisX;
    geo.TilesPerAxisZ = tilesPerAxisZ;

    // Square packing: slotsPerRow = ceil(sqrt(slotCount)).
    uint32 spr = static_cast<uint32>(std::ceil(std::sqrt(static_cast<double>(slotCount))));
    while (spr * spr < slotCount)
        ++spr;
    geo.SlotsPerRow = spr;
    geo.AtlasDim = spr * geo.SlotStride;
    return geo;
}

uint32 DeriveAtlasSlotCount(uint32 tilesPerAxisX, uint32 tilesPerAxisZ, uint32 tileRes,
                            uint64 vramBudgetBytes, uint32 bytesPerTexel)
{
    const uint64 totalTiles = static_cast<uint64>(std::max(1u, tilesPerAxisX)) *
                              static_cast<uint64>(std::max(1u, tilesPerAxisZ));
    if (tileRes < 2u)
        return 1u;

    const uint64 slotStride = static_cast<uint64>(tileRes) + 2ull * kAtlasApron;
    const uint64 bytesPerSlot =
        slotStride * slotStride * static_cast<uint64>(std::max(kAtlasSlotBytesPerTexel, bytesPerTexel));

    // The atlas is square-padded to slotsPerRow^2 slots, so the VRAM cap bounds slotsPerRow, not
    // the raw slot count: the largest N with N^2 * bytesPerSlot <= budget.
    const double perRowD =
        std::sqrt(static_cast<double>(vramBudgetBytes) /
                  static_cast<double>(std::max<uint64>(1ull, bytesPerSlot)));
    const uint64 slotsPerRow = std::max<uint64>(1ull, static_cast<uint64>(perRowD));
    const uint64 vramCappedSlots = slotsPerRow * slotsPerRow;

    return static_cast<uint32>(std::max<uint64>(1ull, std::min(totalTiles, vramCappedSlots)));
}

void AtlasIndirectionTable::Resize(const AtlasGeometry& geo)
{
    Geometry = geo;
    Rows.assign(static_cast<size_t>(geo.TilesPerAxisX) * geo.TilesPerAxisZ, TileAtlasSlot{});
}

void AtlasIndirectionTable::Clear()
{
    for (TileAtlasSlot& r : Rows)
        r = TileAtlasSlot{};
}

AtlasTileDirtyRoute ResolveAtlasTileDirtyRoute(bool heightDirty, bool splatDirty, bool resident)
{
    if (heightDirty)
        return resident ? AtlasTileDirtyRoute::ResidentFull : AtlasTileDirtyRoute::NonResidentHeight;
    if (splatDirty)
        return resident ? AtlasTileDirtyRoute::ResidentSplatOnly : AtlasTileDirtyRoute::NonResidentSplatOnly;
    return AtlasTileDirtyRoute::None;
}

AtlasEdgeSource ResolveEdgeOwnership(bool selfIsFull, bool neighborResident, bool neighborIsFull)
{
    // The Full side always owns the shared texel. Coarse yields to a resident
    // Full neighbour; every other combination keeps Self (both Full and both
    // coarse are equal by construction; a non-resident neighbour is the horizon
    // case, handled by the fallback contract, not here).
    if (!selfIsFull && neighborResident && neighborIsFull)
        return AtlasEdgeSource::Neighbor;
    return AtlasEdgeSource::Self;
}

float32 SampleGridBilinearTexel(const float32* grid, uint32 width, uint32 height,
                                float32 tx, float32 ty)
{
    if (!grid || width == 0 || height == 0)
        return 0.0f;
    tx = std::clamp(tx, 0.0f, static_cast<float32>(width - 1));
    ty = std::clamp(ty, 0.0f, static_cast<float32>(height - 1));
    const uint32 x0 = static_cast<uint32>(std::floor(tx));
    const uint32 y0 = static_cast<uint32>(std::floor(ty));
    const uint32 x1 = std::min(x0 + 1u, width - 1u);
    const uint32 y1 = std::min(y0 + 1u, height - 1u);
    const float32 fx = tx - static_cast<float32>(x0);
    const float32 fy = ty - static_cast<float32>(y0);
    const float32 s00 = grid[static_cast<size_t>(y0) * width + x0];
    const float32 s10 = grid[static_cast<size_t>(y0) * width + x1];
    const float32 s01 = grid[static_cast<size_t>(y1) * width + x0];
    const float32 s11 = grid[static_cast<size_t>(y1) * width + x1];
    const float32 a = s00 + (s10 - s00) * fx;
    const float32 b = s01 + (s11 - s01) * fx;
    return a + (b - a) * fy;
}

uint32 ResolveCornerOwner(const bool* isFull, uint32 count)
{
    if (!isFull || count == 0u)
        return 0u;
    for (uint32 i = 0; i < count; ++i)
        if (isFull[i])
            return i; // the Full tile earliest in the caller's order owns the corner
    return 0u;        // all coarse -> the four copies are equal by construction
}

void PackTileComponentsIntoSlot(float32* atlas, const AtlasGeometry& geo, uint32 slot,
                                const float32* tileData, uint32 components, bool tileIsFull,
                                const AtlasTileNeighbors& neighbors,
                                const AtlasNeighborEdgesN& neighborEdges,
                                const AtlasCornerYieldsN& corners)
{
    if (!atlas || !tileData || !geo.IsValid() || slot >= geo.SlotCount || components == 0)
        return;

    const uint32 dim = geo.AtlasDim;
    const uint32 res = geo.TileRes;
    const uint32 ix = geo.SlotInteriorTexelX(slot);
    const uint32 iy = geo.SlotInteriorTexelY(slot);
    const size_t edgeLen = static_cast<size_t>(res) * components; // components-interleaved edge line
    // One texel's component c. atlas is components-interleaved: texel (x,y) owns [idx, idx+components).
    auto at = [&](uint32 x, uint32 y, uint32 c) -> float32& {
        return atlas[(static_cast<size_t>(y) * dim + x) * components + c];
    };
    // Copy one texel's components from src (points at the texel's first component) to (dx,dy).
    auto copyTexel = [&](uint32 dx, uint32 dy, const float32* src) {
        for (uint32 c = 0; c < components; ++c)
            at(dx, dy, c) = src[c];
    };

    // 1) Interior data.
    for (uint32 z = 0; z < res; ++z)
        for (uint32 x = 0; x < res; ++x)
            copyTexel(ix + x, iy + z, &tileData[(static_cast<size_t>(z) * res + x) * components]);

    // 2) Edge ownership (Risk 1): where this coarse tile abuts a resident-Full
    //    neighbour, overwrite the shared interior edge with the neighbour's edge
    //    so the two physical copies agree. The Full side never overwrites.
    if (ResolveEdgeOwnership(tileIsFull, neighbors.LeftResident, neighbors.LeftFull) ==
            AtlasEdgeSource::Neighbor && neighborEdges.Left.size() == edgeLen)
        for (uint32 z = 0; z < res; ++z)
            copyTexel(ix, iy + z, &neighborEdges.Left[static_cast<size_t>(z) * components]);
    if (ResolveEdgeOwnership(tileIsFull, neighbors.RightResident, neighbors.RightFull) ==
            AtlasEdgeSource::Neighbor && neighborEdges.Right.size() == edgeLen)
        for (uint32 z = 0; z < res; ++z)
            copyTexel(ix + res - 1u, iy + z, &neighborEdges.Right[static_cast<size_t>(z) * components]);
    if (ResolveEdgeOwnership(tileIsFull, neighbors.TopResident, neighbors.TopFull) ==
            AtlasEdgeSource::Neighbor && neighborEdges.Top.size() == edgeLen)
        for (uint32 x = 0; x < res; ++x)
            copyTexel(ix + x, iy, &neighborEdges.Top[static_cast<size_t>(x) * components]);
    if (ResolveEdgeOwnership(tileIsFull, neighbors.BottomResident, neighbors.BottomFull) ==
            AtlasEdgeSource::Neighbor && neighborEdges.Bottom.size() == edgeLen)
        for (uint32 x = 0; x < res; ++x)
            copyTexel(ix + x, iy + res - 1u, &neighborEdges.Bottom[static_cast<size_t>(x) * components]);

    // 2b) Corner ownership (Risk 1, 4-tile junction): a corner texel is shared by
    //     up to four tiles across the diagonal, so a coarse tile diagonal to the
    //     only Full tile learns the shared corner from NO axis edge. Overwrite each
    //     corner this tile does not own with the authoritative value. Applied AFTER
    //     edge ownership so all four physical copies of a shared corner converge.
    auto applyCorner = [&](const AtlasCornerYieldsN::Corner& corner, uint32 cx, uint32 cy) {
        if (corner.Apply && corner.Value.size() == components)
            copyTexel(cx, cy, corner.Value.data());
    };
    applyCorner(corners.TopLeft, ix, iy);
    applyCorner(corners.TopRight, ix + res - 1u, iy);
    applyCorner(corners.BottomLeft, ix, iy + res - 1u);
    applyCorner(corners.BottomRight, ix + res - 1u, iy + res - 1u);

    // 3) Apron gutter duplicates the resolved interior edges + corners, so
    //    hardware bilinear straddle at a slot edge reads a duplicated edge texel
    //    rather than the neighbouring slot's data. iy/ix >= kAtlasApron (>=1)
    //    always, so the apron indices stay inside this slot.
    auto dupTexel = [&](uint32 dx, uint32 dy, uint32 sx, uint32 sy) {
        for (uint32 c = 0; c < components; ++c)
            at(dx, dy, c) = at(sx, sy, c);
    };
    for (uint32 z = 0; z < res; ++z)
    {
        dupTexel(ix - 1u, iy + z, ix, iy + z);                   // left apron col
        dupTexel(ix + res, iy + z, ix + res - 1u, iy + z);       // right apron col
    }
    for (uint32 x = 0; x < res; ++x)
    {
        dupTexel(ix + x, iy - 1u, ix + x, iy);                   // top apron row
        dupTexel(ix + x, iy + res, ix + x, iy + res - 1u);       // bottom apron row
    }
    dupTexel(ix - 1u, iy - 1u, ix, iy);                                 // TL
    dupTexel(ix + res, iy - 1u, ix + res - 1u, iy);                     // TR
    dupTexel(ix - 1u, iy + res, ix, iy + res - 1u);                     // BL
    dupTexel(ix + res, iy + res, ix + res - 1u, iy + res - 1u);         // BR
}

void PackTileHeightIntoSlot(float32* atlas, const AtlasGeometry& geo, uint32 slot,
                            const float32* tileHeights, bool tileIsFull,
                            const AtlasTileNeighbors& neighbors,
                            const AtlasNeighborEdges& neighborEdges,
                            const AtlasCornerYields& corners)
{
    // Height is the componentsPerTexel == 1 specialization; delegate so the intricate apron /
    // edge / corner index math is single-sourced (the shipped height oracles cover this path).
    AtlasNeighborEdgesN edgesN;
    edgesN.Left = neighborEdges.Left;
    edgesN.Right = neighborEdges.Right;
    edgesN.Top = neighborEdges.Top;
    edgesN.Bottom = neighborEdges.Bottom;
    AtlasCornerYieldsN cornersN;
    auto marshal = [](const AtlasCornerYields::Corner& src, AtlasCornerYieldsN::Corner& dst) {
        dst.Apply = src.Apply;
        if (src.Apply)
            dst.Value = {src.Value};
    };
    marshal(corners.TopLeft, cornersN.TopLeft);
    marshal(corners.TopRight, cornersN.TopRight);
    marshal(corners.BottomLeft, cornersN.BottomLeft);
    marshal(corners.BottomRight, cornersN.BottomRight);
    PackTileComponentsIntoSlot(atlas, geo, slot, tileHeights, 1u, tileIsFull, neighbors, edgesN,
                               cornersN);
}

void ReadSlotInteriorEdge(const float32* atlas, const AtlasGeometry& geo, uint32 slot,
                          AtlasEdgeSide side, std::vector<float32>& outEdge)
{
    outEdge.clear();
    if (!atlas || !geo.IsValid() || slot >= geo.SlotCount)
        return;
    const uint32 dim = geo.AtlasDim;
    const uint32 res = geo.TileRes;
    const uint32 ix = geo.SlotInteriorTexelX(slot);
    const uint32 iy = geo.SlotInteriorTexelY(slot);
    outEdge.resize(res);
    auto at = [&](uint32 x, uint32 y) { return atlas[static_cast<size_t>(y) * dim + x]; };
    switch (side)
    {
    case AtlasEdgeSide::NegX: for (uint32 z = 0; z < res; ++z) outEdge[z] = at(ix, iy + z); break;
    case AtlasEdgeSide::PosX: for (uint32 z = 0; z < res; ++z) outEdge[z] = at(ix + res - 1u, iy + z); break;
    case AtlasEdgeSide::NegZ: for (uint32 x = 0; x < res; ++x) outEdge[x] = at(ix + x, iy); break;
    case AtlasEdgeSide::PosZ: for (uint32 x = 0; x < res; ++x) outEdge[x] = at(ix + x, iy + res - 1u); break;
    }
}

std::vector<TileCoord> NeighborsNeedingApronRefresh(const AtlasIndirectionTable& table,
                                                    int32 tx, int32 tz)
{
    std::vector<TileCoord> out;
    // Axis neighbours share an edge; diagonal neighbours share only the corner
    // texel — both depend on this tile's LOD, so both must refresh on a change.
    const TileCoord candidates[8] = {
        {tx - 1, tz}, {tx + 1, tz}, {tx, tz - 1}, {tx, tz + 1},
        {tx - 1, tz - 1}, {tx + 1, tz - 1}, {tx - 1, tz + 1}, {tx + 1, tz + 1}};
    for (const TileCoord& c : candidates)
    {
        if (!table.InBounds(c.X, c.Z))
            continue;
        if (table.Row(c.X, c.Z).Slot != kAtlasNoSlot)
            out.push_back(c);
    }
    return out;
}

float32 AtlasHeightSampler::SampleHeightNormalized(float32 u, float32 v) const
{
    u = std::clamp(u, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);

    auto coarseSample = [&]() -> float32 {
        if (Coarse && Coarse->IsValid())
            return SampleGridBilinearTexel(Coarse->Heights.data(), Coarse->Dim, Coarse->Dim,
                                           u * static_cast<float32>(Coarse->Dim - 1),
                                           v * static_cast<float32>(Coarse->Dim - 1));
        return 0.0f;
    };

    if (!Rows || !Geometry.IsValid() || Geometry.TilesPerAxisX == 0 || Geometry.TilesPerAxisZ == 0)
        return coarseSample();

    const float32 fx = u * static_cast<float32>(Geometry.TilesPerAxisX);
    const float32 fz = v * static_cast<float32>(Geometry.TilesPerAxisZ);
    const int32 tx = std::clamp(static_cast<int32>(std::floor(fx)), 0,
                                static_cast<int32>(Geometry.TilesPerAxisX) - 1);
    const int32 tz = std::clamp(static_cast<int32>(std::floor(fz)), 0,
                                static_cast<int32>(Geometry.TilesPerAxisZ) - 1);
    const float32 localU = std::clamp(fx - static_cast<float32>(tx), 0.0f, 1.0f);
    const float32 localV = std::clamp(fz - static_cast<float32>(tz), 0.0f, 1.0f);

    const TileAtlasSlot& row = Rows[Geometry.TileIndex(tx, tz)];
    if (row.Slot == kAtlasNoSlot || !Atlas)
        return coarseSample(); // out-of-window fallback (Risk 3): coarse, never garbage

    const float32 absX = static_cast<float32>(Geometry.SlotInteriorTexelX(row.Slot)) +
                         localU * static_cast<float32>(Geometry.TileRes - 1);
    const float32 absY = static_cast<float32>(Geometry.SlotInteriorTexelY(row.Slot)) +
                         localV * static_cast<float32>(Geometry.TileRes - 1);
    return SampleGridBilinearTexel(Atlas, Geometry.AtlasDim, Geometry.AtlasDim, absX, absY);
}

bool AtlasHeightSampler::IsResidentAt(float32 u, float32 v) const
{
    if (!Rows || !Geometry.IsValid() || Geometry.TilesPerAxisX == 0 || Geometry.TilesPerAxisZ == 0)
        return false;
    u = std::clamp(u, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);
    const int32 tx = std::clamp(static_cast<int32>(std::floor(u * static_cast<float32>(Geometry.TilesPerAxisX))),
                                0, static_cast<int32>(Geometry.TilesPerAxisX) - 1);
    const int32 tz = std::clamp(static_cast<int32>(std::floor(v * static_cast<float32>(Geometry.TilesPerAxisZ))),
                                0, static_cast<int32>(Geometry.TilesPerAxisZ) - 1);
    return Rows[Geometry.TileIndex(tx, tz)].Slot != kAtlasNoSlot;
}

void BuildCoarseHeightField(uint32 coarseDim, uint32 tilesX, uint32 tilesZ, float32 fallback,
                            const std::function<CoarseTileSource(int32 tx, int32 tz)>& getTile,
                            std::vector<float32>& out, const float32* baseField)
{
    if (coarseDim == 0 || tilesX == 0 || tilesZ == 0 || !getTile)
    {
        out.clear();
        return;
    }
    const size_t texelCount = static_cast<size_t>(coarseDim) * coarseDim;
    // A texel with no streamed tile keeps the whole-terrain base relief (continuous with resident
    // tiles, same source) when a base field is supplied, else the flat fallback constant.
    if (baseField)
        out.assign(baseField, baseField + texelCount);
    else
        out.assign(texelCount, fallback);
    const float32 invCoarse = coarseDim > 1u ? 1.0f / static_cast<float32>(coarseDim - 1u) : 0.0f;
    for (uint32 cz = 0; cz < coarseDim; ++cz)
    {
        const float32 v = static_cast<float32>(cz) * invCoarse;
        const float32 tfz = v * static_cast<float32>(tilesZ);
        const int32 tz = std::clamp(static_cast<int32>(tfz), 0, static_cast<int32>(tilesZ) - 1);
        const float32 lv = std::clamp(tfz - static_cast<float32>(tz), 0.0f, 1.0f);
        for (uint32 cx = 0; cx < coarseDim; ++cx)
        {
            const float32 u = static_cast<float32>(cx) * invCoarse;
            const float32 tfx = u * static_cast<float32>(tilesX);
            const int32 tx = std::clamp(static_cast<int32>(tfx), 0, static_cast<int32>(tilesX) - 1);
            const float32 lu = std::clamp(tfx - static_cast<float32>(tx), 0.0f, 1.0f);
            const CoarseTileSource src = getTile(tx, tz);
            if (src.Heights && src.Width >= 2 && src.Height >= 2)
                out[static_cast<size_t>(cz) * coarseDim + cx] = SampleGridBilinearTexel(
                    src.Heights, src.Width, src.Height, lu * static_cast<float32>(src.Width - 1u),
                    lv * static_cast<float32>(src.Height - 1u));
        }
    }
}

void ReadSlotInteriorEdgeN(const float32* atlas, const AtlasGeometry& geo, uint32 slot,
                           AtlasEdgeSide side, uint32 components, std::vector<float32>& outEdge)
{
    outEdge.clear();
    if (!atlas || !geo.IsValid() || slot >= geo.SlotCount || components == 0)
        return;
    const uint32 dim = geo.AtlasDim;
    const uint32 res = geo.TileRes;
    const uint32 ix = geo.SlotInteriorTexelX(slot);
    const uint32 iy = geo.SlotInteriorTexelY(slot);
    outEdge.resize(static_cast<size_t>(res) * components);
    auto read = [&](uint32 k, uint32 x, uint32 y) {
        for (uint32 c = 0; c < components; ++c)
            outEdge[static_cast<size_t>(k) * components + c] =
                atlas[(static_cast<size_t>(y) * dim + x) * components + c];
    };
    switch (side)
    {
    case AtlasEdgeSide::NegX: for (uint32 z = 0; z < res; ++z) read(z, ix, iy + z); break;
    case AtlasEdgeSide::PosX: for (uint32 z = 0; z < res; ++z) read(z, ix + res - 1u, iy + z); break;
    case AtlasEdgeSide::NegZ: for (uint32 x = 0; x < res; ++x) read(x, ix + x, iy); break;
    case AtlasEdgeSide::PosZ: for (uint32 x = 0; x < res; ++x) read(x, ix + x, iy + res - 1u); break;
    }
}

float32 SampleGridBilinearTexelN(const float32* grid, uint32 width, uint32 height,
                                 uint32 components, uint32 comp, float32 tx, float32 ty)
{
    if (!grid || width == 0 || height == 0 || comp >= components)
        return 0.0f;
    tx = std::clamp(tx, 0.0f, static_cast<float32>(width - 1));
    ty = std::clamp(ty, 0.0f, static_cast<float32>(height - 1));
    const uint32 x0 = static_cast<uint32>(std::floor(tx));
    const uint32 y0 = static_cast<uint32>(std::floor(ty));
    const uint32 x1 = std::min(x0 + 1u, width - 1u);
    const uint32 y1 = std::min(y0 + 1u, height - 1u);
    const float32 fx = tx - static_cast<float32>(x0);
    const float32 fy = ty - static_cast<float32>(y0);
    auto tap = [&](uint32 x, uint32 y) {
        return grid[(static_cast<size_t>(y) * width + x) * components + comp];
    };
    const float32 a = tap(x0, y0) + (tap(x1, y0) - tap(x0, y0)) * fx;
    const float32 b = tap(x0, y1) + (tap(x1, y1) - tap(x0, y1)) * fx;
    return a + (b - a) * fy;
}

void AtlasComponentSampler::Sample(float32 u, float32 v, float32* out) const
{
    if (!out || Components == 0)
        return;
    u = std::clamp(u, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);

    auto coarseSample = [&]() {
        if (Coarse && Coarse->IsValid() && Coarse->Components == Components)
            for (uint32 c = 0; c < Components; ++c)
                out[c] = SampleGridBilinearTexelN(Coarse->Data.data(), Coarse->Dim, Coarse->Dim,
                                                  Components, c, u * static_cast<float32>(Coarse->Dim - 1),
                                                  v * static_cast<float32>(Coarse->Dim - 1));
        else
            for (uint32 c = 0; c < Components; ++c)
                out[c] = 0.0f;
    };

    if (!Rows || !Geometry.IsValid() || Geometry.TilesPerAxisX == 0 || Geometry.TilesPerAxisZ == 0)
    {
        coarseSample();
        return;
    }

    const float32 fx = u * static_cast<float32>(Geometry.TilesPerAxisX);
    const float32 fz = v * static_cast<float32>(Geometry.TilesPerAxisZ);
    const int32 tx = std::clamp(static_cast<int32>(std::floor(fx)), 0,
                                static_cast<int32>(Geometry.TilesPerAxisX) - 1);
    const int32 tz = std::clamp(static_cast<int32>(std::floor(fz)), 0,
                                static_cast<int32>(Geometry.TilesPerAxisZ) - 1);
    const float32 localU = std::clamp(fx - static_cast<float32>(tx), 0.0f, 1.0f);
    const float32 localV = std::clamp(fz - static_cast<float32>(tz), 0.0f, 1.0f);

    const TileAtlasSlot& row = Rows[Geometry.TileIndex(tx, tz)];
    if (row.Slot == kAtlasNoSlot || !Atlas)
    {
        coarseSample(); // out-of-window fallback (Risk 3): coarse, never garbage
        return;
    }

    const float32 absX = static_cast<float32>(Geometry.SlotInteriorTexelX(row.Slot)) +
                         localU * static_cast<float32>(Geometry.TileRes - 1);
    const float32 absY = static_cast<float32>(Geometry.SlotInteriorTexelY(row.Slot)) +
                         localV * static_cast<float32>(Geometry.TileRes - 1);
    for (uint32 c = 0; c < Components; ++c)
        out[c] = SampleGridBilinearTexelN(Atlas, Geometry.AtlasDim, Geometry.AtlasDim, Components, c,
                                          absX, absY);
}

bool AtlasComponentSampler::IsResidentAt(float32 u, float32 v) const
{
    if (!Rows || !Geometry.IsValid() || Geometry.TilesPerAxisX == 0 || Geometry.TilesPerAxisZ == 0)
        return false;
    u = std::clamp(u, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);
    const int32 tx = std::clamp(static_cast<int32>(std::floor(u * static_cast<float32>(Geometry.TilesPerAxisX))),
                                0, static_cast<int32>(Geometry.TilesPerAxisX) - 1);
    const int32 tz = std::clamp(static_cast<int32>(std::floor(v * static_cast<float32>(Geometry.TilesPerAxisZ))),
                                0, static_cast<int32>(Geometry.TilesPerAxisZ) - 1);
    return Rows[Geometry.TileIndex(tx, tz)].Slot != kAtlasNoSlot;
}

void BuildCoarseComponentField(uint32 coarseDim, uint32 tilesX, uint32 tilesZ, uint32 components,
                               const float32* fallback,
                               const std::function<CoarseTileSourceN(int32 tx, int32 tz)>& getTile,
                               std::vector<float32>& out)
{
    if (coarseDim == 0 || tilesX == 0 || tilesZ == 0 || components == 0 || !getTile)
    {
        out.clear();
        return;
    }
    out.assign(static_cast<size_t>(coarseDim) * coarseDim * components, 0.0f);
    if (fallback)
        for (size_t i = 0; i < static_cast<size_t>(coarseDim) * coarseDim; ++i)
            for (uint32 c = 0; c < components; ++c)
                out[i * components + c] = fallback[c];
    const float32 invCoarse = coarseDim > 1u ? 1.0f / static_cast<float32>(coarseDim - 1u) : 0.0f;
    for (uint32 cz = 0; cz < coarseDim; ++cz)
    {
        const float32 v = static_cast<float32>(cz) * invCoarse;
        const float32 tfz = v * static_cast<float32>(tilesZ);
        const int32 tz = std::clamp(static_cast<int32>(tfz), 0, static_cast<int32>(tilesZ) - 1);
        const float32 lv = std::clamp(tfz - static_cast<float32>(tz), 0.0f, 1.0f);
        for (uint32 cx = 0; cx < coarseDim; ++cx)
        {
            const float32 u = static_cast<float32>(cx) * invCoarse;
            const float32 tfx = u * static_cast<float32>(tilesX);
            const int32 tx = std::clamp(static_cast<int32>(tfx), 0, static_cast<int32>(tilesX) - 1);
            const float32 lu = std::clamp(tfx - static_cast<float32>(tx), 0.0f, 1.0f);
            const CoarseTileSourceN src = getTile(tx, tz);
            if (src.Data && src.Width >= 2 && src.Height >= 2 && src.Components == components)
            {
                const size_t base = (static_cast<size_t>(cz) * coarseDim + cx) * components;
                for (uint32 c = 0; c < components; ++c)
                    out[base + c] = SampleGridBilinearTexelN(
                        src.Data, src.Width, src.Height, components, c,
                        lu * static_cast<float32>(src.Width - 1u),
                        lv * static_cast<float32>(src.Height - 1u));
            }
        }
    }
}

void PackTileBytesIntoSlot(uint8* atlas, const AtlasGeometry& geo, uint32 slot,
                           const uint8* tileData, uint32 bytesPerTexel)
{
    if (!atlas || !tileData || !geo.IsValid() || slot >= geo.SlotCount || bytesPerTexel == 0)
        return;

    const uint32 dim = geo.AtlasDim;
    const uint32 res = geo.TileRes;
    const uint32 ix = geo.SlotInteriorTexelX(slot);
    const uint32 iy = geo.SlotInteriorTexelY(slot);
    auto texel = [&](uint32 x, uint32 y) -> uint8* {
        return atlas + (static_cast<size_t>(y) * dim + x) * bytesPerTexel;
    };
    auto copy = [&](uint32 dx, uint32 dy, const uint8* src) { std::memcpy(texel(dx, dy), src, bytesPerTexel); };
    auto dup = [&](uint32 dx, uint32 dy, uint32 sx, uint32 sy) { std::memcpy(texel(dx, dy), texel(sx, sy), bytesPerTexel); };

    // Interior (empty neighbours -> no edge/corner ownership; matches the shipped height extraction).
    for (uint32 z = 0; z < res; ++z)
        for (uint32 x = 0; x < res; ++x)
            copy(ix + x, iy + z, tileData + (static_cast<size_t>(z) * res + x) * bytesPerTexel);

    // 1-texel apron gutter duplicates the interior edges + corners (hardware-bilinear straddle safety).
    for (uint32 z = 0; z < res; ++z)
    {
        dup(ix - 1u, iy + z, ix, iy + z);
        dup(ix + res, iy + z, ix + res - 1u, iy + z);
    }
    for (uint32 x = 0; x < res; ++x)
    {
        dup(ix + x, iy - 1u, ix + x, iy);
        dup(ix + x, iy + res, ix + x, iy + res - 1u);
    }
    dup(ix - 1u, iy - 1u, ix, iy);
    dup(ix + res, iy - 1u, ix + res - 1u, iy);
    dup(ix - 1u, iy + res, ix, iy + res - 1u);
    dup(ix + res, iy + res, ix + res - 1u, iy + res - 1u);
}

AtlasSlotUploadRect PackTileEditRegionIntoSlot(std::vector<uint8>& outScratch,
                                               const uint8* tileData, uint32 bytesPerTexel,
                                               const AtlasGeometry& geo, uint32 slot,
                                               int32 x0, int32 z0, int32 x1, int32 z1)
{
    AtlasSlotUploadRect r{};
    if (!tileData || bytesPerTexel == 0 || !geo.IsValid() || slot >= geo.SlotCount)
    {
        outScratch.clear();
        return r;
    }

    const int32 res = static_cast<int32>(geo.TileRes);
    x0 = std::clamp(x0, 0, res - 1);
    z0 = std::clamp(z0, 0, res - 1);
    x1 = std::clamp(x1, 0, res - 1);
    z1 = std::clamp(z1, 0, res - 1);
    if (x1 < x0 || z1 < z0)
    {
        outScratch.clear();
        return r;
    }

    // Slot-local texel span. Interior tile texel t lives at slot texel (kAtlasApron + t); a region
    // that reaches the tile edge extends one texel further into the apron gutter, which duplicates
    // the edge — so hardware bilinear straddle stays safe after a partial upload.
    const int32 apron = static_cast<int32>(kAtlasApron);
    const int32 stride = static_cast<int32>(geo.SlotStride);
    const int32 sx0 = (x0 == 0) ? 0 : x0 + apron;
    const int32 sy0 = (z0 == 0) ? 0 : z0 + apron;
    const int32 sx1 = (x1 == res - 1) ? stride - 1 : x1 + apron;
    const int32 sy1 = (z1 == res - 1) ? stride - 1 : z1 + apron;

    const uint32 w = static_cast<uint32>(sx1 - sx0 + 1);
    const uint32 h = static_cast<uint32>(sy1 - sy0 + 1);
    outScratch.resize(static_cast<size_t>(w) * h * bytesPerTexel);

    for (int32 sy = sy0; sy <= sy1; ++sy)
    {
        // apron rows/cols read the clamped interior edge (identical to PackTileBytesIntoSlot's dup).
        const int32 tz = std::clamp(sy - apron, 0, res - 1);
        for (int32 sx = sx0; sx <= sx1; ++sx)
        {
            const int32 tx = std::clamp(sx - apron, 0, res - 1);
            const size_t dst = (static_cast<size_t>(sy - sy0) * w + (sx - sx0)) * bytesPerTexel;
            const size_t src = (static_cast<size_t>(tz) * res + tx) * bytesPerTexel;
            std::memcpy(&outScratch[dst], tileData + src, bytesPerTexel);
        }
    }

    r.DstTexelX = geo.SlotOriginTexelX(slot) + static_cast<uint32>(sx0);
    r.DstTexelY = geo.SlotOriginTexelY(slot) + static_cast<uint32>(sy0);
    r.Width = w;
    r.Height = h;
    return r;
}

void BuildCoarseFieldU8(uint32 coarseDim, uint32 tilesX, uint32 tilesZ, uint32 channels,
                        const uint8* fallback,
                        const std::function<CoarseTileSourceU8(int32 tx, int32 tz)>& getTile,
                        std::vector<uint8>& out)
{
    if (coarseDim == 0 || tilesX == 0 || tilesZ == 0 || channels == 0 || !getTile)
    {
        out.clear();
        return;
    }
    out.assign(static_cast<size_t>(coarseDim) * coarseDim * channels, 0);
    if (fallback)
        for (size_t i = 0; i < static_cast<size_t>(coarseDim) * coarseDim; ++i)
            std::memcpy(&out[i * channels], fallback, channels);

    // Per-channel bilinear read of a channels-interleaved uint8 grid, rounded.
    auto sampleU8 = [](const uint8* grid, uint32 w, uint32 h, uint32 ch, uint32 c, float32 tx, float32 ty) {
        tx = std::clamp(tx, 0.0f, static_cast<float32>(w - 1));
        ty = std::clamp(ty, 0.0f, static_cast<float32>(h - 1));
        const uint32 x0 = static_cast<uint32>(std::floor(tx)), y0 = static_cast<uint32>(std::floor(ty));
        const uint32 x1 = std::min(x0 + 1u, w - 1u), y1 = std::min(y0 + 1u, h - 1u);
        const float32 fx = tx - static_cast<float32>(x0), fy = ty - static_cast<float32>(y0);
        auto tap = [&](uint32 x, uint32 y) {
            return static_cast<float32>(grid[(static_cast<size_t>(y) * w + x) * ch + c]);
        };
        const float32 a = tap(x0, y0) + (tap(x1, y0) - tap(x0, y0)) * fx;
        const float32 b = tap(x0, y1) + (tap(x1, y1) - tap(x0, y1)) * fx;
        return static_cast<uint8>(std::clamp(a + (b - a) * fy + 0.5f, 0.0f, 255.0f));
    };

    const float32 invCoarse = coarseDim > 1u ? 1.0f / static_cast<float32>(coarseDim - 1u) : 0.0f;
    for (uint32 cz = 0; cz < coarseDim; ++cz)
    {
        const float32 v = static_cast<float32>(cz) * invCoarse;
        const float32 tfz = v * static_cast<float32>(tilesZ);
        const int32 tz = std::clamp(static_cast<int32>(tfz), 0, static_cast<int32>(tilesZ) - 1);
        const float32 lv = std::clamp(tfz - static_cast<float32>(tz), 0.0f, 1.0f);
        for (uint32 cx = 0; cx < coarseDim; ++cx)
        {
            const float32 u = static_cast<float32>(cx) * invCoarse;
            const float32 tfx = u * static_cast<float32>(tilesX);
            const int32 tx = std::clamp(static_cast<int32>(tfx), 0, static_cast<int32>(tilesX) - 1);
            const float32 lu = std::clamp(tfx - static_cast<float32>(tx), 0.0f, 1.0f);
            const CoarseTileSourceU8 src = getTile(tx, tz);
            if (src.Data && src.Width >= 2 && src.Height >= 2 && src.Channels == channels)
            {
                const size_t base = (static_cast<size_t>(cz) * coarseDim + cx) * channels;
                for (uint32 c = 0; c < channels; ++c)
                    out[base + c] = sampleU8(src.Data, src.Width, src.Height, channels, c,
                                             lu * static_cast<float32>(src.Width - 1u),
                                             lv * static_cast<float32>(src.Height - 1u));
            }
        }
    }
}

} // namespace GameEngine::TerrainECS
