#pragma once

#include "AssetCore/Asset.h"
#include "AssetCore/AssetEvents.h"
#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include "AssetCore/Types.h"
#include <filesystem>
#include <functional>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

namespace GameEngine
{

/**
 * @brief Asset factory function type
 * Creates an asset instance from metadata
 */
using AssetFactory = std::function<SharedPtr<Asset>(const AssetMetadata&)>;

/**
 * @brief Asset type registration information
 */
struct AssetTypeRegistration
{
    AssetType type;            ///< Asset type
    Vector<String> extensions; ///< Supported file extensions
    AssetFactory factory;      ///< Factory function to create assets
    String description;        ///< Human-readable description
    int32 priority;            ///< Registration priority (higher = preferred)

    AssetTypeRegistration() : type(AssetType::Unknown), priority(0) {}

    AssetTypeRegistration(AssetType assetType, const Vector<String>& exts,
                          AssetFactory factoryFunc, const String& desc = "", int32 prio = 0)
        : type(assetType), extensions(exts), factory(factoryFunc), description(desc), priority(prio) {}
};

/**
 * @brief Core asset registry for type management
 *
 * This registry manages asset type registrations and provides factories
 * for creating asset instances. It's designed to be used by higher-level
 * asset management systems.
 */
class AssetTypeRegistry
{
  public:
    AssetTypeRegistry();
    ~AssetTypeRegistry() = default;

    /**
     * @brief Initialize the registry
     * @return true if initialization was successful
     */
    bool Initialize();

    /**
     * @brief Shutdown the registry
     */
    void Shutdown();

    /**
     * @brief Register an asset type
     * @param registration The asset type registration information
     * @return true if registration was successful
     */
    bool RegisterAssetType(const AssetTypeRegistration& registration);

    /**
     * @brief Unregister an asset type
     * @param type The asset type to unregister
     */
    void UnregisterAssetType(AssetType type);

    /**
     * @brief Check if an asset type is registered
     * @param type The asset type to check
     * @return true if the asset type is registered
     */
    bool IsAssetTypeRegistered(AssetType type) const;

    /**
     * @brief Get asset type from file extension
     * @param extension The file extension (including dot)
     * @return The asset type, or AssetType::Unknown if not found
     */
    AssetType GetAssetTypeFromExtension(const String& extension) const;

    /**
     * @brief Create an asset instance
     * @param metadata The asset metadata
     * @return Shared pointer to the created asset, or nullptr if no factory is registered for the type
     * @throws Whatever the registered factory throws; the caller reports it with the asset's context
     */
    SharedPtr<Asset> CreateAsset(const AssetMetadata& metadata) const;

    /**
     * @brief Get all registered asset types
     * @return Vector of all registered asset types
     */
    Vector<AssetType> GetRegisteredAssetTypes() const;

    /**
     * @brief Get registration information for an asset type
     * @param type The asset type
     * @return true if found and outRegistration was set
     */
    bool TryGetAssetTypeRegistration(AssetType type, AssetTypeRegistration& outRegistration) const;

    /**
     * @brief Get all supported extensions
     * @return Set of all supported file extensions
     */
    std::unordered_set<String> GetAllSupportedExtensions() const;

    /**
     * @brief Check if a file extension is supported
     * @param extension The file extension to check
     * @return true if the extension is supported
     */
    bool IsExtensionSupported(const String& extension) const;

    /**
     * @brief Get the number of registered asset types
     * @return Number of registered asset types
     */
    size_t GetRegisteredTypeCount() const;

    /**
     * @brief Clear all registrations
     */
    void Clear();

    /**
     * @brief Get event dispatcher for asset type events
     * @return Reference to the event dispatcher
     */
    AssetEventDispatcher& GetEventDispatcher() { return m_EventDispatcher; }
    const AssetEventDispatcher& GetEventDispatcher() const { return m_EventDispatcher; }

  private:
    bool m_Initialized = false;

    // Thread-safety: allow concurrent reads while protecting writes.
    mutable std::shared_mutex m_RegistryMutex;

    // Asset type registrations
    std::unordered_map<AssetType, AssetTypeRegistration> m_TypeRegistrations;

    // Extension to asset type mapping (for fast lookup)
    std::unordered_map<String, AssetType> m_ExtensionToType;

    // Event dispatcher for asset type events
    AssetEventDispatcher m_EventDispatcher;

    /**
     * @brief Register built-in asset types
     */
    void RegisterBuiltInAssetTypes();

    /**
     * @brief Update extension mappings after registration changes
     */
    void UpdateExtensionMappings();

    /**
     * @brief Normalize file extension (lowercase, ensure starts with '.')
     */
    String NormalizeExtension(const String& extension) const;

    DISALLOW_COPY_AND_ASSIGN(AssetTypeRegistry);
};

/**
 * @brief Asset reference structure
 *
 * Lightweight reference to an asset that can be used for dependency tracking
 * and lazy loading.
 */
struct AssetReference
{
    GUID guid;      ///< Asset GUID
    AssetType type; ///< Asset type
    String path;    ///< Asset path (for debugging)

    AssetReference() : type(AssetType::Unknown) {}
    AssetReference(const GUID& assetGuid, AssetType assetType, const String& assetPath = "")
        : guid(assetGuid), type(assetType), path(assetPath) {}

    bool IsValid() const { return !guid.IsNull() && type != AssetType::Unknown; }

    bool operator==(const AssetReference& other) const
    {
        return guid == other.guid && type == other.type;
    }

    bool operator!=(const AssetReference& other) const
    {
        return !(*this == other);
    }
};

} // namespace GameEngine

// Hash specialization for AssetReference
namespace std
{
template <>
struct hash<GameEngine::AssetReference>
{
    size_t operator()(const GameEngine::AssetReference& ref) const
    {
        return hash<GameEngine::GUID>{}(ref.guid) ^
               (hash<int>{}(static_cast<int>(ref.type)) << 1);
    }
};
} // namespace std
