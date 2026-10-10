#include "AssetCore/AssetEvents.h"
#include <algorithm>

namespace GameEngine {

void AssetEventDispatcher::AddListener(IAssetEventListener* listener) {
    if (!listener)
        return;
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (std::find(m_Listeners.begin(), m_Listeners.end(), listener) == m_Listeners.end()) {
        m_Listeners.push_back(listener);
    }
}

void AssetEventDispatcher::RemoveListener(IAssetEventListener* listener) {
    std::lock_guard<std::mutex> lk(m_Mutex);
    auto it = std::find(m_Listeners.begin(), m_Listeners.end(), listener);
    if (it != m_Listeners.end()) {
        m_Listeners.erase(it);
    }
}

uint32 AssetEventDispatcher::AddCallback(const AssetEventCallback& callback) {
    std::lock_guard<std::mutex> lk(m_Mutex);
    uint32 handle = m_NextCallbackHandle++;
    m_Callbacks[handle] = callback;
    return handle;
}

void AssetEventDispatcher::RemoveCallback(uint32 handle) {
    std::lock_guard<std::mutex> lk(m_Mutex);
    m_Callbacks.erase(handle);
}

void AssetEventDispatcher::DispatchEvent(const AssetEvent& event) {
    Vector<IAssetEventListener*> listenersCopy;
    Vector<AssetEventCallback> callbacksCopy;
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        listenersCopy = m_Listeners;
        callbacksCopy.reserve(m_Callbacks.size());
        for (const auto& kv : m_Callbacks)
        {
            callbacksCopy.push_back(kv.second);
        }
    }

    // Notify interface listeners
    for (auto* listener : listenersCopy) {
        if (listener) {
            listener->OnAssetEvent(event);
        }
    }

    // Notify callback listeners
    for (const auto& callback : callbacksCopy) {
        if (callback) {
            callback(event);
        }
    }
}

void AssetEventDispatcher::Clear() {
    std::lock_guard<std::mutex> lk(m_Mutex);
    m_Listeners.clear();
    m_Callbacks.clear();
}

size_t AssetEventDispatcher::GetListenerCount() const {
    std::lock_guard<std::mutex> lk(m_Mutex);
    return m_Listeners.size() + m_Callbacks.size();
}

String AssetEventTypeToString(AssetEventType type) {
    switch (type) {
        case AssetEventType::AssetLoaded:       return "AssetLoaded";
        case AssetEventType::AssetUnloaded:     return "AssetUnloaded";
        case AssetEventType::AssetReloaded:     return "AssetReloaded";
        case AssetEventType::AssetLoadFailed:   return "AssetLoadFailed";
        case AssetEventType::AssetCreated:      return "AssetCreated";
        case AssetEventType::AssetDestroyed:    return "AssetDestroyed";
        case AssetEventType::AssetModified:     return "AssetModified";
        case AssetEventType::GuidRemapped:      return "GuidRemapped";
        default:                                return "Unknown";
    }
}

namespace AssetEvents {

AssetEvent AssetLoaded(const GUID& guid, AssetType type, const String& path) {
    return AssetEvent(AssetEventType::AssetLoaded, guid, type, path);
}

AssetEvent AssetUnloaded(const GUID& guid, AssetType type, const String& path) {
    return AssetEvent(AssetEventType::AssetUnloaded, guid, type, path);
}

AssetEvent AssetReloaded(const GUID& guid, AssetType type, const String& path) {
    return AssetEvent(AssetEventType::AssetReloaded, guid, type, path);
}

AssetEvent AssetLoadFailed(const GUID& guid, AssetType type, const String& path, const String& error) {
    return AssetEvent(AssetEventType::AssetLoadFailed, guid, type, path, error);
}

AssetEvent AssetCreated(const GUID& guid, AssetType type, const String& path) {
    return AssetEvent(AssetEventType::AssetCreated, guid, type, path);
}

AssetEvent AssetDestroyed(const GUID& guid, AssetType type, const String& path) {
    return AssetEvent(AssetEventType::AssetDestroyed, guid, type, path);
}

AssetEvent AssetModified(const GUID& guid, AssetType type, const String& path) {
    return AssetEvent(AssetEventType::AssetModified, guid, type, path);
}

} // namespace AssetEvents

} // namespace GameEngine
