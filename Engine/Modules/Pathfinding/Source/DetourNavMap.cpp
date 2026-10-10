#include "Pathfinding/DetourNavMap.h"
#include "Pathfinding/PathBuffer.h"

#include <cmath>
#include <cstring>
#include <vector>

#if GE_PATHFINDING_BACKEND_DETOUR
#include <Recast.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#endif

namespace GameEngine::Pathfinding
{

#if GE_PATHFINDING_BACKEND_DETOUR

static_assert(sizeof(int) == sizeof(uint32), "Recast API expects int-sized indices");

static constexpr int32 kMaxPathPolys = 256;
static constexpr int32 kMaxStraightPathPoints = 256;
static constexpr float32 kPolySearchExtent[3] = {2.0f, 4.0f, 2.0f};

struct DetourNavMap::Impl
{
    rcPolyMesh* PolyMesh = nullptr;
    rcPolyMeshDetail* DetailMesh = nullptr;
    dtNavMesh* NavMesh = nullptr;
    dtNavMeshQuery* NavQuery = nullptr;
    dtQueryFilter Filter;

    std::vector<float32> DebugVertices;
    std::vector<uint32> DebugIndices;

    ~Impl()
    {
        if (NavQuery)
        {
            dtFreeNavMeshQuery(NavQuery);
            NavQuery = nullptr;
        }
        if (NavMesh)
        {
            dtFreeNavMesh(NavMesh);
            NavMesh = nullptr;
        }
        if (DetailMesh)
        {
            rcFreePolyMeshDetail(DetailMesh);
            DetailMesh = nullptr;
        }
        if (PolyMesh)
        {
            rcFreePolyMesh(PolyMesh);
            PolyMesh = nullptr;
        }
    }

    void BuildDebugMesh()
    {
        DebugVertices.clear();
        DebugIndices.clear();

        if (!NavMesh)
            return;

        // Extract debug geometry from the navmesh (use const pointer to
        // select the public const overload of getTile).
        const dtNavMesh* navMeshConst = NavMesh;
        for (int32 i = 0; i < navMeshConst->getMaxTiles(); ++i)
        {
            const dtMeshTile* tile = navMeshConst->getTile(i);
            if (!tile || !tile->header)
                continue;

            const uint32 baseVertex = static_cast<uint32>(DebugVertices.size() / 3);

            for (int32 v = 0; v < tile->header->vertCount; ++v)
            {
                const float32* vert = &tile->verts[v * 3];
                DebugVertices.push_back(vert[0]);
                DebugVertices.push_back(vert[1]);
                DebugVertices.push_back(vert[2]);
            }

            for (int32 p = 0; p < tile->header->polyCount; ++p)
            {
                const dtPoly& poly = tile->polys[p];
                if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
                    continue;

                // Triangulate the polygon as a fan
                for (int32 t = 2; t < static_cast<int32>(poly.vertCount); ++t)
                {
                    DebugIndices.push_back(baseVertex + poly.verts[0]);
                    DebugIndices.push_back(baseVertex + poly.verts[t - 1]);
                    DebugIndices.push_back(baseVertex + poly.verts[t]);
                }
            }
        }
    }
};

DetourNavMap::DetourNavMap()
    : m_Impl(std::make_unique<Impl>())
{
}

DetourNavMap::~DetourNavMap() = default;
DetourNavMap::DetourNavMap(DetourNavMap&&) noexcept = default;
DetourNavMap& DetourNavMap::operator=(DetourNavMap&&) noexcept = default;

bool DetourNavMap::Build(const NavMeshSettings& settings, const InputGeometry& geometry)
{
    if (!geometry.Vertices || geometry.VertexCount == 0 ||
        !geometry.Indices || geometry.TriangleCount == 0)
    {
        return false;
    }

    m_Impl = std::make_unique<Impl>();

    rcContext ctx;

    // Step 1: Calculate bounds
    float32 bmin[3], bmax[3];
    rcCalcBounds(geometry.Vertices, static_cast<int32>(geometry.VertexCount), bmin, bmax);

    // Step 2: Create heightfield
    rcConfig cfg = {};
    cfg.cs = settings.CellSize;
    cfg.ch = settings.CellHeight;
    cfg.walkableSlopeAngle = settings.AgentMaxSlope;
    cfg.walkableHeight = static_cast<int32>(std::ceil(settings.AgentHeight / settings.CellHeight));
    cfg.walkableClimb = static_cast<int32>(std::floor(settings.AgentMaxClimb / settings.CellHeight));
    cfg.walkableRadius = static_cast<int32>(std::ceil(settings.AgentRadius / settings.CellSize));
    cfg.maxEdgeLen = static_cast<int32>(settings.EdgeMaxLen / settings.CellSize);
    cfg.maxSimplificationError = settings.EdgeMaxError;
    cfg.minRegionArea = static_cast<int32>(settings.RegionMinSize * settings.RegionMinSize);
    cfg.mergeRegionArea = static_cast<int32>(settings.RegionMergeSize * settings.RegionMergeSize);
    cfg.maxVertsPerPoly = settings.VertsPerPoly;
    cfg.detailSampleDist = settings.DetailSampleDist < 0.9f ? 0.0f : settings.CellSize * settings.DetailSampleDist;
    cfg.detailSampleMaxError = settings.CellHeight * settings.DetailSampleMaxError;
    rcVcopy(cfg.bmin, bmin);
    rcVcopy(cfg.bmax, bmax);
    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);

    rcHeightfield* heightfield = rcAllocHeightfield();
    if (!heightfield)
        return false;

    if (!rcCreateHeightfield(&ctx, *heightfield, cfg.width, cfg.height,
                             cfg.bmin, cfg.bmax, cfg.cs, cfg.ch))
    {
        rcFreeHeightField(heightfield);
        return false;
    }

    // Step 3: Rasterize triangles
    const int32 triCount = static_cast<int32>(geometry.TriangleCount);
    std::vector<uint8> triAreas(triCount, 0);
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle,
                            geometry.Vertices, static_cast<int32>(geometry.VertexCount),
                            reinterpret_cast<const int32*>(geometry.Indices), triCount,
                            triAreas.data());
    if (!rcRasterizeTriangles(&ctx, geometry.Vertices, static_cast<int32>(geometry.VertexCount),
                              reinterpret_cast<const int32*>(geometry.Indices), triAreas.data(),
                              triCount, *heightfield, cfg.walkableClimb))
    {
        rcFreeHeightField(heightfield);
        return false;
    }

    // Step 4: Filter walkable surfaces
    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *heightfield);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *heightfield);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *heightfield);

    // Step 5: Build compact heightfield
    rcCompactHeightfield* compactHf = rcAllocCompactHeightfield();
    if (!compactHf)
    {
        rcFreeHeightField(heightfield);
        return false;
    }

    if (!rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb,
                                   *heightfield, *compactHf))
    {
        rcFreeCompactHeightfield(compactHf);
        rcFreeHeightField(heightfield);
        return false;
    }

    rcFreeHeightField(heightfield);

    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *compactHf))
    {
        rcFreeCompactHeightfield(compactHf);
        return false;
    }

    // Step 6: Build regions
    if (!rcBuildDistanceField(&ctx, *compactHf))
    {
        rcFreeCompactHeightfield(compactHf);
        return false;
    }

    if (!rcBuildRegions(&ctx, *compactHf, 0, cfg.minRegionArea, cfg.mergeRegionArea))
    {
        rcFreeCompactHeightfield(compactHf);
        return false;
    }

    // Step 7: Build contours
    rcContourSet* contourSet = rcAllocContourSet();
    if (!contourSet)
    {
        rcFreeCompactHeightfield(compactHf);
        return false;
    }

    if (!rcBuildContours(&ctx, *compactHf, cfg.maxSimplificationError,
                         cfg.maxEdgeLen, *contourSet))
    {
        rcFreeContourSet(contourSet);
        rcFreeCompactHeightfield(compactHf);
        return false;
    }

    // Step 8: Build poly mesh
    m_Impl->PolyMesh = rcAllocPolyMesh();
    if (!m_Impl->PolyMesh)
    {
        rcFreeContourSet(contourSet);
        rcFreeCompactHeightfield(compactHf);
        return false;
    }

    if (!rcBuildPolyMesh(&ctx, *contourSet, cfg.maxVertsPerPoly, *m_Impl->PolyMesh))
    {
        rcFreeContourSet(contourSet);
        rcFreeCompactHeightfield(compactHf);
        return false;
    }

    // Step 9: Build detail mesh
    m_Impl->DetailMesh = rcAllocPolyMeshDetail();
    if (!m_Impl->DetailMesh)
    {
        rcFreeContourSet(contourSet);
        rcFreeCompactHeightfield(compactHf);
        return false;
    }

    if (!rcBuildPolyMeshDetail(&ctx, *m_Impl->PolyMesh, *compactHf,
                                cfg.detailSampleDist, cfg.detailSampleMaxError,
                                *m_Impl->DetailMesh))
    {
        rcFreeContourSet(contourSet);
        rcFreeCompactHeightfield(compactHf);
        return false;
    }

    rcFreeContourSet(contourSet);
    rcFreeCompactHeightfield(compactHf);

    // Step 10: Create Detour navmesh
    for (int32 i = 0; i < m_Impl->PolyMesh->npolys; ++i)
    {
        m_Impl->PolyMesh->flags[i] = 1;
    }

    dtNavMeshCreateParams params = {};
    params.verts = m_Impl->PolyMesh->verts;
    params.vertCount = m_Impl->PolyMesh->nverts;
    params.polys = m_Impl->PolyMesh->polys;
    params.polyAreas = m_Impl->PolyMesh->areas;
    params.polyFlags = m_Impl->PolyMesh->flags;
    params.polyCount = m_Impl->PolyMesh->npolys;
    params.nvp = m_Impl->PolyMesh->nvp;
    params.detailMeshes = m_Impl->DetailMesh->meshes;
    params.detailVerts = m_Impl->DetailMesh->verts;
    params.detailVertsCount = m_Impl->DetailMesh->nverts;
    params.detailTris = m_Impl->DetailMesh->tris;
    params.detailTriCount = m_Impl->DetailMesh->ntris;
    params.walkableHeight = settings.AgentHeight;
    params.walkableRadius = settings.AgentRadius;
    params.walkableClimb = settings.AgentMaxClimb;
    rcVcopy(params.bmin, m_Impl->PolyMesh->bmin);
    rcVcopy(params.bmax, m_Impl->PolyMesh->bmax);
    params.cs = cfg.cs;
    params.ch = cfg.ch;
    params.buildBvTree = true;

    uint8* navData = nullptr;
    int32 navDataSize = 0;
    if (!dtCreateNavMeshData(&params, &navData, &navDataSize))
        return false;

    m_Impl->NavMesh = dtAllocNavMesh();
    if (!m_Impl->NavMesh)
    {
        dtFree(navData);
        return false;
    }

    dtStatus status = m_Impl->NavMesh->init(navData, navDataSize, DT_TILE_FREE_DATA);
    if (dtStatusFailed(status))
    {
        dtFree(navData);
        dtFreeNavMesh(m_Impl->NavMesh);
        m_Impl->NavMesh = nullptr;
        return false;
    }

    // Step 11: Create query object
    m_Impl->NavQuery = dtAllocNavMeshQuery();
    if (!m_Impl->NavQuery)
        return false;

    status = m_Impl->NavQuery->init(m_Impl->NavMesh, 2048);
    if (dtStatusFailed(status))
    {
        dtFreeNavMeshQuery(m_Impl->NavQuery);
        m_Impl->NavQuery = nullptr;
        return false;
    }

    m_Impl->BuildDebugMesh();
    return true;
}

bool DetourNavMap::IsBuilt() const
{
    return m_Impl && m_Impl->NavMesh != nullptr && m_Impl->NavQuery != nullptr;
}

PathStatus DetourNavMap::FindPath(const PathRequest& request, PathBuffer& pathBuffer, PathHandle& outPath) const
{
    if (!IsBuilt())
        return PathStatus::Failed;

    const float32 startPos[3] = {request.StartX, request.StartY, request.StartZ};
    const float32 endPos[3] = {request.EndX, request.EndY, request.EndZ};
    const float32 extent[3] = {request.AgentRadius * 4.0f, request.AgentHeight, request.AgentRadius * 4.0f};

    dtPolyRef startRef = 0, endRef = 0;
    float32 nearestStartPos[3], nearestEndPos[3];

    dtStatus status = m_Impl->NavQuery->findNearestPoly(startPos, extent, &m_Impl->Filter,
                                                         &startRef, nearestStartPos);
    if (dtStatusFailed(status) || startRef == 0)
        return PathStatus::Failed;

    status = m_Impl->NavQuery->findNearestPoly(endPos, extent, &m_Impl->Filter,
                                                &endRef, nearestEndPos);
    if (dtStatusFailed(status) || endRef == 0)
        return PathStatus::Failed;

    dtPolyRef pathPolys[kMaxPathPolys];
    int32 pathPolyCount = 0;
    const dtStatus findPathStatus = m_Impl->NavQuery->findPath(
        startRef, endRef, nearestStartPos, nearestEndPos,
        &m_Impl->Filter, pathPolys, &pathPolyCount, kMaxPathPolys);
    if (dtStatusFailed(findPathStatus) || pathPolyCount == 0)
        return PathStatus::Failed;

    float32 straightPath[kMaxStraightPathPoints * 3];
    uint8 straightPathFlags[kMaxStraightPathPoints];
    dtPolyRef straightPathPolys[kMaxStraightPathPoints];
    int32 straightPathCount = 0;

    status = m_Impl->NavQuery->findStraightPath(nearestStartPos, nearestEndPos,
                                                  pathPolys, pathPolyCount,
                                                  straightPath, straightPathFlags,
                                                  straightPathPolys, &straightPathCount,
                                                  kMaxStraightPathPoints);
    if (dtStatusFailed(status) || straightPathCount == 0)
        return PathStatus::Failed;

    std::vector<PathPoint> points(straightPathCount);
    for (int32 i = 0; i < straightPathCount; ++i)
    {
        points[i].X = straightPath[i * 3 + 0];
        points[i].Y = straightPath[i * 3 + 1];
        points[i].Z = straightPath[i * 3 + 2];
    }

    outPath = pathBuffer.AllocatePath(points.data(), static_cast<uint32>(points.size()));

    // Check the findPath status (not findStraightPath) for partial results.
    bool isPartial = dtStatusDetail(findPathStatus, DT_PARTIAL_RESULT);
    return isPartial ? PathStatus::Partial : PathStatus::Complete;
}

bool DetourNavMap::IsPointNavigable(float32 x, float32 y, float32 z, float32 radius) const
{
    if (!IsBuilt())
        return false;

    const float32 pos[3] = {x, y, z};
    const float32 extent[3] = {radius, radius * 2.0f, radius};

    dtPolyRef ref = 0;
    float32 nearestPos[3];
    dtStatus status = m_Impl->NavQuery->findNearestPoly(pos, extent, &m_Impl->Filter, &ref, nearestPos);
    if (dtStatusFailed(status) || ref == 0)
        return false;

    // Check XZ distance to nearest navigable point (height is checked separately
    // with a more generous tolerance since navmesh Y can differ from query Y due
    // to terrain slope within a polygon).
    const float32 dx = nearestPos[0] - x;
    const float32 dz = nearestPos[2] - z;
    const float32 distXZSq = dx * dx + dz * dz;
    const float32 dy = std::abs(nearestPos[1] - y);
    return distXZSq <= radius * radius && dy <= radius * 2.0f;
}

bool DetourNavMap::GetClosestNavigablePoint(float32 x, float32 y, float32 z, float32 searchRadius,
                                             float32& outX, float32& outY, float32& outZ) const
{
    if (!IsBuilt())
        return false;

    const float32 pos[3] = {x, y, z};
    const float32 extent[3] = {searchRadius, searchRadius, searchRadius};

    dtPolyRef ref = 0;
    float32 nearestPos[3];
    dtStatus status = m_Impl->NavQuery->findNearestPoly(pos, extent, &m_Impl->Filter, &ref, nearestPos);
    if (dtStatusFailed(status) || ref == 0)
        return false;

    // Reject results outside the requested search radius in the XZ plane
    // (Detour's extent is an AABB, so corner results can exceed the radius).
    const float32 dx = nearestPos[0] - x;
    const float32 dz = nearestPos[2] - z;
    if (dx * dx + dz * dz > searchRadius * searchRadius)
        return false;

    outX = nearestPos[0];
    outY = nearestPos[1];
    outZ = nearestPos[2];
    return true;
}

// NOTE: The start position is snapped to the nearest point on the navmesh via
// findNearestPoly before the raycast is performed. This means the actual ray
// origin may differ from (startX, startY, startZ) if the start is off-mesh.
// The returned hit point is along the snapped ray, not the original ray.
// Callers that need the exact ray origin should verify the start is on the
// navmesh (e.g., via IsPointNavigable) before calling Raycast.
bool DetourNavMap::Raycast(float32 startX, float32 startY, float32 startZ,
                            float32 endX, float32 endY, float32 endZ,
                            float32& hitX, float32& hitY, float32& hitZ) const
{
    if (!IsBuilt())
        return false;

    const float32 startPos[3] = {startX, startY, startZ};
    const float32 endPos[3] = {endX, endY, endZ};

    dtPolyRef startRef = 0;
    float32 nearestPos[3];
    dtStatus status = m_Impl->NavQuery->findNearestPoly(startPos, kPolySearchExtent,
                                                         &m_Impl->Filter, &startRef, nearestPos);
    if (dtStatusFailed(status) || startRef == 0)
        return false;

    float32 t = 0.0f;
    float32 hitNormal[3];
    dtPolyRef pathPolys[kMaxPathPolys];
    int32 pathPolyCount = 0;

    status = m_Impl->NavQuery->raycast(startRef, nearestPos, endPos, &m_Impl->Filter,
                                        &t, hitNormal, pathPolys, &pathPolyCount, kMaxPathPolys);
    if (dtStatusFailed(status))
        return false;

    if (t >= 1.0f)
        return false; // No hit, ray reached end

    hitX = nearestPos[0] + (endPos[0] - nearestPos[0]) * t;
    hitY = nearestPos[1] + (endPos[1] - nearestPos[1]) * t;
    hitZ = nearestPos[2] + (endPos[2] - nearestPos[2]) * t;
    return true;
}

// Serialization format (supports multi-tile navmeshes):
// [magic "DTNM"][version uint32][dtNavMeshParams (28 bytes)][tileCount uint32]
// For each tile: [tileRef uint64][dataSize uint32][tile data bytes]
static constexpr char kSerializeMagic[4] = {'D', 'T', 'N', 'M'};
static constexpr uint32 kSerializeVersion = 1;

bool DetourNavMap::Serialize(std::vector<uint8>& outData) const
{
    if (!IsBuilt())
        return false;

    const dtNavMesh* navMesh = m_Impl->NavMesh;
    const dtNavMeshParams* meshParams = navMesh->getParams();

    // Count active tiles and compute total size
    uint32 tileCount = 0;
    size_t totalTileDataSize = 0;
    for (int32 i = 0; i < navMesh->getMaxTiles(); ++i)
    {
        const dtMeshTile* tile = navMesh->getTile(i);
        if (!tile || !tile->header || tile->dataSize == 0)
            continue;
        ++tileCount;
        totalTileDataSize += tile->dataSize;
    }

    if (tileCount == 0)
        return false;

    // Header: magic(4) + version(4) + params(28) + tileCount(4) = 40
    // Per tile: tileRef(8) + dataSize(4) + data(variable)
    const size_t headerSize = 4 + sizeof(uint32) + sizeof(dtNavMeshParams) + sizeof(uint32);
    const size_t perTileOverhead = sizeof(uint64) + sizeof(uint32);
    outData.resize(headerSize + tileCount * perTileOverhead + totalTileDataSize);

    size_t offset = 0;
    auto writeBytes = [&](const void* src, size_t count) {
        std::memcpy(outData.data() + offset, src, count);
        offset += count;
    };

    writeBytes(kSerializeMagic, 4);
    writeBytes(&kSerializeVersion, sizeof(uint32));
    writeBytes(meshParams, sizeof(dtNavMeshParams));
    writeBytes(&tileCount, sizeof(uint32));

    for (int32 i = 0; i < navMesh->getMaxTiles(); ++i)
    {
        const dtMeshTile* tile = navMesh->getTile(i);
        if (!tile || !tile->header || tile->dataSize == 0)
            continue;

        const uint64 tileRef = static_cast<uint64>(navMesh->getTileRef(tile));
        const uint32 dataSize = static_cast<uint32>(tile->dataSize);
        writeBytes(&tileRef, sizeof(uint64));
        writeBytes(&dataSize, sizeof(uint32));
        writeBytes(tile->data, dataSize);
    }

    return true;
}

bool DetourNavMap::Deserialize(const uint8* data, uint32 size)
{
    if (!data || size == 0)
        return false;

    const size_t minHeaderSize = 4 + sizeof(uint32) + sizeof(dtNavMeshParams) + sizeof(uint32);
    if (size < minHeaderSize)
        return false;

    size_t offset = 0;
    auto readBytes = [&](void* dst, size_t count) -> bool {
        if (offset + count > size)
            return false;
        std::memcpy(dst, data + offset, count);
        offset += count;
        return true;
    };

    // Check magic
    char magic[4];
    if (!readBytes(magic, 4) || std::memcmp(magic, kSerializeMagic, 4) != 0)
    {
        // Try legacy single-tile format (no header, raw Detour tile data).
        // The old format wrote tile data directly with no magic/version.
        return DeserializeLegacy(data, size);
    }

    uint32 version = 0;
    if (!readBytes(&version, sizeof(uint32)) || version != kSerializeVersion)
        return false;

    dtNavMeshParams meshParams;
    if (!readBytes(&meshParams, sizeof(dtNavMeshParams)))
        return false;

    uint32 tileCount = 0;
    if (!readBytes(&tileCount, sizeof(uint32)) || tileCount == 0)
        return false;

    m_Impl = std::make_unique<Impl>();

    m_Impl->NavMesh = dtAllocNavMesh();
    if (!m_Impl->NavMesh)
        return false;

    dtStatus status = m_Impl->NavMesh->init(&meshParams);
    if (dtStatusFailed(status))
    {
        dtFreeNavMesh(m_Impl->NavMesh);
        m_Impl->NavMesh = nullptr;
        return false;
    }

    uint32 tilesLoaded = 0;
    for (uint32 t = 0; t < tileCount; ++t)
    {
        uint64 tileRef = 0;
        uint32 dataSize = 0;
        if (!readBytes(&tileRef, sizeof(uint64)) || !readBytes(&dataSize, sizeof(uint32)))
        {
            dtFreeNavMesh(m_Impl->NavMesh);
            m_Impl->NavMesh = nullptr;
            return false;
        }

        if (dataSize == 0 || offset + dataSize > size)
        {
            dtFreeNavMesh(m_Impl->NavMesh);
            m_Impl->NavMesh = nullptr;
            return false;
        }

        // Detour takes ownership of tile data, so allocate a copy
        uint8* tileData = static_cast<uint8*>(dtAlloc(dataSize, DT_ALLOC_PERM));
        if (!tileData)
        {
            dtFreeNavMesh(m_Impl->NavMesh);
            m_Impl->NavMesh = nullptr;
            return false;
        }
        std::memcpy(tileData, data + offset, dataSize);
        offset += dataSize;

        status = m_Impl->NavMesh->addTile(tileData, static_cast<int32>(dataSize),
                                            DT_TILE_FREE_DATA,
                                            static_cast<dtTileRef>(tileRef), nullptr);
        if (dtStatusFailed(status))
            dtFree(tileData);
        else
            ++tilesLoaded;
    }

    if (tilesLoaded == 0)
    {
        dtFreeNavMesh(m_Impl->NavMesh);
        m_Impl->NavMesh = nullptr;
        return false;
    }

    m_Impl->NavQuery = dtAllocNavMeshQuery();
    if (!m_Impl->NavQuery)
        return false;

    status = m_Impl->NavQuery->init(m_Impl->NavMesh, 2048);
    if (dtStatusFailed(status))
    {
        dtFreeNavMeshQuery(m_Impl->NavQuery);
        m_Impl->NavQuery = nullptr;
        return false;
    }

    m_Impl->BuildDebugMesh();
    return true;
}

bool DetourNavMap::DeserializeLegacy(const uint8* data, uint32 size)
{
    m_Impl = std::make_unique<Impl>();

    m_Impl->NavMesh = dtAllocNavMesh();
    if (!m_Impl->NavMesh)
        return false;

    uint8* navData = static_cast<uint8*>(dtAlloc(size, DT_ALLOC_PERM));
    if (!navData)
    {
        dtFreeNavMesh(m_Impl->NavMesh);
        m_Impl->NavMesh = nullptr;
        return false;
    }
    std::memcpy(navData, data, size);

    dtStatus status = m_Impl->NavMesh->init(navData, size, DT_TILE_FREE_DATA);
    if (dtStatusFailed(status))
    {
        dtFree(navData);
        dtFreeNavMesh(m_Impl->NavMesh);
        m_Impl->NavMesh = nullptr;
        return false;
    }

    m_Impl->NavQuery = dtAllocNavMeshQuery();
    if (!m_Impl->NavQuery)
        return false;

    status = m_Impl->NavQuery->init(m_Impl->NavMesh, 2048);
    if (dtStatusFailed(status))
    {
        dtFreeNavMeshQuery(m_Impl->NavQuery);
        m_Impl->NavQuery = nullptr;
        return false;
    }

    m_Impl->BuildDebugMesh();
    return true;
}

uint32 DetourNavMap::GetDebugVertexCount() const
{
    return m_Impl ? static_cast<uint32>(m_Impl->DebugVertices.size() / 3) : 0;
}

uint32 DetourNavMap::GetDebugTriangleCount() const
{
    return m_Impl ? static_cast<uint32>(m_Impl->DebugIndices.size() / 3) : 0;
}

const float32* DetourNavMap::GetDebugVertices() const
{
    return (m_Impl && !m_Impl->DebugVertices.empty()) ? m_Impl->DebugVertices.data() : nullptr;
}

const uint32* DetourNavMap::GetDebugIndices() const
{
    return (m_Impl && !m_Impl->DebugIndices.empty()) ? m_Impl->DebugIndices.data() : nullptr;
}

void* DetourNavMap::GetDetourNavMesh() const
{
    return m_Impl ? m_Impl->NavMesh : nullptr;
}

void* DetourNavMap::GetDetourNavMeshQuery() const
{
    return m_Impl ? m_Impl->NavQuery : nullptr;
}

#else // !GE_PATHFINDING_BACKEND_DETOUR

struct DetourNavMap::Impl
{
};

DetourNavMap::DetourNavMap()
    : m_Impl(std::make_unique<Impl>())
{
}

DetourNavMap::~DetourNavMap() = default;
DetourNavMap::DetourNavMap(DetourNavMap&&) noexcept = default;
DetourNavMap& DetourNavMap::operator=(DetourNavMap&&) noexcept = default;

bool DetourNavMap::Build(const NavMeshSettings& /*settings*/, const InputGeometry& /*geometry*/)
{
    return false;
}

bool DetourNavMap::IsBuilt() const
{
    return false;
}

PathStatus DetourNavMap::FindPath(const PathRequest& /*request*/, PathBuffer& /*pathBuffer*/, PathHandle& /*outPath*/) const
{
    return PathStatus::Failed;
}

bool DetourNavMap::IsPointNavigable(float32 /*x*/, float32 /*y*/, float32 /*z*/, float32 /*radius*/) const
{
    return false;
}

bool DetourNavMap::GetClosestNavigablePoint(float32 /*x*/, float32 /*y*/, float32 /*z*/, float32 /*searchRadius*/,
                                             float32& /*outX*/, float32& /*outY*/, float32& /*outZ*/) const
{
    return false;
}

bool DetourNavMap::Raycast(float32 /*startX*/, float32 /*startY*/, float32 /*startZ*/,
                            float32 /*endX*/, float32 /*endY*/, float32 /*endZ*/,
                            float32& /*hitX*/, float32& /*hitY*/, float32& /*hitZ*/) const
{
    return false;
}

bool DetourNavMap::Serialize(std::vector<uint8>& /*outData*/) const
{
    return false;
}

bool DetourNavMap::Deserialize(const uint8* /*data*/, uint32 /*size*/)
{
    return false;
}

uint32 DetourNavMap::GetDebugVertexCount() const
{
    return 0;
}

uint32 DetourNavMap::GetDebugTriangleCount() const
{
    return 0;
}

const float32* DetourNavMap::GetDebugVertices() const
{
    return nullptr;
}

const uint32* DetourNavMap::GetDebugIndices() const
{
    return nullptr;
}

void* DetourNavMap::GetDetourNavMesh() const
{
    return nullptr;
}

void* DetourNavMap::GetDetourNavMeshQuery() const
{
    return nullptr;
}

#endif // GE_PATHFINDING_BACKEND_DETOUR

} // namespace GameEngine::Pathfinding
