#include "AssetCore/AssetRegistry.h"
#include <algorithm>
#include <cctype>

namespace GameEngine
{

AssetTypeRegistry::AssetTypeRegistry() = default;

bool AssetTypeRegistry::Initialize()
{
    std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
    if (m_Initialized)
    {
        return true;
    }

    // Register built-in asset types
    RegisterBuiltInAssetTypes();

    m_Initialized = true;
    return true;
}

void AssetTypeRegistry::Shutdown()
{
    std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
    if (!m_Initialized)
    {
        return;
    }

    // Clear internal state (do not call public Clear() to avoid double-locking)
    m_TypeRegistrations.clear();
    m_ExtensionToType.clear();
    m_EventDispatcher.Clear();

    m_Initialized = false;
}

bool AssetTypeRegistry::RegisterAssetType(const AssetTypeRegistration& registration)
{
    if (registration.type == AssetType::Unknown || !registration.factory)
    {
        return false;
    }

    std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);

    // Check if already registered
    if (m_TypeRegistrations.find(registration.type) != m_TypeRegistrations.end())
    {
        return false; // Already registered
    }

    // Store the registration
    m_TypeRegistrations[registration.type] = registration;

    // Update extension mappings
    UpdateExtensionMappings();

    return true;
}

void AssetTypeRegistry::UnregisterAssetType(AssetType type)
{
    std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
    auto it = m_TypeRegistrations.find(type);
    if (it != m_TypeRegistrations.end())
    {
        m_TypeRegistrations.erase(it);
        UpdateExtensionMappings();
    }
}

bool AssetTypeRegistry::IsAssetTypeRegistered(AssetType type) const
{
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    return m_TypeRegistrations.find(type) != m_TypeRegistrations.end();
}

AssetType AssetTypeRegistry::GetAssetTypeFromExtension(const String& extension) const
{
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    String normalizedExt = NormalizeExtension(extension);
    auto it = m_ExtensionToType.find(normalizedExt);
    if (it != m_ExtensionToType.end())
    {
        return it->second;
    }

    // Fallback to the built-in extension mapping so core types (e.g. .gltf/.glb/.mat/.css/.xml)
    // remain recognizable even when no factory has been registered for that type.
    return ::GameEngine::GetAssetTypeFromExtension(normalizedExt);
}

SharedPtr<Asset> AssetTypeRegistry::CreateAsset(const AssetMetadata& metadata) const
{
    AssetFactory factory;
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        auto it = m_TypeRegistrations.find(metadata.Type);
        if (it == m_TypeRegistrations.end() || !it->second.factory)
        {
            return nullptr;
        }
        factory = it->second.factory; // copy out under lock
    }

    return factory(metadata);
}

Vector<AssetType> AssetTypeRegistry::GetRegisteredAssetTypes() const
{
    Vector<AssetType> types;
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        types.reserve(m_TypeRegistrations.size());
        for (const auto& [type, registration] : m_TypeRegistrations)
        {
            (void)registration;
            types.push_back(type);
        }
    }
    return types;
}

bool AssetTypeRegistry::TryGetAssetTypeRegistration(AssetType type, AssetTypeRegistration& outRegistration) const
{
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    auto it = m_TypeRegistrations.find(type);
    if (it == m_TypeRegistrations.end())
    {
        return false;
    }
    outRegistration = it->second;
    return true;
}

std::unordered_set<String> AssetTypeRegistry::GetAllSupportedExtensions() const
{
    std::unordered_set<String> extensions;
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    for (const auto& [type, registration] : m_TypeRegistrations)
    {
        (void)type;
        for (const auto& ext : registration.extensions)
        {
            extensions.insert(NormalizeExtension(ext));
        }
    }
    return extensions;
}

bool AssetTypeRegistry::IsExtensionSupported(const String& extension) const
{
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    String normalizedExt = NormalizeExtension(extension);
    return m_ExtensionToType.find(normalizedExt) != m_ExtensionToType.end();
}

size_t AssetTypeRegistry::GetRegisteredTypeCount() const
{
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    return m_TypeRegistrations.size();
}

void AssetTypeRegistry::Clear()
{
    std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
    m_TypeRegistrations.clear();
    m_ExtensionToType.clear();
    m_EventDispatcher.Clear();
}

void AssetTypeRegistry::RegisterBuiltInAssetTypes()
{
    // Note: Built-in asset types are registered by their respective subsystems
    // This method is kept for future built-in types that don't belong to specific subsystems
}

void AssetTypeRegistry::UpdateExtensionMappings()
{
    // NOTE: expects m_RegistryMutex to be held with a unique lock by the caller.
    m_ExtensionToType.clear();

    // Build extension to type mapping
    for (const auto& [type, registration] : m_TypeRegistrations)
    {
        for (const auto& ext : registration.extensions)
        {
            String normalizedExt = NormalizeExtension(ext);

            // Handle conflicts by priority (higher priority wins)
            auto existing = m_ExtensionToType.find(normalizedExt);
            if (existing == m_ExtensionToType.end())
            {
                m_ExtensionToType[normalizedExt] = type;
            }
            else
            {
                // Check priority
                auto existingReg = m_TypeRegistrations.find(existing->second);
                if (existingReg != m_TypeRegistrations.end() &&
                    registration.priority > existingReg->second.priority)
                {
                    m_ExtensionToType[normalizedExt] = type;
                }
            }
        }
    }
}

String AssetTypeRegistry::NormalizeExtension(const String& extension) const
{
    String normalized = extension;

    // Convert to lowercase
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c)
                   { return static_cast<char>(std::tolower(c)); });

    // Ensure it starts with '.'
    if (!normalized.empty() && normalized[0] != '.')
    {
        normalized = "." + normalized;
    }

    return normalized;
}

} // namespace GameEngine
