#pragma once

#include "AssetCore/Asset.h"
#include "Pathfinding/PathfindingTypes.h"
#include <vector>

namespace GameEngine {

// Binary format: [magic "NGRD"][version uint32][GridSettings fields][costs float32[W*D]][blocked uint8[W*D]]
class NavGridAsset : public Asset {
public:
    NavGridAsset(const GUID& guid, const std::filesystem::path& path);
    ~NavGridAsset() override;

    bool Load() override;
    bool LoadFromData(const Vector<uint8>& data) override;
    void Unload() override;
    size_t GetMemoryUsage() const override;

    const Pathfinding::GridSettings& GetGridSettings() const;
    const std::vector<float32>& GetCosts() const;
    const std::vector<uint8>& GetBlocked() const;

    // Write asset to file (used by brush tool save)
    static bool SaveToFile(const std::filesystem::path& path,
                           const Pathfinding::GridSettings& settings,
                           const float32* costs, const uint8* blocked,
                           uint32 cellCount);

private:
    bool ParseBinary(const uint8* data, size_t size);

    Pathfinding::GridSettings m_Settings;
    std::vector<float32> m_Costs;
    std::vector<uint8> m_Blocked;

    DISALLOW_COPY_AND_ASSIGN(NavGridAsset);
};

} // namespace GameEngine
