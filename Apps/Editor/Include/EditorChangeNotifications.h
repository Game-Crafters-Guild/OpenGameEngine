#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "Core/CpuProfiler.h"
#include "ECS/ECS.h"

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
// Lightweight publish/subscribe channel for editor-side state changes.
// This intentionally stays small and concrete (no generic "event bus" naming)
// and can be expanded with additional event types as the editor grows.
class EditorChangeNotifications
{
public:
    enum class ChangeKind : std::uint8_t
    {
        Preview,         // live preview (dragging / scrubbing)
        Commit,          // committed change from user interaction (end of drag, field commit)
        UndoRedo,        // committed change from undo or redo — recipients may need to rebuild UI
        InspectorRebuild // scene data unchanged; inspector UI should refresh (e.g. async download status)
    };

    struct ComponentChangedEvent
    {
        ECS::World* world = nullptr;
        ECS::EntityHandle entity{};
        ECS::ComponentTypeId componentType = 0;
        ChangeKind kind = ChangeKind::Commit;
    };

    using ComponentChangedCallback = std::function<void(const ComponentChangedEvent&)>;

    // Structural changes that affect world membership/hierarchy (create/destroy/reparent).
    // These are not tied to a single component type.
    struct WorldStructureChangedEvent
    {
        ECS::World* world = nullptr;
        ChangeKind kind = ChangeKind::Commit;
    };

    using WorldStructureChangedCallback = std::function<void(const WorldStructureChangedEvent&)>;

    struct SubscriptionToken
    {
        std::uint64_t id = 0;
        explicit operator bool() const { return id != 0; }
    };

    SubscriptionToken SubscribeComponentChanged(ComponentChangedCallback cb)
    {
        if (!cb)
            return {};
        std::lock_guard<std::mutex> lock(m_Mutex);
        const std::uint64_t id = m_NextId++;
        m_ComponentChanged.emplace(id, std::make_shared<ComponentSlot>(std::move(cb)));
        return SubscriptionToken{id};
    }

    SubscriptionToken SubscribeWorldStructureChanged(WorldStructureChangedCallback cb)
    {
        if (!cb)
            return {};
        std::lock_guard<std::mutex> lock(m_Mutex);
        const std::uint64_t id = m_NextId++;
        m_WorldStructureChanged.emplace(id, std::make_shared<WorldStructureSlot>(std::move(cb)));
        return SubscriptionToken{id};
    }

    // Unsubscribe marks the slot dead before erasing so any in-flight dispatch
    // snapshot that still holds a shared_ptr to the slot will skip the callback
    // rather than call it on a destroyed subscriber.
    void Unsubscribe(SubscriptionToken token)
    {
        if (!token)
            return;
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (auto it = m_ComponentChanged.find(token.id); it != m_ComponentChanged.end())
        {
            if (it->second)
                it->second->Alive.store(false, std::memory_order_release);
            m_ComponentChanged.erase(it);
        }
        if (auto it = m_WorldStructureChanged.find(token.id); it != m_WorldStructureChanged.end())
        {
            if (it->second)
                it->second->Alive.store(false, std::memory_order_release);
            m_WorldStructureChanged.erase(it);
        }
    }

    template<typename T>
    void NotifyComponentCommit(ECS::World* world, ECS::EntityHandle entity)
    {
        NotifyComponentChanged({world, entity, ECS::GetComponentTypeId<T>(), ChangeKind::Commit});
    }

    template<typename T>
    void NotifyComponentChange(ECS::World* world, ECS::EntityHandle entity, ChangeKind kind)
    {
        NotifyComponentChanged({world, entity, ECS::GetComponentTypeId<T>(), kind});
    }

    void NotifyComponentChanged(const ComponentChangedEvent& e)
    {
        GE_CPU_PROFILE_SCOPE("EditorChangeNotifications.NotifyComponentChanged");
        // Snapshot slots so subscribe/unsubscribe is safe during dispatch.
        // The Alive flag on each slot lets Unsubscribe invalidate in-flight
        // snapshots when an earlier callback tears down later subscribers
        // (e.g. InspectorPanel rebuilding sections mid-notify after Ctrl+Z).
        std::vector<std::shared_ptr<ComponentSlot>> snap;
        {
            GE_CPU_PROFILE_SCOPE("EditorChangeNotifications.NotifyComponentChanged.Snapshot");
            std::lock_guard<std::mutex> lock(m_Mutex);
            snap.reserve(m_ComponentChanged.size());
            for (auto& kv : m_ComponentChanged)
            {
                if (kv.second)
                    snap.push_back(kv.second);
            }
        }

        for (auto& slot : snap)
        {
            GE_CPU_PROFILE_SCOPE("EditorChangeNotifications.NotifyComponentChanged.Dispatch");
            if (slot && slot->Alive.load(std::memory_order_acquire) && slot->Cb)
                slot->Cb(e);
        }
    }

    void NotifyWorldStructureChanged(const WorldStructureChangedEvent& e)
    {
        std::vector<std::shared_ptr<WorldStructureSlot>> snap;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            snap.reserve(m_WorldStructureChanged.size());
            for (auto& kv : m_WorldStructureChanged)
            {
                if (kv.second)
                    snap.push_back(kv.second);
            }
        }

        for (auto& slot : snap)
        {
            if (slot && slot->Alive.load(std::memory_order_acquire) && slot->Cb)
                slot->Cb(e);
        }
    }

private:
    struct ComponentSlot
    {
        std::atomic<bool> Alive{true};
        ComponentChangedCallback Cb;
        explicit ComponentSlot(ComponentChangedCallback c) : Cb(std::move(c)) {}
    };
    struct WorldStructureSlot
    {
        std::atomic<bool> Alive{true};
        WorldStructureChangedCallback Cb;
        explicit WorldStructureSlot(WorldStructureChangedCallback c) : Cb(std::move(c)) {}
    };

    std::mutex m_Mutex;
    std::uint64_t m_NextId = 1;
    std::unordered_map<std::uint64_t, std::shared_ptr<ComponentSlot>> m_ComponentChanged;
    std::unordered_map<std::uint64_t, std::shared_ptr<WorldStructureSlot>> m_WorldStructureChanged;
};

/** Publish a committed structure change of `world`. A null `notifications` is a no-op. */
inline void NotifyWorldStructure(EditorChangeNotifications* notifications, ECS::World* world)
{
    if (!notifications)
        return;
    EditorChangeNotifications::WorldStructureChangedEvent e{};
    e.world = world;
    e.kind = EditorChangeNotifications::ChangeKind::Commit;
    notifications->NotifyWorldStructureChanged(e);
}

} // namespace GameEngine::Editor


