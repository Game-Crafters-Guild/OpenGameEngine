#pragma once

#include "Components/Animation/Animator.h"
#include "Components/Animation/SkeletonRef.h"
#include "Components/Markup/Markup.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/MorphTargetWeights.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "PathfindingECS/Components/NavigationGrid.h"
#include "PathfindingECS/NavigationGridRuntime.h"
#include "PhysicsECS/Components/CharacterController.h"
#include "PhysicsECS/Components/HeightFieldColliderShape.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "SplineECS/SplineService.h"
#include "TerrainECS/Components/TerrainPlanetFaceCollider.h"
#include "TerrainECS/Components/TerrainTileCollider.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <utility>

namespace GameEngine::Editor
{
// Copying an entity copies its component bytes verbatim (World::CloneEntity
// CopyRows every column), so a field naming a resource some system ALLOCATED —
// a store runtime, a GPU scene slot, a physics body — comes out aliased: the
// copy names something it does not own. What that costs varies by resource —
// the source's teardown frees it underneath the copy, the copy's own
// provisioning tears it out from under the source, or the two are simply driven
// as one — so each field below carries its own argument for what it resets to.
//
// This is the one place that repairs those fields for a copy that becomes a NEW
// instance, so a new runtime-owned field is handled once rather than once per
// caller. Reviving a DELETED entity is the deliberate exception and does not
// route through here: it restores the SAME identity, so DeleteEntitiesCommand
// carries each field across the delete window itself. How it carries one depends
// on whether the destroy frees it — a reference-counted runtime is retained and
// handed straight back, while a physics body is freed by the command before the
// destroy and rebuilt from the authoring data on revive. Adding
// a field here means deciding which of those it is; leaving that undecided is
// how a resource ends up owned by nobody once the entity is gone.
//
// Scope is the whole copied payload, not one entity. A skinned model instance
// is several entities sharing ONE skeleton runtime, so the repair memoizes on
// the SOURCE handle: entities that shared a runtime still share the new one.
// Repairing each entity in isolation would hand every copied submesh its own
// runtime and take the instance apart.
//
// Repair reads only the copy's OWN components, never the source entity. It does
// still consult the store by the copied runtime id, so it requires a source that
// is alive and holding its reference: once a slot is recycled that id names a
// stranger's runtime and nothing about it is a validity test (SkeletonStore.h,
// RetainRuntime).
class CopiedEntityRuntimeRefs
{
  public:
    CopiedEntityRuntimeRefs() = default;

    // Holds the construction reference on every runtime it creates, so a
    // payload abandoned partway through still releases them.
    ~CopiedEntityRuntimeRefs()
    {
        auto& store = Engine::Renderer::SkeletonStore::Instance();
        for (const auto& [sourceRuntimeId, createdRuntimeId] : m_SkeletonRuntimeBySource)
            store.ReleaseRuntime(createdRuntimeId);
    }

    // Owns store references for the payload's lifetime; a copy would release
    // them twice.
    CopiedEntityRuntimeRefs(const CopiedEntityRuntimeRefs&) = delete;
    CopiedEntityRuntimeRefs& operator=(const CopiedEntityRuntimeRefs&) = delete;

    // Which copy stands in for a source entity. A SkeletonRef names its
    // instance owner by handle, and a copy must name the copied owner, not
    // the source's; record every clone before repairing it so an owner
    // cloned earlier in the same payload resolves.
    void RecordClone(ECS::EntityHandle source, ECS::EntityHandle copy)
    {
        m_CloneBySource.emplace(source.id, copy);
    }

    // Repair one freshly materialized copy. Use ONE instance across a whole
    // payload so entities that shared a runtime still share one.
    void Repair(ECS::World& world, ECS::EntityHandle copy)
    {
        if (!copy.IsValid() || !world.IsValid(copy))
            return;

        // Each step re-fetches its component: an absent one turns the write
        // into a structural add, which moves the entity and invalidates any
        // pointer held across steps.
        RepairMeshGpuData(world, copy);
        RepairMorphTargetRuntime(world, copy);
        RepairWorldTransform(world, copy);
        RepairSkeletonRuntime(world, copy);
        RepairSplineData(world, copy);
        RepairTerrain(world, copy);
        RepairHeightFieldCollider(world, copy);
        RepairPhysicsBody(world, copy);
        RepairCharacterController(world, copy);
        RepairAnimatorRuntime(world, copy);
        RepairTerrainColliders(world, copy);
        RepairNavigationGrid(world, copy);
    }

    // Repair what a copy names of another entity of the payload, once every entity of it is
    // cloned (call after the payload's last RecordClone): Repair runs on each copy as it is
    // made, parents before children, so a reference to a child or a sibling cloned later
    // cannot resolve there.
    void RepairPayloadReferences(ECS::World& world)
    {
        for (const auto& [sourceId, copy] : m_CloneBySource)
            RepairMarkupRegionMembers(world, copy);
    }

  private:
    // A region's member list is byte-copied: a member duplicated in the same payload (a
    // region's own clearings, copied as its children) names its copy, so the copy is
    // independent; a member duplicated without it (a lake two regions exclude) stays shared.
    void RepairMarkupRegionMembers(ECS::World& world, ECS::EntityHandle copy) const
    {
        const auto* region = world.GetComponent<Components::MarkupRegion>(copy);
        if (!region || !world.IsValid(copy))
            return;
        Components::MarkupRegion updated = *region;
        bool remapped = false;
        for (uint32 i = 0; i < std::min(updated.MemberCount, Components::kMaxRegionMembers); ++i)
        {
            const ECS::EntityHandle member = updated.Members[i].Entity;
            updated.Members[i].Entity = CloneOf(member, member);
            remapped = remapped || updated.Members[i].Entity != member;
        }
        if (remapped)
            world.AddComponentImmediate(copy, updated);
    }

    // The GPUScene index belongs to the entity render extraction assigned it
    // to; a copy starts unassigned and is given its own.
    static void RepairMeshGpuData(ECS::World& world, ECS::EntityHandle copy)
    {
        if (!world.GetComponent<Components::MeshRenderer>(copy))
            return;
        world.AddComponentImmediate(copy, Components::MeshGPUData{});
    }

    // runtimeModelGuid / runtimeMeshGpuHandleId name a mesh MorphTargetSystem
    // baked from ONE entity's weights and registered under a GUID derived from
    // that entity's id, so the entry can never be shared: the copy's own id
    // yields a different GUID. Left copied, the copy's first update treats the
    // source's entry as its own stale one and unregisters it — MeshGPURegistry
    // is not reference counted, so that single call destroys it.
    //
    // Cleared, the copy has no previous entry to retire and bakes its own. The
    // authoring weights and the sourceMesh* fields stay: they name the shared
    // model submesh every instance registers, which no instance unregisters.
    // The map a NavigationGrid names belongs to the entity that created it. The
    // copy forgets the binding and NavigationBuildSystem creates its own map;
    // sharing it would let the copy's teardown release the original's map.
    static void RepairNavigationGrid(ECS::World& world, ECS::EntityHandle copy)
    {
        const auto* grid = world.GetComponent<Components::NavigationGrid>(copy);
        if (!grid || !grid->Initialized)
            return;

        Components::NavigationGrid updated = *grid;
        PathfindingECS::ForgetNavigationGridMap(updated);
        world.AddComponentImmediate(copy, updated);
    }

    static void RepairMorphTargetRuntime(ECS::World& world, ECS::EntityHandle copy)
    {
        const auto* morph = world.GetComponent<Components::MorphTargetWeights>(copy);
        if (!morph)
            return;

        Components::MorphTargetWeights updated = *morph;
        std::memset(updated.runtimeModelGuid, 0, sizeof(updated.runtimeModelGuid));
        updated.runtimeMeshGpuHandleId = 0;
        world.AddComponentImmediate(copy, updated);
    }

    // The copied world matrix is the right starting value — the copy sits
    // where the source did until its own hierarchy update runs. The version
    // bump is what makes consumers re-read it.
    static void RepairWorldTransform(ECS::World& world, ECS::EntityHandle copy)
    {
        if (!world.GetComponent<Components::Transform>(copy))
            return;

        Components::WorldTransform wt{};
        if (const auto* copied = world.GetComponent<Components::WorldTransform>(copy))
            wt = *copied;
        else if (const auto* local = world.GetComponent<Components::Transform>(copy))
            std::memcpy(wt.matrix, local->matrix, sizeof(wt.matrix));
        ++wt.Version;
        world.AddComponentImmediate(copy, wt);
    }

    // A model instance's entities share one reference-counted runtime
    // (SkeletonStore.h ownership rule). A copy is a NEW instance: it gets its
    // own runtime so the two animate independently, and every copied
    // SkeletonRef naming the same source runtime lands on that same new one.
    // The copy also takes that runtime's generation and names the copied
    // owner, and the runtime records the copy's source identity, so the
    // shared resolver keeps this runtime instead of regrouping the copy under
    // the source or leaving the repair unreleasable.
    void RepairSkeletonRuntime(ECS::World& world, ECS::EntityHandle copy)
    {
        const auto* ref = world.GetComponent<Components::SkeletonRef>(copy);
        if (!ref || ref->runtimeId == 0)
            return;

        auto& store = Engine::Renderer::SkeletonStore::Instance();
        Components::SkeletonRef updated = *ref;
        updated.runtimeId = RuntimeForSource(updated.runtimeId, updated.skeletonId);
        updated.runtimeGeneration = store.GetRuntimeGeneration(updated.runtimeId);
        // A part copied without its owner becomes its own instance rather
        // than a stranger's.
        if (updated.ownerMode == Components::SkeletonInstanceOwner::Entity)
            updated.instanceOwner = CloneOf(updated.instanceOwner, copy);
        if (auto* runtime = store.GetRuntime(updated.runtimeId))
        {
            runtime->Source.ModelGuid = updated.sourceModelGuid.ToGuid();
            runtime->Source.InstanceOwner =
                updated.ownerMode == Components::SkeletonInstanceOwner::Entity ? updated.instanceOwner : copy;
            runtime->Source.WorldId = world.GetWorldId();
            runtime->Source.WorldGeneration = world.GetLifecycleResetGeneration();
        }

        // Overwriting the component does NOT run the remove hook — World's set
        // path is a data-only write — which is what this needs: the copy never
        // held a reference on the source runtime to drop.
        world.AddComponentImmediate(copy, updated);

        // 0 means the runtime could not be recreated. Storing 0 rather than the
        // aliased id is what keeps that failure safe: 0 is the invalid sentinel
        // (SkeletonRef.h) and every consumer skips it, so the copy renders
        // unskinned instead of aliasing a runtime it does not own.
        if (updated.runtimeId != 0)
            store.RetainRuntime(updated.runtimeId);
    }

    ECS::EntityHandle CloneOf(ECS::EntityHandle source, ECS::EntityHandle fallback) const
    {
        const auto it = m_CloneBySource.find(source.id);
        return it != m_CloneBySource.end() ? it->second : fallback;
    }

    std::uint32_t RuntimeForSource(std::uint32_t sourceRuntimeId, std::uint32_t skeletonId)
    {
        const auto it = m_SkeletonRuntimeBySource.find(sourceRuntimeId);
        if (it != m_SkeletonRuntimeBySource.end())
            return it->second;

        auto& store = Engine::Renderer::SkeletonStore::Instance();

        // The store fills in a skeleton the component does not name, and
        // corroborates one it does. It is only believed when the two agree,
        // because a recycled slot answers for a stranger just as confidently:
        // on disagreement the component's own id is the trustworthy one.
        const std::uint32_t storeSkeletonId = store.GetRuntimeSkeletonId(sourceRuntimeId);
        const bool storeAgrees =
            storeSkeletonId != 0 && (skeletonId == 0 || storeSkeletonId == skeletonId);
        const std::uint32_t createdRuntimeId =
            store.CreateRuntime(storeAgrees ? storeSkeletonId : skeletonId);

        // Memoized even when 0, so a failed create is not retried per entity.
        m_SkeletonRuntimeBySource.emplace(sourceRuntimeId, createdRuntimeId);

        // Same reason the pose only travels when the slot is corroborated: an
        // uncorroborated one would pose the copy from a stranger's matrices.
        if (createdRuntimeId != 0 && storeAgrees)
            CopySourcePose(store, sourceRuntimeId, createdRuntimeId);
        return createdRuntimeId;
    }

    // The copy appears already posed instead of snapping to bind pose until
    // something re-evaluates the animation. Only the pose travels: the atlas
    // offsets name slots the atlas allocated for the source this frame, and a
    // runtime with no history of its own must report none (SkeletonStore.h)
    // rather than point the motion-vector pass at another runtime's slot.
    static void CopySourcePose(Engine::Renderer::SkeletonStore& store,
                               std::uint32_t sourceRuntimeId,
                               std::uint32_t createdRuntimeId)
    {
        const auto* source = store.GetRuntime(sourceRuntimeId);
        auto* created = store.GetRuntime(createdRuntimeId);
        if (source && created)
            created->CompactSkinMatrices = source->CompactSkinMatrices;
    }

    // SplineComponent names an entry in the SplineService, which is not
    // reference counted, so the copy gets a deep copy of the source's points.
    static void RepairSplineData(ECS::World& world, ECS::EntityHandle copy)
    {
        const auto* splineComp = world.GetComponent<Components::SplineComponent>(copy);
        if (!splineComp)
            return;

        auto* splineService = SplineECS::SplineService::TryGet();
        if (!splineService)
            return;

        const SplineECS::SplineHandle sourceHandle(splineComp->SplineDataIndex,
                                                   splineComp->SplineDataGeneration);
        const auto* sourceData = splineService->GetSplineData(sourceHandle);
        if (!sourceData)
            return;

        auto copiedData = *sourceData;
        const SplineECS::SplineHandle createdHandle =
            splineService->CreateSpline(copiedData.Type, copiedData.Closed);

        auto* createdData = splineService->GetSplineData(createdHandle);
        if (!createdData)
            return;

        *createdData = std::move(copiedData);
        createdData->MarkDirty();
        splineService->RebuildCache(createdHandle);

        Components::SplineComponent updated = *splineComp;
        updated.SplineDataIndex = createdHandle.Index();
        updated.SplineDataGeneration = createdHandle.Generation();
        world.AddComponentImmediate(copy, updated);
    }

    // Terrain names a TerrainService slot and the GPU texture set the render
    // feature allocated for it. Neither is reference counted and the copied
    // pair still RESOLVES — it names the source's live terrain — so the copy's
    // own release would free the terrain out from under the source. Zeroed
    // rather than re-created: a zero pair is provisioning's "not provisioned"
    // state, so extraction builds the copy its own terrain from the authoring
    // fields that travelled with the bytes.
    static void RepairTerrain(ECS::World& world, ECS::EntityHandle copy)
    {
        const auto* terrain = world.GetComponent<Components::Terrain>(copy);
        if (!terrain)
            return;

        Components::Terrain updated = *terrain;
        updated.TerrainDataHandle = 0;
        updated.TerrainDataGeneration = 0;
        updated.TiledTerrainHandle = 0;
        updated.TiledTerrainGeneration = 0;
        world.AddComponentImmediate(copy, updated);
    }

    // A non-tiled terrain's heightfield collider sits on the terrain entity
    // itself and keeps its OWN copy of the terrain handle (dataHandle /
    // dataGeneration), plus the build state cooked from that terrain. RepairTerrain
    // sends the copy off to build its own terrain, so a carried-over shape would
    // leave the copy colliding against the SOURCE's heightfield forever: visuals
    // and collision permanently disagreeing.
    //
    // Removed rather than zeroed. TerrainPhysicsSystem provisions this component
    // by PRESENCE and has no re-point path, so a zeroed one would sit there
    // unprovisionable and the copy would get no collision at all. Removing it
    // lets the owning system rebuild the shape from the copy's own terrain, which
    // extraction has re-provisioned by the time TerrainPhysics runs.
    //
    // The removal frees nothing: the component has no removal hook, and the Jolt
    // shape built from it hangs off PhysicsBody, which RepairPhysicsBody clears.
    static void RepairHeightFieldCollider(ECS::World& world, ECS::EntityHandle copy)
    {
        if (!world.GetComponent<Components::HeightFieldColliderShape>(copy))
            return;
        world.RemoveComponentImmediate<Components::HeightFieldColliderShape>(copy);
    }

    // A copy names no body. The handles are the physics world's, built for the
    // entity they were built for, and PhysicsInitSystem re-provisions any enabled
    // entity whose body is missing — so the copy is simulated from the next tick,
    // one frame unsimulated.
    //
    // The reset itself is ClearPhysicsBodyRuntimeState's (PhysicsBody.h), shared
    // with the scene deserializer: a copy and a load both produce an entity that
    // owns nothing yet, and what that looks like is one definition rather than
    // two that can drift.
    static void RepairPhysicsBody(ECS::World& world, ECS::EntityHandle copy)
    {
        const auto* physicsBody = world.GetComponent<Components::PhysicsBody>(copy);
        if (!physicsBody)
            return;

        Components::PhysicsBody updated = *physicsBody;
        Components::ClearPhysicsBodyRuntimeState(updated);
        world.AddComponentImmediate(copy, updated);
    }

    // A copy names no character. CloneEntity copies the source's CharacterHandle
    // bytes, so without this the copy and the original share one motor: writeback
    // poses both from it, and destroying the copy (OnRemove) frees the original's
    // motor. Clear, don't Destroy — Destroy would free the source's object.
    //
    // The reset is ClearCharacterControllerRuntimeState (CharacterController.h),
    // shared with the scene deserializer and delete-undo snapshot scrub.
    static void RepairCharacterController(ECS::World& world, ECS::EntityHandle copy)
    {
        const auto* character = world.GetComponent<Components::CharacterController>(copy);
        if (!character)
            return;

        Components::CharacterController updated = *character;
        Components::ClearCharacterControllerRuntimeState(updated);
        world.AddComponentImmediate(copy, updated);
    }

    // CloneEntity copies graphRuntimeId and eventCollectorId bytes. They name a
    // GraphStore player and an event collector the source holds; OnRemove
    // Destroy frees them. Clear the copy's handles without Destroy so the source
    // keeps both. Next tick the copy instantiates its own player
    // (graphInstanceGuid cleared) and the animation wave gives it its own
    // collector.
    static void RepairAnimatorRuntime(ECS::World& world, ECS::EntityHandle copy)
    {
        const auto* animator = world.GetComponent<Components::Animator>(copy);
        if (!animator)
            return;
        if (animator->graphRuntimeId == 0 && animator->graphInstanceGuid.IsNull()
            && animator->pendingGraphParamCount == 0 && animator->eventCollectorId == 0)
            return;

        Components::Animator updated = *animator;
        updated.graphRuntimeId = 0;
        updated.graphInstanceGuid = GUID{};
        updated.pendingGraphParamCount = 0;
        updated.eventCollectorId = 0;
        world.AddComponentImmediate(copy, updated);
    }

    // Both collider tags name a heightfield physics handle the terrain service
    // allocated for the source tile/face, so the copy is stripped of its claim:
    // zero has the service's handle bit clear, and both release paths return on
    // that bit before touching a slot (TerrainService::ReleaseTilePhysicsHandle).
    //
    // Nothing re-links the zeroed tag — a handle is only ever assigned when
    // TerrainPhysicsSystem creates a collider entity. The copy is simply
    // redundant, and its remaining tile/face keys are byte-identical to the
    // source's, so the system's sweep classifies both the same way and destroys
    // them together the moment that tile stops being resident.
    static void RepairTerrainColliders(ECS::World& world, ECS::EntityHandle copy)
    {
        if (const auto* tile = world.GetComponent<Components::TerrainTileCollider>(copy))
        {
            Components::TerrainTileCollider updated = *tile;
            updated.PhysicsHandleIndex = 0;
            updated.PhysicsHandleGeneration = 0;
            world.AddComponentImmediate(copy, updated);
        }

        if (const auto* face = world.GetComponent<Components::TerrainPlanetFaceCollider>(copy))
        {
            Components::TerrainPlanetFaceCollider updated = *face;
            updated.PhysicsHandleIndex = 0;
            updated.PhysicsHandleGeneration = 0;
            world.AddComponentImmediate(copy, updated);
        }
    }


    // Source runtime id -> the one runtime the copies of that instance share.
    std::unordered_map<std::uint32_t, std::uint32_t> m_SkeletonRuntimeBySource;
    std::unordered_map<std::uint32_t, ECS::EntityHandle> m_CloneBySource;
};
} // namespace GameEngine::Editor
