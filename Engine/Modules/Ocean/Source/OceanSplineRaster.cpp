#include "Ocean/OceanSplineRaster.h"
#include "Ocean/OceanShaderProgram.h"
#include "Rendering/Materials/ShaderLayoutShape.h"
#include "OceanShaderDirectory.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Logger/Logger.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <limits>
#include <functional>
namespace GameEngine::Ocean
{
using namespace Rendering;
std::vector<OceanRibbonTriangleGPU> BuildOceanRibbon(const std::vector<OceanRibbonPoint> &points,
                                                     const OceanRibbonStyle &style)
{
    std::vector<OceanRibbonTriangleGPU> out;
    if (points.size() < 2 || style.Width <= 0)
        return out;
    const float feather = std::max(style.Feather, 0.0f);
    const float apron = std::max(style.Width, 0.5f * feather);
    const float reach = style.Width + apron;
    for (size_t i = 1; i < points.size(); ++i)
    {
        const auto &a = points[i - 1];
        const auto &b = points[i];
        if (std::hypot(b.X - a.X, b.Z - a.Z) < 1e-5f)
            continue;
        float vertex[4][4]{}, velocity[4][4]{};
        for (uint32 j = 0; j < 4; ++j)
        {
            const auto &p = j < 2 ? a : b;
            float len = std::hypot(p.TX, p.TZ);
            const float tx = len > 1e-6f ? p.TX / len : 0, tz = len > 1e-6f ? p.TZ / len : 1;
            const float side = (j % 2) ? 1.0f : -1.0f;
            vertex[j][0] = p.X + tz * reach * side;
            vertex[j][1] = p.Z - tx * reach * side;
            vertex[j][2] = side * reach / style.Width;
            vertex[j][3] = p.Y;
            velocity[j][0] = tx * style.FlowSpeed;
            velocity[j][1] = tz * style.FlowSpeed;
        }
        for (const auto &ids : {std::array<uint32, 3>{0, 1, 2}, std::array<uint32, 3>{2, 1, 3}})
        {
            OceanRibbonTriangleGPU t{};
            std::copy_n(vertex[ids[0]], 4, t.A);
            std::copy_n(vertex[ids[1]], 4, t.B);
            std::copy_n(vertex[ids[2]], 4, t.C);
            std::copy_n(velocity[ids[0]], 4, t.FlowA);
            std::copy_n(velocity[ids[1]], 4, t.FlowB);
            std::copy_n(velocity[ids[2]], 4, t.FlowC);
            std::copy_n(style.Color, 4, t.Color);
            t.Values[0] = style.Width;
            t.Values[1] = std::max(style.Depth, 0.0f);
            t.Values[2] = feather;
            t.Values[3] = std::max(style.UnderwaterDepth, 0.0f);
            t.FlowA[2] = std::max(style.DepthFeather, 0.0f);
            t.FlowA[3] = std::max(style.DepthSaturation, 0.0f);
            t.FlowB[2] = apron;
            t.Meta[0] = style.Flags;
            t.Meta[1] = style.Id;
            out.push_back(t);
        }
    }
    return out;
}
bool SampleOceanRibbonTriangle(const OceanRibbonTriangleGPU &t, float x, float z, float bary[3])
{
    const float bx = t.B[0] - t.A[0], bz = t.B[1] - t.A[1], cx = t.C[0] - t.A[0], cz = t.C[1] - t.A[1];
    const float det = bx * cz - bz * cx;
    if (std::abs(det) < 1e-8f)
        return false;
    bary[1] = ((x - t.A[0]) * cz - (z - t.A[1]) * cx) / det;
    bary[2] = (bx * (z - t.A[1]) - bz * (x - t.A[0])) / det;
    bary[0] = 1 - bary[1] - bary[2];
    return bary[0] >= -1e-6f && bary[1] >= -1e-6f && bary[2] >= -1e-6f;
}
float OceanRibbonEdgeDistance(const OceanRibbonTriangleGPU &t, const float b[3])
{
    const float cross = t.A[2] * b[0] + t.B[2] * b[1] + t.C[2] * b[2];
    return (1 - std::abs(cross)) * t.Values[0];
}
namespace
{
// The inclusive dispatch-tile rectangle (8x8 texels per tile) a triangle's
// bounds overlap in one cascade of the field.
struct RibbonTileRange
{
    int X0, Z0, X1, Z1;
};
RibbonTileRange TileRangeOf(const OceanRibbonTriangleGPU &t, const OceanCascadeLayoutGPU &layout, uint32 lod,
                            uint32 groups)
{
    const float minX = std::min({t.A[0], t.B[0], t.C[0]}), maxX = std::max({t.A[0], t.B[0], t.C[0]});
    const float minZ = std::min({t.A[1], t.B[1], t.C[1]}), maxZ = std::max({t.A[1], t.B[1], t.C[1]});
    const auto *cascade = layout.CascadeOriginScale[lod];
    const float tile = cascade[2] * 8;
    const int last = static_cast<int>(groups) - 1;
    return {std::max(0, static_cast<int>(std::floor((minX - cascade[0]) / tile))),
            std::max(0, static_cast<int>(std::floor((minZ - cascade[1]) / tile))),
            std::min(last, static_cast<int>(std::floor((maxX - cascade[0]) / tile))),
            std::min(last, static_cast<int>(std::floor((maxZ - cascade[1]) / tile)))};
}
// The inclusive query-grid cell rectangle a triangle's XZ bounds overlap.
template <typename Grid>
void GridCellRange(const OceanRibbonTriangleGPU &t, const Grid &grid, uint32 range[4])
{
    const float lowX = std::min({t.A[0], t.B[0], t.C[0]}), highX = std::max({t.A[0], t.B[0], t.C[0]});
    const float lowZ = std::min({t.A[1], t.B[1], t.C[1]}), highZ = std::max({t.A[1], t.B[1], t.C[1]});
    const auto cellOf = [&](float value, float origin, uint32 count) {
        return std::min(static_cast<uint32>(std::max((value - origin) / grid.CellSize, 0.0f)), count - 1u);
    };
    range[0] = cellOf(lowX, grid.OriginX, grid.Width);
    range[1] = cellOf(lowZ, grid.OriginZ, grid.Height);
    range[2] = cellOf(highX, grid.OriginX, grid.Width);
    range[3] = cellOf(highZ, grid.OriginZ, grid.Height);
}
} // namespace
static float RibbonFlowWeight(const OceanRibbonTriangleGPU &t, float edge)
{
    if (t.Values[2] <= 1e-5f)
        return 1;
    const float w = std::clamp(edge / t.Values[2], 0.0f, 1.0f);
    return w * w * (3 - 2 * w);
}
// The innermost triangle of one ribbon at a query point.
struct RibbonFlowHit
{
    const OceanRibbonTriangleGPU *Triangle = nullptr;
    float Bary[3]{};
    float Edge = 0;
};
// Adds a ribbon's current when the point lies inside its true edge.
static void AddRibbonFlow(const RibbonFlowHit &hit, OceanCurrentSample &result)
{
    if (!hit.Triangle || hit.Edge < 0)
        return;
    const auto &t = *hit.Triangle;
    const float *b = hit.Bary;
    const float w = RibbonFlowWeight(t, hit.Edge);
    result.Valid = true;
    result.FlowX += (t.FlowA[0] * b[0] + t.FlowB[0] * b[1] + t.FlowC[0] * b[2]) * w;
    result.FlowZ += (t.FlowA[1] * b[0] + t.FlowB[1] * b[1] + t.FlowC[1] * b[2]) * w;
}
bool OceanSplineRaster::SetTriangles(std::vector<OceanRibbonTriangleGPU> triangles)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (triangles.size() == m_Triangles.size() && (triangles.empty() ||
        std::memcmp(triangles.data(), m_Triangles.data(), triangles.size() * sizeof(OceanRibbonTriangleGPU)) == 0))
        return false;
    m_Triangles = std::move(triangles);
    ++m_Revision;
    m_Flags = 0;
    std::vector<OceanRibbonTriangleGPU> underwater;
    m_HasSurfaceBounds = false;
    m_HasClipBounds = false;
    for (const auto &t : m_Triangles)
    {
        m_Flags |= t.Meta[0];
        if (t.Meta[0] & 32u)
            underwater.push_back(t);
        if (t.Meta[0] & 4u)
            for (const float *v : {t.A, t.B, t.C})
            {
                m_ClipBounds[0] = m_HasClipBounds ? std::min(m_ClipBounds[0], v[0]) : v[0];
                m_ClipBounds[1] = m_HasClipBounds ? std::min(m_ClipBounds[1], v[1]) : v[1];
                m_ClipBounds[2] = m_HasClipBounds ? std::max(m_ClipBounds[2], v[0]) : v[0];
                m_ClipBounds[3] = m_HasClipBounds ? std::max(m_ClipBounds[3], v[1]) : v[1];
                m_HasClipBounds = true;
            }
        if ((t.Meta[0] & 16u) == 0u)
            continue;
        for (const float *v : {t.A, t.B, t.C})
        {
            m_SurfaceBounds[0] = m_HasSurfaceBounds ? std::min(m_SurfaceBounds[0], v[0]) : v[0];
            m_SurfaceBounds[1] = m_HasSurfaceBounds ? std::min(m_SurfaceBounds[1], v[1]) : v[1];
            m_SurfaceBounds[2] = m_HasSurfaceBounds ? std::max(m_SurfaceBounds[2], v[0]) : v[0];
            m_SurfaceBounds[3] = m_HasSurfaceBounds ? std::max(m_SurfaceBounds[3], v[1]) : v[1];
            m_HasSurfaceBounds = true;
        }
    }
    m_UnderwaterTree.clear();
    std::function<void(size_t, size_t)> build = [&](size_t begin, size_t end) {
        const auto nodeIndex = m_UnderwaterTree.size();
        OceanRibbonTriangleGPU node{};
        node.Meta[0] = 0x80000000u;
        for (uint32 axis = 0; axis < 3; ++axis)
        {
            node.A[axis] = std::numeric_limits<float>::max();
            node.B[axis] = -node.A[axis];
        }
        for (size_t i = begin; i < end; ++i)
            for (const float *v : {underwater[i].A, underwater[i].B, underwater[i].C})
            {
                const float xyz[] = {v[0], v[3], v[1]};
                for (uint32 axis = 0; axis < 3; ++axis)
                {
                    node.A[axis] =
                        std::min(node.A[axis], xyz[axis] - (axis == 1 ? underwater[i].Values[3] : 0));
                    node.B[axis] = std::max(node.B[axis], xyz[axis]);
                }
            }
        m_UnderwaterTree.push_back(node);
        if (end - begin <= 8)
            m_UnderwaterTree.insert(m_UnderwaterTree.end(), underwater.begin() + begin,
                                    underwater.begin() + end);
        else
        {
            const uint32 axis = node.B[0] - node.A[0] > node.B[2] - node.A[2] ? 0 : 1;
            const size_t middle = begin + (end - begin) / 2;
            std::nth_element(underwater.begin() + begin, underwater.begin() + middle,
                             underwater.begin() + end, [axis](const auto &a, const auto &b) {
                                 return a.A[axis] + a.B[axis] + a.C[axis] < b.A[axis] + b.B[axis] + b.C[axis];
                             });
            build(begin, middle);
            build(middle, end);
        }
        m_UnderwaterTree[nodeIndex].Meta[1] = static_cast<uint32>(m_UnderwaterTree.size());
    };
    if (!underwater.empty())
        build(0, underwater.size());
    BuildQueryGrid();
    return true;
}
void OceanSplineRaster::BuildQueryGrid()
{
    // About one triangle per cell on average, capped so a sparse, far-flung
    // ribbon set cannot allocate an unbounded grid.
    constexpr uint32 kMaxGridCellsPerAxis = 1024u;
    constexpr float kMinCellSize = 0.5f;
    m_Grid = {};
    if (m_Triangles.empty())
        return;
    float minX = std::numeric_limits<float>::max(), minZ = minX, maxX = -minX, maxZ = -minX;
    for (const auto &t : m_Triangles)
        for (const float *v : {t.A, t.B, t.C})
        {
            minX = std::min(minX, v[0]);
            maxX = std::max(maxX, v[0]);
            minZ = std::min(minZ, v[1]);
            maxZ = std::max(maxZ, v[1]);
        }
    const float sizeX = std::max(maxX - minX, kMinCellSize), sizeZ = std::max(maxZ - minZ, kMinCellSize);
    float cell = std::max(std::sqrt(sizeX * sizeZ / static_cast<float>(m_Triangles.size())), kMinCellSize);
    cell = std::max({cell, sizeX / kMaxGridCellsPerAxis, sizeZ / kMaxGridCellsPerAxis});
    m_Grid.OriginX = minX;
    m_Grid.OriginZ = minZ;
    m_Grid.CellSize = cell;
    m_Grid.Width = std::min(static_cast<uint32>(sizeX / cell) + 1u, kMaxGridCellsPerAxis);
    m_Grid.Height = std::min(static_cast<uint32>(sizeZ / cell) + 1u, kMaxGridCellsPerAxis);
    const uint32 cellCount = m_Grid.Width * m_Grid.Height;
    auto &cells = m_Grid.Cells;
    cells.assign(cellCount * 2u, 0u);
    uint32 range[4];
    for (const auto &t : m_Triangles)
    {
        GridCellRange(t, m_Grid, range);
        for (uint32 z = range[1]; z <= range[3]; ++z)
            for (uint32 x = range[0]; x <= range[2]; ++x)
                ++cells[(z * m_Grid.Width + x) * 2u + 1u];
    }
    uint32 offset = cellCount * 2u;
    for (uint32 i = 0; i < cellCount; ++i)
    {
        cells[i * 2u] = offset;
        offset += cells[i * 2u + 1u];
        cells[i * 2u + 1u] = 0u;
    }
    cells.resize(offset);
    for (uint32 index = 0; index < m_Triangles.size(); ++index)
    {
        GridCellRange(m_Triangles[index], m_Grid, range);
        for (uint32 z = range[1]; z <= range[3]; ++z)
            for (uint32 x = range[0]; x <= range[2]; ++x)
            {
                const uint32 slot = (z * m_Grid.Width + x) * 2u;
                cells[cells[slot] + cells[slot + 1u]++] = index;
            }
    }
}
std::span<const uint32> OceanSplineRaster::TrianglesAt(float x, float z) const
{
    if (m_Grid.Cells.empty())
        return {};
    const float fx = (x - m_Grid.OriginX) / m_Grid.CellSize, fz = (z - m_Grid.OriginZ) / m_Grid.CellSize;
    if (fx < 0 || fz < 0 || fx >= static_cast<float>(m_Grid.Width) || fz >= static_cast<float>(m_Grid.Height))
        return {};
    const uint32 slot = (static_cast<uint32>(fz) * m_Grid.Width + static_cast<uint32>(fx)) * 2u;
    return {m_Grid.Cells.data() + m_Grid.Cells[slot], m_Grid.Cells[slot + 1u]};
}
void OceanSplineRaster::RebaseOrigin(float x, float z, float y)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    ++m_Revision;
    m_Grid.OriginX -= x;
    m_Grid.OriginZ -= z;
    m_SurfaceBounds[0] -= x;
    m_SurfaceBounds[1] -= z;
    m_SurfaceBounds[2] -= x;
    m_SurfaceBounds[3] -= z;
    m_ClipBounds[0] -= x;
    m_ClipBounds[1] -= z;
    m_ClipBounds[2] -= x;
    m_ClipBounds[3] -= z;
    for (auto &t : m_Triangles)
        for (float *v : {t.A, t.B, t.C})
        {
            v[0] -= x;
            v[1] -= z;
            v[3] -= y;
        }
    for (auto &t : m_UnderwaterTree)
    {
        if (t.Meta[0] & 0x80000000u)
        {
            t.A[0] -= x;
            t.A[1] -= y;
            t.A[2] -= z;
            t.B[0] -= x;
            t.B[1] -= y;
            t.B[2] -= z;
        }
        else
            for (float *v : {t.A, t.B, t.C})
            {
                v[0] -= x;
                v[1] -= z;
                v[3] -= y;
            }
    }
}
bool OceanSplineRaster::Contains(float x, float z) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    float b[3];
    for (const uint32 index : TrianglesAt(x, z))
    {
        const auto &t = m_Triangles[index];
        if ((t.Meta[0] & 16u) && SampleOceanRibbonTriangle(t, x, z, b) && OceanRibbonEdgeDistance(t, b) >= 0)
            return true;
    }
    return false;
}
bool OceanSplineRaster::ClipBoundsXZ(float &minX, float &minZ, float &maxX, float &maxZ) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_HasClipBounds)
        return false;
    minX = m_ClipBounds[0];
    minZ = m_ClipBounds[1];
    maxX = m_ClipBounds[2];
    maxZ = m_ClipBounds[3];
    return true;
}

bool OceanSplineRaster::SurfaceBoundsXZ(float &minX, float &minZ, float &maxX, float &maxZ) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_HasSurfaceBounds)
        return false;
    minX = m_SurfaceBounds[0];
    minZ = m_SurfaceBounds[1];
    maxX = m_SurfaceBounds[2];
    maxZ = m_SurfaceBounds[3];
    return true;
}
void OceanSplineRaster::ApplyFlow(float x, float z, OceanCurrentSample &result) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    // Cell lists are in ascending triangle order and each ribbon's triangles are
    // contiguous. Like the GPU raster, each ribbon applies once, through its
    // innermost triangle, so a shared diagonal or a bend overlap does not add twice.
    uint32 ribbon = std::numeric_limits<uint32>::max();
    RibbonFlowHit best;
    float b[3];
    for (const uint32 index : TrianglesAt(x, z))
    {
        const auto &t = m_Triangles[index];
        if (!(t.Meta[0] & 2u))
            continue;
        if (t.Meta[1] != ribbon)
        {
            AddRibbonFlow(best, result);
            best = {};
            ribbon = t.Meta[1];
        }
        if (!SampleOceanRibbonTriangle(t, x, z, b))
            continue;
        const float edge = OceanRibbonEdgeDistance(t, b);
        if (!best.Triangle || edge > best.Edge)
            best = {&t, {b[0], b[1], b[2]}, edge};
    }
    AddRibbonFlow(best, result);
}
bool OceanSplineRaster::SampleDepth(float x, float z, float &depth) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    bool found = false;
    float b[3];
    for (const uint32 index : TrianglesAt(x, z))
    {
        const auto &t = m_Triangles[index];
        if (!(t.Meta[0] & 1u) || !SampleOceanRibbonTriangle(t, x, z, b))
            continue;
        const float d = OceanDepthBandDepth(t.Values[1], t.FlowA[3], OceanRibbonEdgeDistance(t, b), t.FlowA[2]);
        if (d >= kOceanDepthBandOutside)
            continue;
        depth = found ? std::min(depth, d) : d;
        found = true;
    }
    return found;
}
bool OceanSplineRaster::UnderwaterDepth(float x, float y, float z, float &depth) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    bool found = false;
    float b[3];
    depth = 0;
    for (const uint32 index : TrianglesAt(x, z))
    {
        const auto &t = m_Triangles[index];
        if ((t.Meta[0] & 32u) && SampleOceanRibbonTriangle(t, x, z, b) && OceanRibbonEdgeDistance(t, b) >= 0)
        {
            const float surface = t.A[3] * b[0] + t.B[3] * b[1] + t.C[3] * b[2];
            if (y <= surface && y >= surface - t.Values[3])
            {
                depth = std::max(depth, surface - y);
                found = true;
            }
        }
    }
    return found;
}
bool OceanSplineRaster::Initialize(IDevice *device, uint32 field)
{
    if (m_Pipelines[field].IsValid())
        return true;
    // A program that failed to load stays failed: the error is logged once and
    // the field keeps its analytic bake instead of retrying every frame.
    if (m_LoadFailed[field])
        return false;
    m_LoadFailed[field] = true;
    // One program per field: each writes its own storage-image format.
    constexpr const char *kPrograms[] = {"ocean_spline_raster_depth", "ocean_spline_raster_flow",
                                         "ocean_spline_raster_clip", "ocean_spline_raster_albedo"};
    const std::string source = std::string(kPrograms[field]) + ".comp";
    const auto dir = OceanShaderDirectory(source.c_str());
    ShaderProgramCompileRequest req{};
    req.debugName = kPrograms[field];
    req.baseDirectory = dir;
    req.cacheRoot = ".Cache/Shaders";
    req.includeDirs = {dir.parent_path()};
    req.stages = {{"cs", source, "main", {}}};
    ShaderProgramCompileResult result{};
    std::string error;
    const auto stage = LoadOceanShaderProgram(req, device->PreferredShaderSource(), result, &error)
                           ? result.stageBytes.find("cs")
                           : result.stageBytes.end();
    if (stage == result.stageBytes.end() || stage->second.empty())
    {
        Logger::Log::Error("Ocean spline raster: {} failed to load, so splines are not drawn into that "
                           "ocean field until restart: {}",
                           kPrograms[field], error);
        return false;
    }
    m_Layout.debugName = "Ocean.SplineRaster.Set0";
    m_Layout.bindings = {{0, DescriptorType::UniformBuffer, 1, kShaderStageCompute},
                         {1, DescriptorType::StorageBuffer, 1, kShaderStageCompute},
                         {2, DescriptorType::StorageBuffer, 1, kShaderStageCompute},
                         {3, DescriptorType::StorageImage, 1, kShaderStageCompute}};
    Rendering::ApplyMetaImageShapeToLayout(result.meta, m_Layout);
    ComputePipelineDesc desc{};
    desc.DebugName = req.debugName;
    desc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(stage->second));
    desc.DescriptorSetLayouts.push_back(device->InternDescriptorSetLayout(m_Layout));
    m_Pipelines[field] = device->InternComputePipeline(desc);
    m_LoadFailed[field] = !m_Pipelines[field].IsValid();
    return m_Pipelines[field].IsValid();
}
void OceanSplineRaster::BinTriangles(uint32 field, const OceanCascadeLayoutGPU &layout, uint32 resolution,
                                     TileBins &bins) const
{
    const uint32 groups = (resolution + 7u) / 8u, tileCount = groups * groups * layout.LodCount;
    // Counting sort over the same tile ranges: count per tile, prefix-sum the
    // list offsets, then fill the lists in place.
    auto &packed = bins.Packed;
    packed.assign(tileCount * 2u, 0u);
    for (const auto &t : m_Triangles)
        if (t.Meta[0] & (1u << field))
            for (uint32 lod = 0; lod < layout.LodCount; ++lod)
            {
                const RibbonTileRange r = TileRangeOf(t, layout, lod, groups);
                for (int z = r.Z0; z <= r.Z1; ++z)
                    for (int x = r.X0; x <= r.X1; ++x)
                        ++packed[((lod * groups + z) * groups + x) * 2u + 1u];
            }
    uint32 offset = tileCount * 2u;
    for (uint32 i = 0; i < tileCount; ++i)
    {
        packed[i * 2u] = offset;
        offset += packed[i * 2u + 1u];
        packed[i * 2u + 1u] = 0u;
    }
    packed.resize(offset);
    for (uint32 index = 0; index < m_Triangles.size(); ++index)
        if (m_Triangles[index].Meta[0] & (1u << field))
            for (uint32 lod = 0; lod < layout.LodCount; ++lod)
            {
                const RibbonTileRange r = TileRangeOf(m_Triangles[index], layout, lod, groups);
                for (int z = r.Z0; z <= r.Z1; ++z)
                    for (int x = r.X0; x <= r.X1; ++x)
                    {
                        const uint32 tile = ((lod * groups + z) * groups + x) * 2u;
                        packed[packed[tile] + packed[tile + 1u]++] = index;
                    }
            }
}
bool OceanSplineRaster::Declare(RenderGraph::RGFrame &frame, IDevice *device, uint32 field,
                                RenderGraph::RGTexture target, const OceanCascadeLayoutGPU &layout,
                                uint32 resolution)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (field > 3 || !(m_Flags & (1u << field)) || !target.IsValid() || !Initialize(device, field))
        return false;
    const uint32 groups = (resolution + 7u) / 8u;
    auto &bins = m_Bins[field];
    if (bins.Revision != m_Revision || bins.Resolution != resolution ||
        std::memcmp(&bins.Layout, &layout, sizeof(layout)) != 0)
    {
        BinTriangles(field, layout, resolution, bins);
        bins.Layout = layout;
        bins.Resolution = resolution;
        bins.Revision = m_Revision;
    }
    const auto &packed = bins.Packed;
    const auto tris = frame.AllocUpload(m_Triangles.size() * sizeof(OceanRibbonTriangleGPU));
    const auto tiles = frame.AllocUpload(packed.size() * sizeof(uint32));
    struct alignas(16) Params
    {
        OceanCascadeLayoutGPU Layout;
        uint32 Grid[4];
    };
    const auto u = frame.AllocUpload<Params>();
    if (!tris.Valid() || !tiles.Valid() || !u.Valid())
        return false;
    std::memcpy(tris.Ptr, m_Triangles.data(), m_Triangles.size() * sizeof(OceanRibbonTriangleGPU));
    std::memcpy(tiles.Ptr, packed.data(), packed.size() * sizeof(uint32));
    *u.Ptr = {layout, {resolution, groups, 0, 0}};
    const auto pipe = m_Pipelines[field];
    const auto dsLayout = m_Layout;
    frame.AddPass(("OceanSplineRaster" + std::to_string(field)).c_str(), PassPhase::kEarlySetup,
                  [&](RenderGraph::RGPassBuilder &p) {
                      p.Read(target, RenderGraph::RGTextureRead::Storage);
                      p.Write(target, RenderGraph::RGTextureWrite::Storage);
                  },
                  [pipe, dsLayout, target, tris, tiles, params = u.Buffer, offset = u.Offset, groups,
                   lods = layout.LodCount, triBytes = m_Triangles.size() * sizeof(OceanRibbonTriangleGPU),
                   tileBytes = packed.size() * sizeof(uint32)](RenderGraph::RGContext &ctx) {
                      auto *dev = ctx.GetDevice();
                      if (!dev || !ctx.Cmd)
                          return;
                      const auto pso = dev->GetOrCreateComputePipeline(pipe);
                      if (!pso)
                          return;
                      DescriptorSetDesc desc{};
                      desc.layout = dsLayout;
                      desc.transient = true;
                      desc.debugName = "Ocean.SplineRaster.DS";
                      const auto ds = dev->CreateDescriptorSet(desc);
                      dev->UpdateBufferBinding(ds, 0, params, offset, sizeof(Params));
                      dev->UpdateStorageBufferBinding(ds, 1, tris.Buffer, tris.Offset, triBytes);
                      dev->UpdateStorageBufferBinding(ds, 2, tiles.Buffer, tiles.Offset, tileBytes);
                      dev->UpdateStorageImageBinding(ds, 3, ctx.GetTexture(target));
                      ctx.Cmd->SetPipeline(pso);
                      ctx.Cmd->BindDescriptorSet(0, ds, pso);
                      ctx.Cmd->Dispatch(groups, groups, lods);
                  });
    frame.MarkOutput(target, RenderGraph::RGImageLayout::ShaderReadOnly);
    return true;
}
} // namespace GameEngine::Ocean
