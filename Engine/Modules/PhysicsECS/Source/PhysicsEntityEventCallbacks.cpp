#include "PhysicsECS/PhysicsEntityEventCallbacks.h"

#include "PhysicsECS/PhysicsWorldService.h"

#include <unordered_map>
#include <vector>

namespace GameEngine::PhysicsECS
{
namespace
{
enum class EntryKind : uint8
{
    ContactBegin,
    ContactEnd,
    TriggerEnter,
    TriggerExit,
};

struct Entry
{
    PhysicsEntityEventCallbacks::SubscriptionId id = 0;
    ECS::EntityHandle entity{};
    EntryKind kind = EntryKind::ContactBegin;

    PhysicsEntityEventCallbacks::ContactCallback contactCb{};
    PhysicsEntityEventCallbacks::TriggerCallback triggerCb{};
};

struct PerEntityLists
{
    std::vector<PhysicsEntityEventCallbacks::SubscriptionId> contactBegin;
    std::vector<PhysicsEntityEventCallbacks::SubscriptionId> contactEnd;
    std::vector<PhysicsEntityEventCallbacks::SubscriptionId> triggerEnter;
    std::vector<PhysicsEntityEventCallbacks::SubscriptionId> triggerExit;
};

static std::unordered_map<ECS::EntityHandle, PerEntityLists, ECS::EntityHandleHash> g_byEntity;
static std::unordered_map<PhysicsEntityEventCallbacks::SubscriptionId, Entry> g_entries;
static PhysicsEntityEventCallbacks::SubscriptionId g_nextId = 1;

static Physics::PhysicsWorld::ListenerId g_entityContactListenerId = 0;
static Physics::PhysicsWorld::ListenerId g_entityTriggerListenerId = 0;

static ECS::EntityHandle EntityRouterEntityFromUserData(uint64 ud)
{
    // Convention: PhysicsECS stores ECS entity id in userData.
    return ECS::EntityHandle(static_cast<uint32>(ud));
}

static bool HasAnyContactSubscriptions()
{
    for (const auto& kv : g_byEntity)
    {
        const auto& l = kv.second;
        if (!l.contactBegin.empty() || !l.contactEnd.empty())
            return true;
    }
    return false;
}

static bool HasAnyTriggerSubscriptions()
{
    for (const auto& kv : g_byEntity)
    {
        const auto& l = kv.second;
        if (!l.triggerEnter.empty() || !l.triggerExit.empty())
            return true;
    }
    return false;
}

static void EnsureWorldListeners()
{
    if (!PhysicsWorldService::IsInitialized())
        return;

    auto& w = PhysicsWorldService::Get();

    const bool wantContact = HasAnyContactSubscriptions();
    const bool wantTrigger = HasAnyTriggerSubscriptions();

    if (wantContact && g_entityContactListenerId == 0)
    {
        g_entityContactListenerId = w.AddContactListener([](const Physics::ContactEvent& e)
                                                   {
                                                       const ECS::EntityHandle a = EntityRouterEntityFromUserData(e.userDataA);
                                                       const ECS::EntityHandle b = EntityRouterEntityFromUserData(e.userDataB);
                                                       if (!a.IsValid() || !b.IsValid())
                                                           return;

                                                       if (e.state != Physics::ContactState::Begin && e.state != Physics::ContactState::End)
                                                           return;

                                                       const bool isBegin = (e.state == Physics::ContactState::Begin);

                                                       auto itA = g_byEntity.find(a);
                                                       if (itA != g_byEntity.end())
                                                       {
                                                           const auto& ids = isBegin ? itA->second.contactBegin : itA->second.contactEnd;
                                                           EntityContactEvent ev{};
                                                           ev.self = a;
                                                           ev.other = b;
                                                           ev.state = e.state;
                                                           ev.point = e.point;
                                                           ev.normal = e.normal;
                                                           ev.impulse = e.impulse;
                                                           ev.selfUserData = e.userDataA;
                                                           ev.otherUserData = e.userDataB;

                                                           for (const auto id : ids)
                                                           {
                                                               auto itE = g_entries.find(id);
                                                               if (itE != g_entries.end() && itE->second.contactCb)
                                                                   itE->second.contactCb(ev);
                                                           }
                                                       }

                                                       auto itB = g_byEntity.find(b);
                                                       if (itB != g_byEntity.end())
                                                       {
                                                           const auto& ids = isBegin ? itB->second.contactBegin : itB->second.contactEnd;
                                                           EntityContactEvent ev{};
                                                           ev.self = b;
                                                           ev.other = a;
                                                           ev.state = e.state;
                                                           ev.point = e.point;
                                                           ev.normal = e.normal;
                                                           ev.impulse = e.impulse;
                                                           ev.selfUserData = e.userDataB;
                                                           ev.otherUserData = e.userDataA;

                                                           for (const auto id : ids)
                                                           {
                                                               auto itE = g_entries.find(id);
                                                               if (itE != g_entries.end() && itE->second.contactCb)
                                                                   itE->second.contactCb(ev);
                                                           }
                                                       }
                                                   });
    }
    else if (!wantContact && g_entityContactListenerId != 0)
    {
        w.RemoveContactListener(g_entityContactListenerId);
        g_entityContactListenerId = 0;
    }

    if (wantTrigger && g_entityTriggerListenerId == 0)
    {
        g_entityTriggerListenerId = w.AddTriggerListener([](const Physics::TriggerEvent& e)
                                                   {
                                                       const ECS::EntityHandle trigger = EntityRouterEntityFromUserData(e.triggerUserData);
                                                       const ECS::EntityHandle other = EntityRouterEntityFromUserData(e.otherUserData);
                                                       if (!trigger.IsValid() || !other.IsValid())
                                                           return;

                                                       const bool isEnter = e.isEntering;

                                                       // Trigger perspective
                                                       if (auto it = g_byEntity.find(trigger); it != g_byEntity.end())
                                                       {
                                                           const auto& ids = isEnter ? it->second.triggerEnter : it->second.triggerExit;
                                                           EntityTriggerEvent ev{};
                                                           ev.self = trigger;
                                                           ev.other = other;
                                                           ev.selfIsTrigger = true;
                                                           ev.isEntering = e.isEntering;
                                                           ev.selfUserData = e.triggerUserData;
                                                           ev.otherUserData = e.otherUserData;

                                                           for (const auto id : ids)
                                                           {
                                                               auto itE = g_entries.find(id);
                                                               if (itE != g_entries.end() && itE->second.triggerCb)
                                                                   itE->second.triggerCb(ev);
                                                           }
                                                       }

                                                       // Other perspective
                                                       if (auto it = g_byEntity.find(other); it != g_byEntity.end())
                                                       {
                                                           const auto& ids = isEnter ? it->second.triggerEnter : it->second.triggerExit;
                                                           EntityTriggerEvent ev{};
                                                           ev.self = other;
                                                           ev.other = trigger;
                                                           ev.selfIsTrigger = false;
                                                           ev.isEntering = e.isEntering;
                                                           ev.selfUserData = e.otherUserData;
                                                           ev.otherUserData = e.triggerUserData;

                                                           for (const auto id : ids)
                                                           {
                                                               auto itE = g_entries.find(id);
                                                               if (itE != g_entries.end() && itE->second.triggerCb)
                                                                   itE->second.triggerCb(ev);
                                                           }
                                                       }
                                                   });
    }
    else if (!wantTrigger && g_entityTriggerListenerId != 0)
    {
        w.RemoveTriggerListener(g_entityTriggerListenerId);
        g_entityTriggerListenerId = 0;
    }
}

static void MaybeRemoveWorldListeners()
{
    if (!PhysicsWorldService::IsInitialized())
    {
        g_entityContactListenerId = 0;
        g_entityTriggerListenerId = 0;
        return;
    }

    auto& w = PhysicsWorldService::Get();

    const bool wantContact = HasAnyContactSubscriptions();
    const bool wantTrigger = HasAnyTriggerSubscriptions();

    if (!wantContact && g_entityContactListenerId != 0)
    {
        w.RemoveContactListener(g_entityContactListenerId);
        g_entityContactListenerId = 0;
    }

    if (!wantTrigger && g_entityTriggerListenerId != 0)
    {
        w.RemoveTriggerListener(g_entityTriggerListenerId);
        g_entityTriggerListenerId = 0;
    }
}

static PhysicsEntityEventCallbacks::SubscriptionId AddEntry(const Entry& e)
{
    if (!e.entity.IsValid())
        return 0;

    Entry stored = e;
    stored.id = g_nextId++;
    g_entries[stored.id] = stored;

    auto& lists = g_byEntity[stored.entity];
    switch (stored.kind)
    {
    case EntryKind::ContactBegin:
        lists.contactBegin.push_back(stored.id);
        break;
    case EntryKind::ContactEnd:
        lists.contactEnd.push_back(stored.id);
        break;
    case EntryKind::TriggerEnter:
        lists.triggerEnter.push_back(stored.id);
        break;
    case EntryKind::TriggerExit:
        lists.triggerExit.push_back(stored.id);
        break;
    }

    EnsureWorldListeners();
    return stored.id;
}

static void RemoveIdFromVector(std::vector<PhysicsEntityEventCallbacks::SubscriptionId>& v, PhysicsEntityEventCallbacks::SubscriptionId id)
{
    for (size_t i = 0; i < v.size(); ++i)
    {
        if (v[i] == id)
        {
            v[i] = v.back();
            v.pop_back();
            return;
        }
    }
}
} // namespace

PhysicsEntityEventCallbacks::SubscriptionId PhysicsEntityEventCallbacks::SubscribeContactBegin(ECS::EntityHandle entity, ContactCallback callback)
{
    Entry e{};
    e.entity = entity;
    e.kind = EntryKind::ContactBegin;
    e.contactCb = std::move(callback);
    return AddEntry(e);
}

PhysicsEntityEventCallbacks::SubscriptionId PhysicsEntityEventCallbacks::SubscribeContactEnd(ECS::EntityHandle entity, ContactCallback callback)
{
    Entry e{};
    e.entity = entity;
    e.kind = EntryKind::ContactEnd;
    e.contactCb = std::move(callback);
    return AddEntry(e);
}

PhysicsEntityEventCallbacks::SubscriptionId PhysicsEntityEventCallbacks::SubscribeTriggerEnter(ECS::EntityHandle entity, TriggerCallback callback)
{
    Entry e{};
    e.entity = entity;
    e.kind = EntryKind::TriggerEnter;
    e.triggerCb = std::move(callback);
    return AddEntry(e);
}

PhysicsEntityEventCallbacks::SubscriptionId PhysicsEntityEventCallbacks::SubscribeTriggerExit(ECS::EntityHandle entity, TriggerCallback callback)
{
    Entry e{};
    e.entity = entity;
    e.kind = EntryKind::TriggerExit;
    e.triggerCb = std::move(callback);
    return AddEntry(e);
}

void PhysicsEntityEventCallbacks::Unsubscribe(SubscriptionId id)
{
    const auto it = g_entries.find(id);
    if (it == g_entries.end())
        return;

    const Entry e = it->second;
    g_entries.erase(it);

    if (auto itL = g_byEntity.find(e.entity); itL != g_byEntity.end())
    {
        auto& lists = itL->second;
        switch (e.kind)
        {
        case EntryKind::ContactBegin:
            RemoveIdFromVector(lists.contactBegin, id);
            break;
        case EntryKind::ContactEnd:
            RemoveIdFromVector(lists.contactEnd, id);
            break;
        case EntryKind::TriggerEnter:
            RemoveIdFromVector(lists.triggerEnter, id);
            break;
        case EntryKind::TriggerExit:
            RemoveIdFromVector(lists.triggerExit, id);
            break;
        }

        if (lists.contactBegin.empty() && lists.contactEnd.empty() && lists.triggerEnter.empty() && lists.triggerExit.empty())
            g_byEntity.erase(itL);
    }

    MaybeRemoveWorldListeners();
}

void PhysicsEntityEventCallbacks::UnsubscribeAllForEntity(ECS::EntityHandle entity)
{
    auto it = g_byEntity.find(entity);
    if (it == g_byEntity.end())
        return;

    // Copy ids so we can mutate lists while removing.
    std::vector<SubscriptionId> ids;
    ids.insert(ids.end(), it->second.contactBegin.begin(), it->second.contactBegin.end());
    ids.insert(ids.end(), it->second.contactEnd.begin(), it->second.contactEnd.end());
    ids.insert(ids.end(), it->second.triggerEnter.begin(), it->second.triggerEnter.end());
    ids.insert(ids.end(), it->second.triggerExit.begin(), it->second.triggerExit.end());

    for (auto id : ids)
        Unsubscribe(id);
}

void PhysicsEntityEventCallbacks::ClearAll()
{
    g_byEntity.clear();
    g_entries.clear();
    MaybeRemoveWorldListeners();
}

void PhysicsEntityEventCallbacks::NotifyWorldStateChanged()
{
    // If the world just became available, install listeners if we have subscriptions.
    // If it was torn down, ids become invalid and we'll re-install on next init.
    if (!PhysicsWorldService::IsInitialized())
    {
        g_entityContactListenerId = 0;
        g_entityTriggerListenerId = 0;
        return;
    }

    EnsureWorldListeners();
}

} // namespace GameEngine::PhysicsECS

