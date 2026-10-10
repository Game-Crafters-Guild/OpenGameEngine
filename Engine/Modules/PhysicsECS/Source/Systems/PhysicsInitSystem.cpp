#include "PhysicsECS/Systems/PhysicsInitSystem.h"

#include "PhysicsECS/PhysicsWorldService.h"
#include "PhysicsECS/Systems/PhysicsWorldHooks.h"

#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "PhysicsECS/Components/BoxColliderShape.h"
#include "PhysicsECS/Components/CapsuleColliderShape.h"
#include "PhysicsECS/Components/PhysicsColliderOwner.h"
#include "PhysicsECS/Components/PlaneColliderShape.h"
#include "PhysicsECS/Components/SphereColliderShape.h"
#include "PhysicsECS/Components/HeightFieldColliderShape.h"
#include "PhysicsECS/HeightFieldDataProvider.h"

#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "Physics/PhysicsShapes.h"

#include "Logger/Logger.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GameEngine::PhysicsECS
{
namespace
{
static GameEngine::Components::Transform TransformFromWorld(const GameEngine::Components::WorldTransform& wt)
{
    GameEngine::Components::Transform t;
    for (int i = 0; i < 16; ++i)
    {
        t.matrix[i] = wt.matrix[i];
    }
    return t;
}

// A dirty region at most this fraction of the total sample grid takes the
// cheap in-place update (tier 1); anything larger pays the full rebuild.
constexpr float64 kInPlaceRegionMaxFraction = 0.25;

// Padding applied to heightfield wake boxes so bodies resting just outside
// the exact region bounds (or above/below the sampled heights) still wake.
constexpr float32 kWakeBoxPaddingMeters = 2.0f;

// World-space AABB of a sample-space region of a heightfield collider. The region is a
// box in the shape's LOCAL frame — [gridX*scaleX, height*heightScale, gridZ*scaleZ] offset
// by the shape Offset — transformed by the body's full world matrix (rotation + translation
// + any scale) into a world AABB. Transforming the 8 local corners is essential for rotated
// bodies (the planet's per-face heightfields are yawed so their local Y is the face normal);
// for a planar unrotated body the matrix is identity-rotation and this reduces to the old
// axis-aligned mapping.
static Physics::AABB HeightFieldRegionWorldBox(const GameEngine::Components::WorldTransform& wt,
                                               const GameEngine::Components::HeightFieldColliderShape& shape,
                                               uint32 sampleCount,
                                               int32 minX, int32 minZ, int32 maxX, int32 maxZ,
                                               float32 minRawHeight, float32 maxRawHeight)
{
    const float32 scaleX = shape.sizeX / static_cast<float32>(sampleCount - 1);
    const float32 scaleZ = shape.sizeZ / static_cast<float32>(sampleCount - 1);

    // Local-space AABB extents (the shape Offset centres the grid at local origin).
    const float32 lx[2] = {-shape.sizeX * 0.5f + static_cast<float32>(minX) * scaleX,
                           -shape.sizeX * 0.5f + static_cast<float32>(maxX) * scaleX};
    const float32 ly[2] = {minRawHeight * shape.heightScale, maxRawHeight * shape.heightScale};
    const float32 lz[2] = {-shape.sizeZ * 0.5f + static_cast<float32>(minZ) * scaleZ,
                           -shape.sizeZ * 0.5f + static_cast<float32>(maxZ) * scaleZ};

    const float32* m = wt.matrix; // column-major: world = M * (local, 1)
    Physics::Vector3 lo(std::numeric_limits<float32>::max(), std::numeric_limits<float32>::max(),
                        std::numeric_limits<float32>::max());
    Physics::Vector3 hi(std::numeric_limits<float32>::lowest(),
                        std::numeric_limits<float32>::lowest(),
                        std::numeric_limits<float32>::lowest());
    for (float32 x : lx)
        for (float32 y : ly)
            for (float32 z : lz)
            {
                const float32 wx = m[0] * x + m[4] * y + m[8] * z + m[12];
                const float32 wy = m[1] * x + m[5] * y + m[9] * z + m[13];
                const float32 wz = m[2] * x + m[6] * y + m[10] * z + m[14];
                lo.x = std::min(lo.x, wx); lo.y = std::min(lo.y, wy); lo.z = std::min(lo.z, wz);
                hi.x = std::max(hi.x, wx); hi.y = std::max(hi.y, wy); hi.z = std::max(hi.z, wz);
            }

    Physics::AABB box;
    box.min = Physics::Vector3(lo.x - kWakeBoxPaddingMeters, lo.y - kWakeBoxPaddingMeters,
                               lo.z - kWakeBoxPaddingMeters);
    box.max = Physics::Vector3(hi.x + kWakeBoxPaddingMeters, hi.y + kWakeBoxPaddingMeters,
                               hi.z + kWakeBoxPaddingMeters);
    return box;
}

// True while the body owns a backend body or shape. Not the same as initialized:
// an edit that asks for a rebuild clears initialized and leaves the old body
// alive until the rebuild destroys it, so a body switched off in that window
// still has something to give back.
bool HoldsBackendObjects(const GameEngine::Components::PhysicsBody& body)
{
    return body.body.IsValid() || body.shape.IsValid() || body.childShapeCount > 0;
}

} // namespace

void PhysicsInitSystem::ReleaseDisabledBodies(ECS::World& world)
{
    // A missed window (the system was paused while the engine kept swapping)
    // lost its transitions: sweep every body that is off and still holds a backend body.
    if (m_SwapGuard.ConsumeAndCheckMissed(world.GetWorldId(), world.GetLifecycleSwapGeneration()))
    {
        auto q = world.Query<ECS::Write<GameEngine::Components::PhysicsBody>,
                             ECS::Optional<ECS::Disabled>,
                             ECS::Optional<ECS::DisabledInHierarchy>,
                             ECS::Optional<ECS::ComponentDisabled<GameEngine::Components::PhysicsBody>>>();
        q.IncludeDisabled();
        q.Each([](GameEngine::Components::PhysicsBody& body, const ECS::Disabled* disabled,
                  const ECS::DisabledInHierarchy* inactive,
                  const ECS::ComponentDisabled<GameEngine::Components::PhysicsBody>* bodyOff) {
            if (HoldsBackendObjects(body) && (disabled || inactive || bodyOff))
                ReleasePhysicsBody(body);
        });
        return;
    }

    for (ECS::EntityHandle e : world.GetDisabled<GameEngine::Components::PhysicsBody>())
    {
        if (auto* body = world.GetComponentForWrite<GameEngine::Components::PhysicsBody>(e);
            body && HoldsBackendObjects(*body))
            ReleasePhysicsBody(*body);
    }
}

void PhysicsInitSystem::Update(ECS::World& world, float32 deltaTime)
{
    auto* pw = PhysicsWorldService::TryGet();
    if (!pw)
        return;

    ReleaseDisabledBodies(world);

    struct BuiltCollider
    {
        ECS::EntityHandle owner{};              // body entity
        GameEngine::Components::Transform local; // local-to-body
        GameEngine::Components::PhysicsCollider collider{};
        Physics::ShapeDefinition shapeDef{};
    };

    std::unordered_map<ECS::EntityHandle, std::vector<BuiltCollider>, ECS::EntityHandleHash> byBody;
    byBody.reserve(256);

    // Bodies that must be torn down and recreated even though they are still
    // valid — e.g. a heightfield whose source data version changed mid-sim.
    // Empty in steady state.
    std::unordered_set<ECS::EntityHandle, ECS::EntityHandleHash> rebuildBodies;

    auto resolveBodyEntity = [](ECS::EntityHandle colliderEntity,
                                const GameEngine::Components::PhysicsColliderOwner* owner) {
        ECS::EntityHandle bodyEntity = owner ? owner->body : ECS::EntityHandle{};
        if (!bodyEntity.IsValid())
            bodyEntity = colliderEntity; // default: self
        return bodyEntity;
    };

    auto addCollider = [&](ECS::EntityHandle colliderEntity,
                           const GameEngine::Components::PhysicsColliderOwner* owner,
                           const GameEngine::Components::Transform& local,
                           const GameEngine::Components::PhysicsCollider& collider,
                           Physics::ShapeDefinition shapeDef) {
        const ECS::EntityHandle bodyEntity = resolveBodyEntity(colliderEntity, owner);

        BuiltCollider bc{};
        bc.owner = bodyEntity;
        bc.local = local;
        bc.collider = collider;
        bc.shapeDef = std::move(shapeDef);
        byBody[bodyEntity].push_back(std::move(bc));
    };

    // Gather sphere colliders
    {
        auto q = world.Query<ECS::Read<GameEngine::Components::Transform>,
                             ECS::Read<GameEngine::Components::PhysicsCollider>,
                             ECS::Read<GameEngine::Components::SphereColliderShape>,
                             ECS::Optional<GameEngine::Components::PhysicsColliderOwner>>();
        q.Each([&](ECS::EntityHandle e,
                   const GameEngine::Components::Transform& local,
                   const GameEngine::Components::PhysicsCollider& collider,
                   const GameEngine::Components::SphereColliderShape& shape,
                   const GameEngine::Components::PhysicsColliderOwner* owner) {
            addCollider(e, owner, local, collider, Physics::SphereShapeDef{shape.radius});
        });
    }

    // Gather box colliders
    {
        auto q = world.Query<ECS::Read<GameEngine::Components::Transform>,
                             ECS::Read<GameEngine::Components::PhysicsCollider>,
                             ECS::Read<GameEngine::Components::BoxColliderShape>,
                             ECS::Optional<GameEngine::Components::PhysicsColliderOwner>>();
        q.Each([&](ECS::EntityHandle e,
                   const GameEngine::Components::Transform& local,
                   const GameEngine::Components::PhysicsCollider& collider,
                   const GameEngine::Components::BoxColliderShape& shape,
                   const GameEngine::Components::PhysicsColliderOwner* owner) {
            addCollider(e,
                       owner,
                       local,
                       collider,
                       Physics::BoxShapeDef{Physics::Vector3(shape.halfExtentsX, shape.halfExtentsY, shape.halfExtentsZ)});
        });
    }

    // Gather capsule colliders
    {
        auto q = world.Query<ECS::Read<GameEngine::Components::Transform>,
                             ECS::Read<GameEngine::Components::PhysicsCollider>,
                             ECS::Read<GameEngine::Components::CapsuleColliderShape>,
                             ECS::Optional<GameEngine::Components::PhysicsColliderOwner>>();
        q.Each([&](ECS::EntityHandle e,
                   const GameEngine::Components::Transform& local,
                   const GameEngine::Components::PhysicsCollider& collider,
                   const GameEngine::Components::CapsuleColliderShape& shape,
                   const GameEngine::Components::PhysicsColliderOwner* owner) {
            Physics::CapsuleAxis axis = Physics::CapsuleAxis::Y;
            switch (shape.axis)
            {
            case 0:
                axis = Physics::CapsuleAxis::X;
                break;
            case 2:
                axis = Physics::CapsuleAxis::Z;
                break;
            case 1:
            default:
                axis = Physics::CapsuleAxis::Y;
                break;
            }
            addCollider(e, owner, local, collider, Physics::CapsuleShapeDef{shape.radius, shape.halfHeight, axis});
        });
    }

    // Gather plane colliders
    {
        auto q = world.Query<ECS::Read<GameEngine::Components::Transform>,
                             ECS::Read<GameEngine::Components::PhysicsCollider>,
                             ECS::Read<GameEngine::Components::PlaneColliderShape>,
                             ECS::Optional<GameEngine::Components::PhysicsColliderOwner>>();
        q.Each([&](ECS::EntityHandle e,
                   const GameEngine::Components::Transform& local,
                   const GameEngine::Components::PhysicsCollider& collider,
                   const GameEngine::Components::PlaneColliderShape& shape,
                   const GameEngine::Components::PhysicsColliderOwner* owner) {
            addCollider(e,
                       owner,
                       local,
                       collider,
                       Physics::PlaneShapeDef{
                           Physics::Vector3(shape.normalX, shape.normalY, shape.normalZ),
                           shape.d,
                           shape.halfExtent,
                       });
        });
    }

    // Gather heightfield colliders (terrain).
    // Unlike simple shapes, heightfields are expensive to gather (~4MB sample
    // copy), so we skip entities whose body is already valid and up-to-date.
    //
    // On a version mismatch with a built body, two tiers (terrain edit
    // pipeline design §7.3):
    //   1. Small dirty region → in-place backend update, same frame. Live
    //      during drags and Play.
    //   2. Otherwise → full destroy+recreate, throttled on version
    //      quiescence so an edit-drag storm pays one Jolt cook, not one per
    //      frame. While throttled the old collider is stale by design
    //      (bounded by RebuildQuiescenceSeconds).
    // Both tiers end with a dynamic-body wake over the changed area: statics
    // are not island members, so sleeping bodies never notice the ground
    // moving on their own.
    std::vector<Physics::AABB> rebuildWakeBoxes;
    {
        auto provider = GetHeightFieldDataProvider();
        if (provider)
        {
            auto q = world.Query<ECS::Read<GameEngine::Components::Transform>,
                                 ECS::Read<GameEngine::Components::WorldTransform>,
                                 ECS::Read<GameEngine::Components::PhysicsCollider>,
                                 ECS::Write<GameEngine::Components::HeightFieldColliderShape>,
                                 ECS::Read<GameEngine::Components::PhysicsBody>,
                                 ECS::Optional<GameEngine::Components::PhysicsColliderOwner>>();
            // A disabled collider, shape or body is not visited, so its
            // lastBuiltVersion stays where it was and re-enabling it rebuilds
            // from current data rather than resurrecting pre-edit geometry.
            q.Each([&](ECS::EntityHandle e,
                       const GameEngine::Components::Transform& local,
                       const GameEngine::Components::WorldTransform& wt,
                       const GameEngine::Components::PhysicsCollider& collider,
                       GameEngine::Components::HeightFieldColliderShape& shape,
                       const GameEngine::Components::PhysicsBody& body,
                       const GameEngine::Components::PhysicsColliderOwner* owner) {
                HeightFieldData hfData = provider(shape.dataHandle, shape.dataGeneration, shape.lastBuiltVersion);
                if (!hfData.samples || hfData.sampleCount < 4)
                    return;

                // Skip if body is valid and data hasn't changed since last build.
                const bool bodyBuilt = body.initialized && pw->IsBodyValid(body.body) && pw->IsShapeValid(body.shape);
                if (bodyBuilt && shape.lastBuiltVersion == hfData.version)
                    return;

                const uint32 N = hfData.sampleCount;

                if (bodyBuilt)
                {
                    // Tier 1: small dirty region → in-place update. Refused
                    // when the provider grid was resized (region strides no
                    // longer match the shape's baked X/Z scale) or when this
                    // version already failed in place (encode range exceeded
                    // — retrying every throttle frame would re-convert the
                    // region for nothing).
                    if (hfData.hasRegion &&
                        shape.builtSampleCount == N &&
                        shape.inPlaceFailedVersion != hfData.version)
                    {
                        const uint64 regionArea = static_cast<uint64>(hfData.regionMaxX - hfData.regionMinX) *
                                                  static_cast<uint64>(hfData.regionMaxZ - hfData.regionMinZ);
                        const uint64 totalArea = static_cast<uint64>(N) * N;
                        const bool inPlaceEligible = regionArea > 0 &&
                            static_cast<float64>(regionArea) <= kInPlaceRegionMaxFraction * static_cast<float64>(totalArea);
                        const bool inPlaceApplied = inPlaceEligible &&
                            pw->UpdateHeightFieldRegion(body.body, body.shape,
                                                        hfData.samples, N,
                                                        static_cast<uint32>(hfData.regionMinX),
                                                        static_cast<uint32>(hfData.regionMinZ),
                                                        static_cast<uint32>(hfData.regionMaxX - hfData.regionMinX),
                                                        static_cast<uint32>(hfData.regionMaxZ - hfData.regionMinZ));
                        if (inPlaceEligible && !inPlaceApplied)
                            shape.inPlaceFailedVersion = hfData.version;
                        if (inPlaceApplied)
                        {
                            shape.lastBuiltVersion = hfData.version;
                            shape.lastSeenVersion = hfData.version;
                            shape.rebuildWaitSeconds = 0.0f;

                            // Wake sleeping bodies over the changed region.
                            // Y spans the built-height envelope, not just the
                            // new samples: after a lower, sleepers rest at
                            // the OLD surface height.
                            float32 regionMin = std::numeric_limits<float32>::max();
                            float32 regionMax = std::numeric_limits<float32>::lowest();
                            for (int32 rz = hfData.regionMinZ; rz < hfData.regionMaxZ; ++rz)
                                for (int32 rx = hfData.regionMinX; rx < hfData.regionMaxX; ++rx)
                                {
                                    const float32 s = hfData.samples[static_cast<size_t>(rz) * N + rx];
                                    regionMin = std::min(regionMin, s);
                                    regionMax = std::max(regionMax, s);
                                }
                            const float32 wakeMin = std::min(regionMin, shape.builtMinHeight);
                            const float32 wakeMax = std::max(regionMax, shape.builtMaxHeight);
                            shape.builtMinHeight = wakeMin;
                            shape.builtMaxHeight = wakeMax;

                            pw->ActivateBodiesInAABB(HeightFieldRegionWorldBox(
                                wt, shape, N,
                                hfData.regionMinX, hfData.regionMinZ,
                                hfData.regionMaxX, hfData.regionMaxZ,
                                wakeMin, wakeMax));
                            return;
                        }
                    }

                    // Tier 2: full rebuild, throttled on version quiescence.
                    // lastBuiltVersion is NOT advanced while throttled, so the
                    // gather retries every frame until the window elapses.
                    if (hfData.version != shape.lastSeenVersion)
                    {
                        shape.lastSeenVersion = hfData.version;
                        shape.rebuildWaitSeconds = 0.0f;
                    }
                    else
                    {
                        shape.rebuildWaitSeconds += deltaTime;
                    }
                    if (shape.rebuildWaitSeconds < m_RebuildQuiescenceSeconds)
                        return;
                }

                const float32 scaleX = shape.sizeX / static_cast<float32>(N - 1);
                const float32 scaleZ = shape.sizeZ / static_cast<float32>(N - 1);

                Physics::HeightFieldShapeDef hfDef;
                hfDef.Samples.assign(hfData.samples, hfData.samples + static_cast<size_t>(N) * N);
                hfDef.SampleCount = N;
                hfDef.Offset = Physics::Vector3(-shape.sizeX * 0.5f, 0.0f, -shape.sizeZ * 0.5f);
                hfDef.Scale = Physics::Vector3(scaleX, shape.heightScale, scaleZ);

                float32 fullMin = std::numeric_limits<float32>::max();
                float32 fullMax = std::numeric_limits<float32>::lowest();
                for (const float32 s : hfDef.Samples)
                {
                    fullMin = std::min(fullMin, s);
                    fullMax = std::max(fullMax, s);
                }

                addCollider(e, owner, local, collider, std::move(hfDef));
                shape.lastBuiltVersion = hfData.version;
                shape.lastSeenVersion = hfData.version;
                shape.rebuildWaitSeconds = 0.0f;
                shape.builtSampleCount = N;
                shape.inPlaceFailedVersion = 0;

                // A version change on a still-valid body means a terrain edit:
                // the build block below early-outs on valid bodies, which would
                // discard the samples we just gathered and (because
                // lastBuiltVersion advanced) never retry. Force a teardown so
                // the normal creation path consumes them this frame.
                if (bodyBuilt)
                {
                    rebuildBodies.insert(resolveBodyEntity(e, owner));
                    // Wake over the whole footprint spanning old and new
                    // heights — the rebuild replaces all geometry.
                    rebuildWakeBoxes.push_back(HeightFieldRegionWorldBox(
                        wt, shape, N,
                        0, 0, static_cast<int32>(N - 1), static_cast<int32>(N - 1),
                        std::min(fullMin, shape.builtMinHeight),
                        std::max(fullMax, shape.builtMaxHeight)));
                }

                shape.builtMinHeight = fullMin;
                shape.builtMaxHeight = fullMax;
            });
        }
    }

    // Build bodies from gathered colliders.
    {
        // A disabled body is not visited: ReleaseDisabledBodies tore its backend
        // body down, and re-enabling it brings it back here like any new one.
        auto q = world.Query<ECS::Read<GameEngine::Components::WorldTransform>, ECS::Write<GameEngine::Components::PhysicsBody>>();
        q.Each([&](ECS::EntityHandle e,
                   const GameEngine::Components::WorldTransform& wt,
                   GameEngine::Components::PhysicsBody& body) {
            auto it = byBody.find(e);
            if (it == byBody.end() || it->second.empty())
                return;

            // Recreate if missing/invalid, or if a gathered source (heightfield
            // version bump) demands a rebuild of a still-valid body.
            //
            // Constraint: a forced rebuild recreates the body from byBody,
            // which only contains colliders gathered THIS frame — and the
            // heightfield gather skips up-to-date colliders. If a body ever
            // owned multiple heightfield colliders (today's TerrainPhysics
            // provisioning is strictly 1:1), the unchanged siblings would be
            // missing here and silently dropped from the rebuilt body.
            const bool haveValidBody = body.initialized && pw->IsBodyValid(body.body);
            const bool haveValidShape = body.initialized && pw->IsShapeValid(body.shape);
            const bool forceRebuild = !rebuildBodies.empty() && rebuildBodies.contains(e);
            if (haveValidBody && haveValidShape && !forceRebuild)
                return;

            // Destroy old body + shape(s)
            if (body.body.IsValid())
            {
                pw->DestroyBody(body.body);
                body.body = {};
            }
            if (body.shape.IsValid())
            {
                pw->DestroyShape(body.shape);
                body.shape = {};
            }
            for (uint16 i = 0; i < body.childShapeCount; ++i)
            {
                if (body.childShapes[i].IsValid())
                    pw->DestroyShape(body.childShapes[i]);
                body.childShapes[i] = {};
            }
            body.childShapeCount = 0;

            // Extract body pose + scale from world transform.
            const auto wtAsTransform = TransformFromWorld(wt);
            const auto pos = wtAsTransform.GetPosition();
            const auto rot = wtAsTransform.GetRotation();
            const auto scl = wtAsTransform.GetScale();
            const Physics::Vector3 bodyScale{scl.x, scl.y, scl.z};

            // Reduce collider state to body-level settings.
            // For now, Jolt applies these at the body level.
            Physics::CollisionLayer effectiveLayer = it->second[0].collider.layer;
            Physics::CollisionGroup effectiveGroup = it->second[0].collider.collisionGroup;

            bool anyTrigger = false;
            bool anySolid = false;
            float32 effectiveFriction = body.material.friction;
            float32 effectiveRestitution = body.material.restitution;

            for (const auto& c : it->second)
            {
                if (c.collider.layer != effectiveLayer)
                {
                    Logger::Log::Warning(
                        "[PhysicsECS] Body entity {} has colliders with mismatched layers ({} vs {}); using first",
                        e.id,
                        (int)effectiveLayer,
                        (int)c.collider.layer);
                }
                if (c.collider.collisionGroup.groupId != effectiveGroup.groupId || c.collider.collisionGroup.subGroupId != effectiveGroup.subGroupId)
                {
                    Logger::Log::Warning(
                        "[PhysicsECS] Body entity {} has colliders with mismatched collision groups; using first",
                        e.id);
                }

                anyTrigger = anyTrigger || c.collider.isTrigger;
                anySolid = anySolid || !c.collider.isTrigger;

                const Physics::PhysicsMaterial mat = c.collider.overrideMaterial ? c.collider.material : body.material;
                effectiveFriction = std::max(effectiveFriction, mat.friction);
                effectiveRestitution = std::max(effectiveRestitution, mat.restitution);
            }

            const bool sensor = anyTrigger && !anySolid;
            if (anyTrigger && anySolid)
            {
                Logger::Log::Warning(
                    "[PhysicsECS] Body entity {} mixes trigger and solid colliders; treating body as solid (Jolt is body-level for sensors).",
                    e.id);
            }

            Physics::ShapeHandle rootShape{};

            if (it->second.size() == 1)
            {
                // Single collider: bake body scale into the shape directly.
                rootShape = pw->CreateShape(it->second[0].shapeDef, bodyScale);
            }
            else
            {
                // Multiple colliders: build a compound.
                Physics::CompoundShapeDef comp{};
                comp.children.reserve(it->second.size());

                for (const auto& c : it->second)
                {
                    if (body.childShapeCount >= GameEngine::Components::PhysicsBody::kMaxChildShapes)
                        break;

                    // Child local TRS (relative to body)
                    const auto childPos = c.local.GetPosition();
                    const auto childRot = c.local.GetRotation();
                    const auto childScl = c.local.GetScale();

                    // Parent (body) scale affects child translation in the transform hierarchy.
                    const Physics::Vector3 childOffset(childPos.x * scl.x, childPos.y * scl.y, childPos.z * scl.z);

                    // Bake both body scale and child local scale into the child shape.
                    const Physics::Vector3 childBakedScale(bodyScale.x * childScl.x, bodyScale.y * childScl.y, bodyScale.z * childScl.z);

                    const Physics::ShapeHandle childShape = pw->CreateShape(c.shapeDef, childBakedScale);
                    if (!childShape.IsValid())
                        continue;

                    body.childShapes[body.childShapeCount++] = childShape;

                    Physics::CompoundShapeChild cc{};
                    cc.shape = childShape;
                    cc.position = childOffset;
                    { const auto& q = childRot.GetGLM(); cc.rotation = Physics::Quaternion(q.w, q.x, q.y, q.z); }
                    cc.bakedScale = Physics::Vector3(1.0f, 1.0f, 1.0f); // already baked
                    comp.children.push_back(cc);
                }

                rootShape = pw->CreateShape(comp, Physics::Vector3(1.0f, 1.0f, 1.0f));
            }

            if (!rootShape.IsValid())
            {
                body.initialized = false;
                return;
            }

            body.shape = rootShape;

            Physics::BodySettings bs{};
            bs.position = Physics::Vector3(pos.x, pos.y, pos.z);
            { const auto& q = rot.GetGLM(); bs.rotation = Physics::Quaternion(q.w, q.x, q.y, q.z); }
            bs.shape = body.shape;

            bs.motionType = body.motionType;
            bs.layer = effectiveLayer;
            bs.collisionGroup = effectiveGroup;

            bs.mass = body.mass;
            bs.linearDamping = body.linearDamping;
            bs.angularDamping = body.angularDamping;
            bs.gravityScale = body.gravityScale;
            bs.centerOfMassOffset = Physics::Vector3(body.centerOfMassOffsetX, body.centerOfMassOffsetY, body.centerOfMassOffsetZ);
            bs.friction = effectiveFriction;
            bs.restitution = effectiveRestitution;

            bs.linearVelocity = Physics::Vector3(body.linearVelocityX, body.linearVelocityY, body.linearVelocityZ);
            bs.angularVelocity = Physics::Vector3(body.angularVelocityX, body.angularVelocityY, body.angularVelocityZ);

            bs.isSensor = sensor;
            bs.allowSleep = body.allowSleep;
            bs.startAwake = body.startAwake;
            bs.continuousCollision = body.continuousCollision;
            // PhysicsECS reserves backend userData for ECS entity mapping.
            // If game code needs custom IDs/tags, keep them in ECS components and resolve during event handling.
            bs.userData = static_cast<uint64>(e.id);

            body.body = pw->CreateBody(bs);
            body.initialized = body.body.IsValid();
        });
    }

    // Wake sleeping dynamics over every heightfield footprint that was
    // rebuilt this frame (the bodies now exist again, so the sweep sticks).
    for (const auto& box : rebuildWakeBoxes)
        pw->ActivateBodiesInAABB(box);
}

} // namespace GameEngine::PhysicsECS
