#include "Assets/BinaryAsset.h"
#include "AssetCore/SharedFileRead.h"
#include "Assets/AssetManager.h"
#include "Logger/Logger.h"
#include <cstring>

namespace GameEngine {

BinaryAsset::BinaryAsset(const GUID& guid, const std::filesystem::path& path, AssetType type)
    : Asset(guid, type, path)
    , m_Data(nullptr)
    , m_DataSize(0)
{
}

BinaryAsset::~BinaryAsset() {
    Unload();
}

bool BinaryAsset::Load() {
    if (GetState() == AssetState::Loaded) {
        return true;
    }

    SetState(AssetState::Loading);
    Logger::Log::Info("Loading binary asset: {}", GetPath().string());

    if (!Exists()) {
        Logger::Log::Error("Binary file does not exist: {}", GetPath().string());
        SetState(AssetState::Failed);
        return false;
    }

    // Read file data
    Vector<uint8> fileData;
    if (!ReadFileBytesShared(GetPath(), fileData)) {
        Logger::Log::Error("Failed to read binary file: {}", GetPath().string());
        SetState(AssetState::Failed);
        return false;
    }

    return LoadFromData(fileData);
}

bool BinaryAsset::LoadFromData(const Vector<uint8>& data) {
    if (GetState() == AssetState::Loaded) {
        return true;
    }

    SetState(AssetState::Loading);
    Logger::Log::Info("Loading binary asset from memory data: {}", GetName());

    if (data.empty()) {
        Logger::Log::Warning("Empty binary data provided for: {}", GetName());
        // Allow empty binary files
        m_DataSize = 0;
        m_Data = nullptr;
        SetState(AssetState::Loaded);
        return true;
    }

    // Allocate and copy data
    m_DataSize = data.size();
    m_Data = new uint8[m_DataSize];
    std::memcpy(m_Data, data.data(), m_DataSize);

    SetState(AssetState::Loaded);
    Logger::Log::Info("Binary asset loaded successfully: {} ({} bytes)", GetName(), m_DataSize);
    return true;
}

void BinaryAsset::Unload() {
    if (m_Data) {
        delete[] m_Data;
        m_Data = nullptr;
    }
    
    m_DataSize = 0;
    SetState(AssetState::Unloaded);
    
    Logger::Log::Debug("Binary asset unloaded: {}", GetName());
}

Vector<uint8> BinaryAsset::GetDataCopy() const {
    if (!HasData()) {
        return Vector<uint8>();
    }
    
    Vector<uint8> copy(m_DataSize);
    std::memcpy(copy.data(), m_Data, m_DataSize);
    return copy;
}

// BinaryAssetParser implementation
AssetParseResult BinaryAssetParser::Parse(const AssetMetadata& metadata, AssetManager& assetManager) {
    (void)assetManager; // Suppress unused parameter warning
    
    try {
        auto asset = std::make_shared<BinaryAsset>(metadata.Guid, metadata.Path, metadata.Type);
        return AssetParseResult(asset);
    }
    catch (const std::exception& e) {
        return AssetParseResult(false, "Failed to create binary asset: " + std::string(e.what()));
    }
}

} // namespace GameEngine
