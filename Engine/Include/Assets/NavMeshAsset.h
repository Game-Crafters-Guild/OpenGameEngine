#pragma once

#include "AssetCore/Asset.h"
#include "Pathfinding/PathfindingTypes.h"
#include <vector>

namespace GameEngine {

class NavMeshAsset : public Asset {
public:
    NavMeshAsset(const GUID& guid, const std::filesystem::path& path);
    ~NavMeshAsset() override;

    bool Load() override;
    bool LoadFromData(const Vector<uint8>& data) override;
    void Unload() override;

    const Pathfinding::NavMeshSettings& GetSettings() const;
    const std::vector<GUID>& GetSourceGeometryGUIDs() const;

    static bool SaveToFile(const std::filesystem::path& path,
                           const Pathfinding::NavMeshSettings& settings,
                           const std::vector<GUID>& geometryGuids);

private:
    Pathfinding::NavMeshSettings m_Settings;
    std::vector<GUID> m_SourceGeometryGUIDs;

    DISALLOW_COPY_AND_ASSIGN(NavMeshAsset);
};

} // namespace GameEngine
