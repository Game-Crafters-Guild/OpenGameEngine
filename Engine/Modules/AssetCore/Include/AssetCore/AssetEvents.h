#pragma once

#include "AssetCore/Types.h"
#include "AssetCore/GUID.h"
#include "AssetCore/AssetTypes.h"
#include <functional>
#include <mutex>
#include <unordered_map>

namespace GameEngine {

// Forward declarations
class Asset;

/**
 * @brief Asset event types
 */
enum class AssetEventType {
    AssetLoaded,        ///< Asset has been loaded
    AssetUnloaded,      ///< Asset has been unloaded
    AssetReloaded,      ///< Asset has been reloaded
    AssetLoadFailed,    ///< Asset failed to load
    AssetCreated,       ///< New asset has been created
    AssetDestroyed,     ///< Asset has been destroyed
    AssetModified,      ///< Asset file has been modified on disk
    GuidRemapped        ///< Asset identity remapped in place (overlapping-source re-claim); AssetGuid = new, Message = old GUID string
};

/**
 * @brief Asset event data structure
 */
struct AssetEvent {
    AssetEventType EventType;                ///< Type of event
    GUID AssetGuid;                     ///< GUID of the affected asset
    AssetType Type;                ///< Type of the affected asset
    String AssetPath;                   ///< Path to the asset file
    String Message;                     ///< Optional message or error description
    /// Other GUIDs that name the same asset: every redirect source that resolves to AssetGuid.
    /// Filled by the asset manager on the events a cache drops entries on (AssetReloaded,
    /// AssetUnloaded, and the file-change events AssetModified, AssetCreated and AssetDestroyed);
    /// empty on the rest and in a project without redirects. A consumer may hold an asset under
    /// any of them (a scene authored before the file's GUID was re-minted), so a cache
    /// invalidating on this event must drop all of them; AssetReloadInvalidator does.
    Vector<GUID> RedirectSources;

    /**
     * @brief Constructor
     */
    AssetEvent(AssetEventType eventType, const GUID& guid, GameEngine::AssetType type, const String& path, const String& msg = "")
        : EventType(eventType), AssetGuid(guid), Type(type), AssetPath(path), Message(msg) {}
};

/**
 * @brief Asset event callback function type
 */
using AssetEventCallback = std::function<void(const AssetEvent&)>;

/**
 * @brief Asset event listener interface
 */
class IAssetEventListener {
public:
    virtual ~IAssetEventListener() = default;
    
    /**
     * @brief Called when an asset event occurs
     * @param event The asset event data
     */
    virtual void OnAssetEvent(const AssetEvent& event) = 0;
};

/**
 * @brief Asset event dispatcher
 * 
 * Manages asset event listeners and dispatches events to them.
 */
class AssetEventDispatcher {
public:
    AssetEventDispatcher() = default;
    ~AssetEventDispatcher() = default;

    /**
     * @brief Add an event listener
     * @param listener Pointer to the listener (must remain valid until removed)
     */
    void AddListener(IAssetEventListener* listener);

    /**
     * @brief Remove an event listener
     * @param listener Pointer to the listener to remove
     */
    void RemoveListener(IAssetEventListener* listener);

    /**
     * @brief Add a callback function as a listener
     * @param callback The callback function
     * @return Handle that can be used to remove the callback
     */
    uint32 AddCallback(const AssetEventCallback& callback);

    /**
     * @brief Remove a callback by handle
     * @param handle The handle returned by AddCallback
     */
    void RemoveCallback(uint32 handle);

    /**
     * @brief Dispatch an event to all listeners
     * @param event The event to dispatch
     */
    void DispatchEvent(const AssetEvent& event);

    /**
     * @brief Clear all listeners and callbacks
     */
    void Clear();

    /**
     * @brief Get the number of registered listeners
     * @return Number of listeners (including callbacks)
     */
    size_t GetListenerCount() const;

private:
    mutable std::mutex m_Mutex;
    Vector<IAssetEventListener*> m_Listeners;
    std::unordered_map<uint32, AssetEventCallback> m_Callbacks;
    uint32 m_NextCallbackHandle = 1;

    DISALLOW_COPY_AND_ASSIGN(AssetEventDispatcher);
};

/**
 * @brief Convert asset event type to string
 * @param type The event type
 * @return String representation of the event type
 */
String AssetEventTypeToString(AssetEventType type);

/**
 * @brief Helper function to create asset events
 */
namespace AssetEvents {
    AssetEvent AssetLoaded(const GUID& guid, AssetType type, const String& path);
    AssetEvent AssetUnloaded(const GUID& guid, AssetType type, const String& path);
    AssetEvent AssetReloaded(const GUID& guid, AssetType type, const String& path);
    AssetEvent AssetLoadFailed(const GUID& guid, AssetType type, const String& path, const String& error);
    AssetEvent AssetCreated(const GUID& guid, AssetType type, const String& path);
    AssetEvent AssetDestroyed(const GUID& guid, AssetType type, const String& path);
    AssetEvent AssetModified(const GUID& guid, AssetType type, const String& path);
}

} // namespace GameEngine
