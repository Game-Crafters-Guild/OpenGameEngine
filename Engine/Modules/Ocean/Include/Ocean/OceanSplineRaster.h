#pragma once
#include "Ocean/OceanTypes.h"
#include "Ocean/OceanQuery.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include <array>
#include <mutex>
#include <span>
#include <vector>
namespace GameEngine::Ocean
{
struct OceanRibbonPoint
{
    float X = 0, Y = 0, Z = 0, TX = 0, TZ = 1;
};
struct OceanRibbonStyle
{
    float Width = 1, FlowSpeed = 0, Depth = 0, Feather = 0, UnderwaterDepth = 0, DepthFeather = 0;
    // Depth at which every shallow term saturates; the depth band ramps from it.
    float DepthSaturation = 0;
    float Color[4]{};
    uint32 Flags = 0; // depth=1, flow=2, clip=4, albedo=8, confine=16, underwater=32
    uint32 Id = 0;
};
// A ribbon triangle. The cross-ribbon coordinate is -1 and +1 on the true edges;
// triangles extend past them by the apron, where it exceeds 1 in magnitude, and
// (1 - |cross|) * width is the signed distance inside the true edge.
struct alignas(16) OceanRibbonTriangleGPU
{
    float A[4]{}, B[4]{}, C[4]{};             // x,z,signed cross-ribbon coordinate,y
    float FlowA[4]{}, FlowB[4]{}, FlowC[4]{}; // FlowA.z depth feather, FlowA.w depth saturation, FlowB.z apron
    float Color[4]{};
    float Values[4]{}; // width, depth, feather, underwater depth
    uint32 Meta[4]{};  // flags, ribbon id, pad
};
static_assert(sizeof(OceanRibbonTriangleGPU) == 144);
// Tessellated ribbons are independent of the analytic source arrays and their caps.
// The triangles extend past each edge by an apron of max(Width, Feather / 2), room
// for the clip and paint ramps centered on the edge and for the depth band's bank
// slope outside it, at every cascade where the ribbon spans at least two texels.
std::vector<OceanRibbonTriangleGPU> BuildOceanRibbon(const std::vector<OceanRibbonPoint> &points,
                                                     const OceanRibbonStyle &style);
// Barycentrics of (x, z) in the triangle, apron included; false outside it.
bool SampleOceanRibbonTriangle(const OceanRibbonTriangleGPU &triangle, float x, float z, float bary[3]);
// Signed distance (meters) inside the ribbon's true edge at barycentrics `bary`;
// negative in the apron.
float OceanRibbonEdgeDistance(const OceanRibbonTriangleGPU &triangle, const float bary[3]);
class OceanSplineRaster
{
  public:
    // Returns true when the triangle set differs from the stored one.
    bool SetTriangles(std::vector<OceanRibbonTriangleGPU> triangles);
    void RebaseOrigin(float x, float z, float y = 0);
    bool HasField(uint32 field) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return (m_Flags & (1u << field)) != 0u;
    }
    bool Contains(float x, float z) const;
    // XZ bounds of the ribbons that carry water surface (the ones Contains reads);
    // false when there are none.
    bool SurfaceBoundsXZ(float &minX, float &minZ, float &maxX, float &maxZ) const;
    // XZ bounds of the ribbons that write the clip field (field 2), aprons
    // included; false when there are none.
    bool ClipBoundsXZ(float &minX, float &minZ, float &maxX, float &maxZ) const;
    void ApplyFlow(float x, float z, OceanCurrentSample &result) const;
    bool SampleDepth(float x, float z, float &depth) const;
    bool UnderwaterDepth(float x, float y, float z, float &depth) const;
    bool HasUnderwater() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return (m_Flags & 32u) != 0;
    }
    bool Declare(Rendering::RenderGraph::RGFrame &frame, Rendering::IDevice *device, uint32 field,
                 Rendering::RenderGraph::RGTexture target, const OceanCascadeLayoutGPU &layout,
                 uint32 resolution);
    size_t GetTriangleCount() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Triangles.size();
    }
    // Copies the underwater ribbon hierarchy into `out`, reusing its storage.
    void CopyUnderwaterTree(std::vector<OceanRibbonTriangleGPU> &out) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        out.assign(m_UnderwaterTree.begin(), m_UnderwaterTree.end());
    }

  private:
    // Triangle indices binned per 8x8-texel dispatch tile of one field: per
    // tile a (begin, count) pair, then the index lists. Rebuilt only when the
    // triangles, the cascade layout or the resolution change.
    struct TileBins
    {
        OceanCascadeLayoutGPU Layout{};
        uint32 Resolution = 0;
        uint64 Revision = ~0ull;
        std::vector<uint32> Packed;
    };
    // Triangle indices binned on a uniform XZ grid for the CPU queries: per cell
    // a (begin, count) pair, then the index lists in ascending triangle order.
    // Rebuilt with the triangles; a rebase moves the origin with them.
    struct QueryGrid
    {
        float OriginX = 0, OriginZ = 0, CellSize = 1;
        uint32 Width = 0, Height = 0;
        std::vector<uint32> Cells;
    };
    void BuildQueryGrid();
    std::span<const uint32> TrianglesAt(float x, float z) const;
    bool Initialize(Rendering::IDevice *device, uint32 field);
    void BinTriangles(uint32 field, const OceanCascadeLayoutGPU &layout, uint32 resolution, TileBins &bins) const;
    mutable std::mutex m_Mutex;
    uint32 m_Flags = 0;
    uint64 m_Revision = 0;
    std::array<TileBins, 4> m_Bins{};
    std::vector<OceanRibbonTriangleGPU> m_Triangles;
    QueryGrid m_Grid;
    // Preorder BVH: branch records use A/B for min/max XYZ and Meta[1] for escape.
    std::vector<OceanRibbonTriangleGPU> m_UnderwaterTree;
    // XZ bounds (min x, min z, max x, max z) of the surface-carrying ribbons
    // (flag 16), kept by SetTriangles and RebaseOrigin.
    float m_SurfaceBounds[4] = {};
    bool m_HasSurfaceBounds = false;
    // Same for the clip-field ribbons (flag 4), kept by SetTriangles and RebaseOrigin.
    float m_ClipBounds[4] = {};
    bool m_HasClipBounds = false;
    std::array<Rendering::ComputePipelineId, 4> m_Pipelines{};
    std::array<bool, 4> m_LoadFailed{};
    Rendering::DescriptorSetLayoutDesc m_Layout{};
};
} // namespace GameEngine::Ocean
