#pragma once

#include "Pathfinding/INavigationMap.h"
#include "Pathfinding/PathfindingTypes.h"

#include <memory>
#include <vector>

namespace GameEngine::Pathfinding
{

class DetourNavMap : public INavigationMap
{
public:
    DetourNavMap();
    ~DetourNavMap();

    DetourNavMap(const DetourNavMap&) = delete;
    DetourNavMap& operator=(const DetourNavMap&) = delete;
    DetourNavMap(DetourNavMap&&) noexcept;
    DetourNavMap& operator=(DetourNavMap&&) noexcept;

    // INavigationMap
    PathStatus FindPath(const PathRequest& request, PathBuffer& pathBuffer, PathHandle& outPath) const override;
    bool IsPointNavigable(float32 x, float32 y, float32 z, float32 radius) const override;
    bool GetClosestNavigablePoint(float32 x, float32 y, float32 z, float32 searchRadius,
                                   float32& outX, float32& outY, float32& outZ) const override;
    bool Raycast(float32 startX, float32 startY, float32 startZ,
                 float32 endX, float32 endY, float32 endZ,
                 float32& hitX, float32& hitY, float32& hitZ) const override;

    // Baking via Recast
    struct InputGeometry
    {
        const float32* Vertices = nullptr; // [VertexCount * 3] XYZ interleaved
        uint32 VertexCount = 0;
        const uint32* Indices = nullptr;   // [TriangleCount * 3]
        uint32 TriangleCount = 0;
    };
    bool Build(const NavMeshSettings& settings, const InputGeometry& geometry);
    bool IsBuilt() const;

    // Serialization
    bool Serialize(std::vector<uint8>& outData) const;
    bool Deserialize(const uint8* data, uint32 size);

    // Debug mesh access
    uint32 GetDebugVertexCount() const;
    uint32 GetDebugTriangleCount() const;
    const float32* GetDebugVertices() const;
    const uint32* GetDebugIndices() const;

    // Opaque access to Detour internals (for CrowdManager)
    void* GetDetourNavMesh() const;
    void* GetDetourNavMeshQuery() const;

private:
    bool DeserializeLegacy(const uint8* data, uint32 size);

    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace GameEngine::Pathfinding
