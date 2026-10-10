#pragma once

// AssetReloadInvalidator: RAII subscriber for asset eviction events.
//
// Wraps AddCallback / RemoveCallback so caches that need to evict their
// per-asset state on hot-reload don't have to track callback handles
// manually. Which dispatcher events fire the handler is selected by
// EventSet; the handler is a single GUID since per-event distinction isn't
// useful at the cache layer (eviction is idempotent and identical
// regardless of which event triggered it).
//
// AssetUnloaded coverage closes a leak class where unloading a model
// (scene switch, asset eviction) would leave its GPU resources +
// per-parent derived caches permanently held.
//
// Construct as a member of the cache; pass a lambda that invalidates the
// entry. Destruction automatically removes the callback. Non-copyable;
// movable so registries may be relocated during owner construction.

#include "AssetCore/AssetEvents.h"
#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include "AssetCore/Types.h"

#include <functional>
#include <utility>

namespace GameEngine
{

class AssetReloadInvalidator
{
public:
    using ReloadHandler = std::function<void(const GUID&)>;

    // Which dispatcher events fire the handler.
    enum class EventSet : uint8
    {
        // Main-thread AssetReloaded only — for handlers that touch
        // single-threaded state (registries, GPU resources) directly.
        ReloadedOnly,

        // Reloaded + Unloaded + Destroyed (the default): every "drop the
        // cache entry" event for assets resident in the AssetManager.
        // Unloaded/Destroyed coverage prevents leaks when assets exit memory
        // (scene switch, asset eviction) — not just on in-place reload.
        DropEvents,

        // DropEvents + Modified + Created — for caches that decode straight
        // from disk. A plain file edit dispatches AssetModified, never
        // AssetReloaded (that only fires for assets resident in the manager's
        // loaded set, which one-shot decodes never enter), and Created
        // re-arms negative-cached decode failures when the file appears.
        // Modified/Created fire on the watcher thread — handlers must be
        // enqueue-only. Over-firing is fine at the cache layer: eviction is
        // idempotent.
        ContentEvents,
    };

    AssetReloadInvalidator() = default;

    // THREADING: only AssetReloaded is dispatched from the main thread
    // (AssetManager::Update -> CheckForReloads). Every other event dispatches
    // SYNCHRONOUSLY on whichever thread raised it — file changes/deletions
    // arrive on the file-watcher thread. Handlers that touch single-threaded
    // state (registries, GPU resources) must pass EventSet::ReloadedOnly.
    AssetReloadInvalidator(AssetEventDispatcher& dispatcher,
                           AssetType type,
                           ReloadHandler onReload,
                           EventSet events = EventSet::DropEvents)
        : m_Dispatcher(&dispatcher), m_Type(type)
    {
        if (!onReload)
            return;

        // Capture the handler by value into the dispatcher callback. We
        // intentionally filter by event type + asset type here so the
        // handler only sees notifications for the asset type it cares about.
        m_Handle = m_Dispatcher->AddCallback(
            [type, events, handler = std::move(onReload)](const AssetEvent& event)
            {
                if (!Selects(events, event.EventType))
                    return;
                // TRACKED: bulk-destroy paths can dispatch AssetDestroyed with
                // AssetType::Unknown (no metadata left to classify) — this
                // type filter drops those, so such assets never invalidate.
                if (event.Type != type)
                    return;
                handler(event.AssetGuid);
                // A cache may have filed the asset under a GUID that redirects to it; the
                // event names the asset by the GUID the redirect resolves to, so each source
                // is invalidated as well. Eviction is idempotent, so a GUID the cache never
                // held costs one failed lookup.
                for (const GUID& source : event.RedirectSources)
                    handler(source);
            });
    }

    ~AssetReloadInvalidator()
    {
        Reset();
    }

    AssetReloadInvalidator(const AssetReloadInvalidator&) = delete;
    AssetReloadInvalidator& operator=(const AssetReloadInvalidator&) = delete;

    AssetReloadInvalidator(AssetReloadInvalidator&& other) noexcept
        : m_Dispatcher(other.m_Dispatcher), m_Type(other.m_Type), m_Handle(other.m_Handle)
    {
        other.m_Dispatcher = nullptr;
        other.m_Handle = 0;
    }

    AssetReloadInvalidator& operator=(AssetReloadInvalidator&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_Dispatcher = other.m_Dispatcher;
            m_Type = other.m_Type;
            m_Handle = other.m_Handle;
            other.m_Dispatcher = nullptr;
            other.m_Handle = 0;
        }
        return *this;
    }

    // Detach from the dispatcher early. Safe to call multiple times.
    void Reset()
    {
        if (m_Dispatcher && m_Handle != 0)
        {
            m_Dispatcher->RemoveCallback(m_Handle);
        }
        m_Dispatcher = nullptr;
        m_Handle = 0;
    }

    bool IsActive() const { return m_Dispatcher != nullptr && m_Handle != 0; }

private:
    static bool Selects(EventSet events, AssetEventType type)
    {
        switch (type)
        {
        case AssetEventType::AssetReloaded:
            return true;
        case AssetEventType::AssetUnloaded:
        case AssetEventType::AssetDestroyed:
            return events != EventSet::ReloadedOnly;
        case AssetEventType::AssetModified:
        case AssetEventType::AssetCreated:
            return events == EventSet::ContentEvents;
        default:
            return false;
        }
    }

    AssetEventDispatcher* m_Dispatcher = nullptr;
    AssetType m_Type = AssetType::Unknown;
    uint32 m_Handle = 0;
};

} // namespace GameEngine
