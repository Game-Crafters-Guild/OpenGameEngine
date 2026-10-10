#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "EditorChangeNotifications.h"
#include "UndoRedo/IEditorCommand.h"

#include "Components/Animation/SkeletonRef.h"
#include "Components/Hierarchy.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/RuntimeOnlyEntity.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "PhysicsECS/Components/CharacterController.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/PhysicsWorldService.h"

namespace GameEngine::Editor
{
// Delete one entity (and optionally its subtree) as a single undoable command.
//
// - Redo: destroys the entity handles (preserve-handle) so Undo can revive them.
// - Undo: revives the handles and re-applies all captured component bytes.
//
// This is designed so a single Undo brings back the full entity/entities with all
// components and values, and a single Redo deletes them again.
//
// Replaying component bytes only restores state the bytes fully describe. A
// component naming a resource some system allocated does not qualify, and which
// way it fails depends on whether destroying the entity frees that resource:
//
// - A hook frees it (SkeletonRef). The id can be reissued to another spawn
//   before Undo replays it, and the revived entity would share a stranger's
//   resource. So while the entities are deleted this command owns the references
//   its snapshots name (see SkeletonStore.h's ownership rule), handing them back
//   on Undo or dropping them when the command itself is discarded.
//
// - A hook frees it and it cannot be held across the window (MeshGPUData). A
//   GPUScene instance slot is strictly one entity's — there is no reference
//   count to retain the way SkeletonRef's store allows — and the freed index is
//   the next one AllocateInstanceSlot reissues (LIFO). Extraction treats any
//   non-sentinel index as owned, so a replayed one writes whichever entity
//   holds the slot by then. The command scrubs its snapshots to the unassigned
//   default and extraction re-provisions on revive — see
//   ScrubSnapshotMeshGpuBridges.
//
// - Nothing frees it (PhysicsBody). The destroy leaves a body simulating with no
//   entity naming it, so the command frees it and revives an entity that owns
//   nothing — see ReleaseSnapshotPhysicsBodies.
//
// - The owning system sweeps it (MorphTargetWeights). MorphTargetSystem keys its
//   bake on the entity and retires entries for entities it stops visiting
//   (MorphTargetSystem.cpp), so the destroy frees the resource a tick later with
//   no help from this command, and the revived entity re-bakes because the handle
//   its restored bytes name is no longer in the registry. Nothing to do here — but
//   only because that sweep exists, so a new field of this kind still has to be
//   checked rather than assumed.
//
// The snapshot cannot arbitrate this on its own: it is taken in the constructor,
// and the duplicate path (DuplicateEntitiesCommand) commits before the clone has
// been provisioned at all, so by the time Redo runs the entity can own resources
// the snapshot has never heard of.
class DeleteEntitiesCommand final : public IEditorCommand
{
  public:
    struct EntitySnapshot
    {
        ECS::EntityHandle entity{};
        std::vector<std::pair<ECS::ComponentTypeId, std::vector<std::uint8_t>>> components;
    };

    // Build a delete command for a single root entity, including all descendants
    // that have Parent chains under that root.
    //
    // RuntimeOnlyEntity children are generator output (spline placement pieces,
    // fence posts and spans), not part of the user's entity. Their generator
    // retires them once the placer is gone and regenerates them if it comes
    // back, so capturing them here would revive a stale set alongside the fresh
    // one on Undo. `root` itself is never filtered: deleting a single piece by
    // hand is allowed, and the generator simply rebuilds it.
    static std::vector<ECS::EntityHandle> CollectSubtree(ECS::World& world, ECS::EntityHandle root)
    {
        std::vector<ECS::EntityHandle> out;
        if (!root.IsValid() || !world.IsValid(root))
            return out;

        // Build parent -> children adjacency using Parent components.
        std::unordered_map<std::uint32_t, std::vector<ECS::EntityHandle>> children;
        children.reserve(128);
        // Deleting a branch takes its disabled members with it, and undo has
        // to put them back, so the adjacency covers the whole scene.
        world.Query<ECS::Read<Components::Parent>>().IncludeDisabled().Each(
            [&](ECS::EntityHandle e, const Components::Parent& p)
            {
                if (!e.IsValid() || !world.IsValid(e))
                    return;
                if (!p.parent.IsValid() || !world.IsValid(p.parent))
                    return;
                if (world.HasComponent<Components::RuntimeOnlyEntity>(e))
                    return;
                children[p.parent.index].push_back(e);
            });

        std::vector<ECS::EntityHandle> stack;
        stack.push_back(root);
        std::unordered_set<std::uint32_t> visited;
        visited.reserve(128);

        while (!stack.empty())
        {
            ECS::EntityHandle cur = stack.back();
            stack.pop_back();
            if (!cur.IsValid() || !world.IsValid(cur))
                continue;
            if (!visited.insert(cur.index).second)
                continue;

            out.push_back(cur);

            auto it = children.find(cur.index);
            if (it != children.end())
            {
                for (auto& ch : it->second)
                {
                    stack.push_back(ch);
                }
            }
        }

        // Stable ordering: sort by (id) so undo/redo is deterministic.
        std::sort(out.begin(), out.end(), [](const ECS::EntityHandle& a, const ECS::EntityHandle& b) { return a.id < b.id; });
        return out;
    }

    DeleteEntitiesCommand(std::string name,
                         ECS::World* world,
                         EditorChangeNotifications* notifications,
                         std::vector<ECS::EntityHandle> entities)
        : m_Name(std::move(name))
        , m_World(world)
        , m_Notifications(notifications)
        , m_Entities(std::move(entities))
    {
        CaptureSnapshots();
    }

    ~DeleteEntitiesCommand() override
    {
        // Discarded while the entities are still deleted (undo stack trimmed,
        // redo branch dropped, or a caller that only ever runs Do()): nothing
        // will revive them, so the held references are ours to drop.
        ReleaseHeldRuntimeRefs();
    }

    // Owns store references while the entities are deleted; a copy would
    // release them twice.
    DeleteEntitiesCommand(const DeleteEntitiesCommand&) = delete;
    DeleteEntitiesCommand& operator=(const DeleteEntitiesCommand&) = delete;

    const char* GetName() const override { return m_Name.c_str(); }
    const char* GetTypeName() const override { return "DeleteEntitiesCommand"; }
    void Do() override { Redo(); }

    void Undo() override
    {
        if (!m_World)
            return;

        // Revive all handles first so Parent references can resolve.
        for (const auto& snap : m_Snapshots)
        {
            if (!snap.entity.IsValid())
                continue;
            (void)m_World->ReviveEntityImmediatePreserveHandle(snap.entity);
        }

        for (const auto& snap : m_Snapshots)
        {
            const bool revived = snap.entity.IsValid() && m_World->IsValid(snap.entity);
            if (revived)
            {
                for (const auto& [typeId, bytes] : snap.components)
                {
                    (void)m_World->ApplyComponentBytesImmediate(snap.entity, typeId, bytes);
                }
            }

            // A restored SkeletonRef names exactly the runtime we held alive
            // across the delete, so it inherits our reference rather than
            // taking a new one. An entity that failed to revive inherits
            // nothing, so its reference is ours to drop.
            if (m_HoldsRuntimeRefs && !revived)
                ReleaseRuntimeRefs(snap);
        }
        m_HoldsRuntimeRefs = false;

        NotifyWorldStructureChanged(EditorChangeNotifications::ChangeKind::Commit);
    }

    void Redo() override
    {
        if (!m_World)
            return;

        // Take the references before the destroys drop them, so no runtime the
        // snapshots name can be freed (and reissued) while we can still revive.
        RetainSnapshotRuntimeRefs();

        // And free the ones the destroys would NOT drop.
        ReleaseSnapshotPhysicsBodies();
        ReleaseSnapshotCharacterControllers();

        // And un-name the one the destroys DO free but whose snapshot bytes
        // Undo would otherwise replay.
        ScrubSnapshotMeshGpuBridges();

        // Generated children were deliberately left out of the snapshot (Undo must
        // not revive a stale set beside the one their generator rebuilds), but they
        // must still go: left parented to a dead entity they are one hierarchy
        // warning apiece until the generator's next sweep retires them. Destroyed,
        // not captured — the generator is what brings them back.
        DestroyGeneratedChildrenOfSnapshots();

        // Destroy in reverse order (children before parents) to avoid transient
        // Parent references to destroyed entities during the loop.
        for (std::size_t i = m_Snapshots.size(); i-- > 0;)
        {
            const auto& snap = m_Snapshots[i];
            if (!snap.entity.IsValid())
                continue;
            m_World->DestroyEntityImmediatePreserveHandle(snap.entity);
        }

        NotifyWorldStructureChanged(EditorChangeNotifications::ChangeKind::Commit);
    }

  private:
    // Every RuntimeOnlyEntity child of an entity this command is about to destroy.
    // CollectSubtree skipped them on the way in, so they are absent from the
    // snapshots and Undo cannot revive them; this is the matching half.
    void DestroyGeneratedChildrenOfSnapshots()
    {
        std::unordered_set<std::uint32_t> doomed;
        doomed.reserve(m_Snapshots.size() * 2u);
        for (const auto& snap : m_Snapshots)
        {
            if (snap.entity.IsValid())
                doomed.insert(snap.entity.index);
        }
        if (doomed.empty())
            return;

        std::vector<ECS::EntityHandle> generated;
        m_World->Query<ECS::Read<Components::Parent>>().IncludeDisabled().Each(
            [&](ECS::EntityHandle e, const Components::Parent& p)
            {
                if (!e.IsValid() || !p.parent.IsValid() || doomed.count(p.parent.index) == 0)
                    return;
                if (m_World->HasComponent<Components::RuntimeOnlyEntity>(e))
                    generated.push_back(e);
            });

        m_World->DestroyEntitiesImmediate(generated);
    }

    // Typed access to a snapshot's bytes for component T: the typeId match,
    // size guard and byte copies live here once. What each caller does with
    // the value is the per-component policy and stays at its call site — the
    // class comment's cases share this mechanism, not their logic, which is
    // why this is a pair of accessors and not a restore-policy interface.
    template <typename T, typename Fn>
    static void ForEachSnapshotComponent(const EntitySnapshot& snap, Fn&& fn)
    {
        static const ECS::ComponentTypeId typeId = ECS::GetComponentTypeId<T>();
        for (const auto& [id, bytes] : snap.components)
        {
            if (id != typeId || bytes.size() != sizeof(T))
                continue;
            T value{};
            std::memcpy(&value, bytes.data(), sizeof(T));
            fn(static_cast<const T&>(value));
        }
    }

    template <typename T, typename Fn>
    static void MutateSnapshotComponents(EntitySnapshot& snap, Fn&& fn)
    {
        static const ECS::ComponentTypeId typeId = ECS::GetComponentTypeId<T>();
        for (auto& [id, bytes] : snap.components)
        {
            if (id != typeId || bytes.size() != sizeof(T))
                continue;
            T value{};
            std::memcpy(&value, bytes.data(), sizeof(T));
            fn(value);
            std::memcpy(bytes.data(), &value, sizeof(T));
        }
    }

    // One reference per snapshotted SkeletonRef, mirroring the one each live
    // component holds — so the retain/destroy pair nets to zero and Undo can
    // hand them straight back.
    static void ForEachRuntimeId(const EntitySnapshot& snap,
                                 const std::function<void(std::uint32_t)>& fn)
    {
        ForEachSnapshotComponent<Components::SkeletonRef>(
            snap,
            [&fn](const Components::SkeletonRef& ref)
            {
                if (ref.runtimeId != 0)
                    fn(ref.runtimeId);
            });
    }

    static void ReleaseRuntimeRefs(const EntitySnapshot& snap)
    {
        auto& store = Engine::Renderer::SkeletonStore::Instance();
        ForEachRuntimeId(snap, [&store](std::uint32_t runtimeId) { store.ReleaseRuntime(runtimeId); });
    }

    void RetainSnapshotRuntimeRefs()
    {
        if (m_HoldsRuntimeRefs)
            return;
        auto& store = Engine::Renderer::SkeletonStore::Instance();
        for (const auto& snap : m_Snapshots)
            ForEachRuntimeId(snap, [&store](std::uint32_t runtimeId) { (void)store.RetainRuntime(runtimeId); });
        m_HoldsRuntimeRefs = true;
    }

    void ReleaseHeldRuntimeRefs()
    {
        if (!m_HoldsRuntimeRefs)
            return;
        for (const auto& snap : m_Snapshots)
            ReleaseRuntimeRefs(snap);
        m_HoldsRuntimeRefs = false;
    }

    // RegisterOnRemove<PhysicsBody> (PhysicsWorldHooks) also frees a destroyed
    // body, but only the live component — it cannot reach the snapshot, and the
    // snapshot is what Undo revives from. Scrubbing that is this function's
    // irreducible half. The freeing half runs first and clears as it goes, so
    // the hook finds zeroed handles and bit-guards; it also stands alone for a
    // world that never had hooks registered.
    //
    // Reads the LIVE component rather than the snapshot, because the snapshot is
    // the one thing guaranteed to be out of date here (see the class comment),
    // then scrubs the snapshot so Undo revives an entity that owns nothing and
    // PhysicsInitSystem provisions it fresh — the state a scene load produces.
    //
    // Both halves are gated on the physics world agreeing the handle is live, so
    // this is a no-op outside Play (no world), for an entity whose body was
    // already freed by its owning system, and for one that never had a body.
    void ReleaseSnapshotPhysicsBodies()
    {
        auto* physicsWorld = PhysicsECS::PhysicsWorldService::TryGet();

        for (auto& snap : m_Snapshots)
        {
            if (!snap.entity.IsValid() || !m_World->IsValid(snap.entity))
                continue;

            if (auto* live = m_World->GetComponentForWrite<Components::PhysicsBody>(snap.entity))
            {
                if (physicsWorld && live->initialized)
                {
                    if (physicsWorld->IsBodyValid(live->body))
                        physicsWorld->DestroyBody(live->body);
                    if (physicsWorld->IsShapeValid(live->shape))
                        physicsWorld->DestroyShape(live->shape);
                    for (std::uint16_t i = 0; i < live->childShapeCount; ++i)
                    {
                        if (physicsWorld->IsShapeValid(live->childShapes[i]))
                            physicsWorld->DestroyShape(live->childShapes[i]);
                    }
                }

                // The entity outlives this call by the rest of the destroy loop,
                // so it must not be left naming freed handles.
                Components::ClearPhysicsBodyRuntimeState(*live);
            }

            MutateSnapshotComponents<Components::PhysicsBody>(
                snap, [](Components::PhysicsBody& restored)
                { Components::ClearPhysicsBodyRuntimeState(restored); });
        }
    }

    // Same class as ReleaseSnapshotPhysicsBodies: a CharacterHandle is the
    // physics world's, and the snapshot cannot own it. OnRemove frees the live
    // component when hooks are registered; the snapshot still has to revive an
    // entity that owns nothing so CharacterControllerSystem provisions fresh.
    void ReleaseSnapshotCharacterControllers()
    {
        auto* physicsWorld = PhysicsECS::PhysicsWorldService::TryGet();

        for (auto& snap : m_Snapshots)
        {
            if (!snap.entity.IsValid() || !m_World->IsValid(snap.entity))
                continue;

            if (auto* live = m_World->GetComponentForWrite<Components::CharacterController>(snap.entity))
            {
                if (physicsWorld && live->initialized)
                {
                    if (physicsWorld->IsCharacterValid(live->character))
                        physicsWorld->DestroyCharacter(live->character);
                }
                Components::ClearCharacterControllerRuntimeState(*live);
            }

            MutateSnapshotComponents<Components::CharacterController>(
                snap, [](Components::CharacterController& restored)
                { Components::ClearCharacterControllerRuntimeState(restored); });
        }
    }

    // The destroy loop's RegisterOnRemove<MeshGPUData> hook (Engine.cpp,
    // ReleaseMeshGpuInstance) frees the GPUScene slot the LIVE component names,
    // so unlike PhysicsBody there is no freeing half here. What the hook cannot
    // reach is the snapshot: replayed on Undo it would revive the freed index —
    // by then AllocateInstanceSlot's next reissue — and extraction's only
    // ownership test is index != sentinel (RenderExtractionSystem's Op::Add
    // gate), so the revived entity would drive another entity's GPU instance.
    //
    // The whole component is the runtime bridge extraction rebuilds
    // ([DoNotSerialize]), so the snapshot restores the default-constructed
    // state — the same reset a copy gets (CopiedEntityRuntimeRefs) — and the
    // revived entity re-enters through Op::Add. hlodEvicted resets with it,
    // deliberately: HLODSelectSystem rewrites the flag only on residency flips,
    // so a preserved 'true' whose clearing flip happened inside the delete
    // window would strand the entity invisible, while a reset 'false' costs at
    // most a duplicate draw until the next flip — the trade the copy path
    // already accepts.
    void ScrubSnapshotMeshGpuBridges()
    {
        for (auto& snap : m_Snapshots)
        {
            MutateSnapshotComponents<Components::MeshGPUData>(
                snap, [](Components::MeshGPUData& restored)
                { restored = Components::MeshGPUData{}; });
        }
    }

    void CaptureSnapshots()
    {
        m_Snapshots.clear();
        if (!m_World)
            return;

        // Ensure entities are unique and valid.
        std::vector<ECS::EntityHandle> ents = m_Entities;
        std::sort(ents.begin(), ents.end(), [](const ECS::EntityHandle& a, const ECS::EntityHandle& b) { return a.id < b.id; });
        ents.erase(std::unique(ents.begin(), ents.end(), [](const ECS::EntityHandle& a, const ECS::EntityHandle& b) { return a.id == b.id; }),
                   ents.end());

        for (auto& e : ents)
        {
            if (!e.IsValid() || !m_World->IsValid(e))
                continue;

            EntitySnapshot snap{};
            snap.entity = e;

            ECS::Archetype* a = m_World->GetEntityArchetype(e);
            if (!a)
                continue;

            const auto typeIds = a->GetSignature().GetComponents();
            snap.components.reserve(typeIds.size());
            for (auto typeId : typeIds)
            {
                std::vector<std::uint8_t> bytes;
                if (m_World->CaptureComponentBytes(e, typeId, bytes))
                {
                    snap.components.emplace_back(typeId, std::move(bytes));
                }
            }

            m_Snapshots.push_back(std::move(snap));
        }
    }

    void NotifyWorldStructureChanged(EditorChangeNotifications::ChangeKind kind)
    {
        if (!m_Notifications || !m_World)
            return;
        EditorChangeNotifications::WorldStructureChangedEvent e{};
        e.world = m_World;
        e.kind = kind;
        m_Notifications->NotifyWorldStructureChanged(e);
    }

  private:
    std::string m_Name;
    ECS::World* m_World = nullptr;                      // not owned
    EditorChangeNotifications* m_Notifications = nullptr; // not owned
    std::vector<ECS::EntityHandle> m_Entities;
    std::vector<EntitySnapshot> m_Snapshots;
    bool m_HoldsRuntimeRefs = false;
};

} // namespace GameEngine::Editor

