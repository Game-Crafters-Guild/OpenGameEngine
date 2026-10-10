#pragma once

#include "AssetCore/Asset.h"
#include "Assets/ParserRegistry.h"
#include "Types/Types.h"

namespace GameEngine {

/**
 * @brief Generic binary asset implementation
 * 
 * Handles loading and management of generic binary files that don't fit into
 * other specific asset categories. Provides raw binary data access for custom
 * processing by game systems.
 */
class BinaryAsset : public Asset {
public:
    BinaryAsset(const GUID& guid, const std::filesystem::path& path, AssetType type);
    virtual ~BinaryAsset();

    /**
     * @brief Load binary data from file
     */
    bool Load() override;

    /**
     * @brief Load binary data from memory (for async loading)
     */
    bool LoadFromData(const Vector<uint8>& data) override;

    /**
     * @brief Unload binary data from memory
     */
    void Unload() override;

    /**
     * @brief Get raw binary data
     */
    const uint8* GetData() const { return m_Data; }

    /**
     * @brief Get data size in bytes
     */
    uint64 GetDataSize() const { return m_DataSize; }

    /**
     * @brief Check if data is loaded
     */
    bool HasData() const { return m_Data != nullptr && m_DataSize > 0; }

    /**
     * @brief Get a copy of the data as a vector
     */
    Vector<uint8> GetDataCopy() const;

private:
    uint8* m_Data;
    uint64 m_DataSize;

    DISALLOW_COPY_AND_ASSIGN(BinaryAsset);
};

/**
 * @brief Parser for generic binary assets
 */
class BinaryAssetParser : public AssetParser {
public:
    AssetType GetAssetType() const override { return AssetType::Unknown; }
    
    std::vector<std::string> GetSupportedExtensions() const override {
        return {".bin", ".dat", ".data", ".raw", ".txt", ".spv",
                ".oceandepth", ".oceanspectrum", ".oceanfft",
                ".oceansettings", ".oceanpreset"};
    }
    
    AssetParseResult Parse(const AssetMetadata& metadata, AssetManager& assetManager) override;
    
    std::string GetName() const override { return "BinaryAssetParser"; }
    
    int GetPriority() const override { return 10; } // Lower priority than specific parsers
};

} // namespace GameEngine
