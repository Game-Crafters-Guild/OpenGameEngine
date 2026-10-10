#include "TerrainECS/Systems/TerrainPhysicsSystem.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/PlanetFaceCollider.h"
#include "TerrainECS/Components/TerrainTileCollider.h"
#include "TerrainECS/Components/TerrainPlanetFaceCollider.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainPlanetRelief.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/Transform.h"
#include "ECS/ECS.h"
#include "ECS/WorldTemplateImplementations.inl"
#include "ECS/ECSTemplates.h"
#include "Logger/Logger.h"
#include "PhysicsECS/PhysicsWorldService.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "PhysicsECS/Components/HeightFieldColliderShape.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace GameEngine::TerrainECS
{
namespace
{

// Identity transform (column-major) translated to (x, y, z).
Components::Transform MakeTranslation(float32 x, float32 y, float32 z)
{
    Components::Transform t{}; // defaults to identity
    t.matrix[12] = x;
    t.matrix[13] = y;
    t.matrix[14] = z;
    return t;
}

Components::WorldTransform MakeWorldTranslation(float32 x, float32 y, float32 z)
{
    Components::WorldTransform t{}; // defaults to identity
    t.matrix[12] = x;
    t.matrix[13] = y;
    t.matrix[14] = z;
    return t;
}

// Column-major TRS matrix whose upper-left 3x3 columns are the planet face body frame
// (Ta, N, Tb) and whose translation is the planet centre. Local X runs along Ta, local Z
// along Tb, and local Y (the heightfield height axis) along the face outward normal N.
void FillFaceMatrix(float32 out[16], const std::array<float32, 3>& ta,
                    const std::array<float32, 3>& n, const std::array<float32, 3>& tb,
                    float32 cx, float32 cy, float32 cz)
{
    out[0] = ta[0]; out[1] = ta[1]; out[2] = ta[2]; out[3] = 0.0f;
    out[4] = n[0];  out[5] = n[1];  out[6] = n[2];  out[7] = 0.0f;
    out[8] = tb[0]; out[9] = tb[1]; out[10] = tb[2]; out[11] = 0.0f;
    out[12] = cx;   out[13] = cy;   out[14] = cz;   out[15] = 1.0f;
}

Components::Transform MakeFaceTransform(uint32 face, float32 cx, float32 cy, float32 cz)
{
    std::array<float32, 3> ta, n, tb;
    ComputePlanetFaceFrame(face, ta, n, tb);
    Components::Transform t{};
    FillFaceMatrix(t.matrix, ta, n, tb, cx, cy, cz);
    return t;
}

Components::WorldTransform MakeFaceWorldTransform(uint32 face, float32 cx, float32 cy, float32 cz)
{
    std::array<float32, 3> ta, n, tb;
    ComputePlanetFaceFrame(face, ta, n, tb);
    Components::WorldTransform t{};
    FillFaceMatrix(t.matrix, ta, n, tb, cx, cy, cz);
    return t;
}

// The planet-shape inputs every face patch is derived from, as one comparable value.
// Bit-exact on the float payloads: any change to the shape must be visible, including one
// too small to matter geometrically, since a missed change leaves physics on the old surface.
uint64 HashPlanetShape(const PlanetColliderParams& params)
{
    auto foldFloat = [](uint64 h, float32 f) {
        uint32 bits;
        std::memcpy(&bits, &f, sizeof(bits));
        return h * 1099511628211ull ^ static_cast<uint64>(bits);
    };
    uint64 hash = 1469598103934665603ull; // FNV-1a offset basis
    hash = foldFloat(hash, params.Radius);
    hash = foldFloat(hash, params.ReliefAmplitude);
    hash = foldFloat(hash, params.ReliefFrequency);
    hash = hash * 1099511628211ull ^ static_cast<uint64>(params.ReliefOctaves);
    return hash;
}

// Frees the entity's backend body now. Entity destruction and component removal
// are deferred to the next frame's flush, so the entity stays live for the rest
// of THIS frame with a freed Jolt body; clearing the runtime state keeps anything
// from dereferencing the freed handle before the removal lands (PhysicsInit's
// initialized/valid early-out is incidental, not a guarantee), and the
// OnRemove<PhysicsBody> hook then finds the handles already cleared.
void ReleaseColliderBody(ECS::World& world, ECS::EntityHandle entity)
{
    auto* pw = PhysicsECS::PhysicsWorldService::TryGet();
    auto* body = world.GetComponentForWrite<Components::PhysicsBody>(entity);
    if (!pw || !body || !body->initialized)
        return;
    if (body->body.IsValid())
        pw->DestroyBody(body->body);
    if (body->shape.IsValid())
        pw->DestroyShape(body->shape);
    for (uint16 i = 0; i < body->childShapeCount; ++i)
        if (body->childShapes[i].IsValid())
            pw->DestroyShape(body->childShapes[i]);
    body->body = {};
    body->shape = {};
    for (uint16 i = 0; i < body->childShapeCount; ++i)
        body->childShapes[i] = {};
    body->childShapeCount = 0;
    body->initialized = false;
}

// Points the collider at the terrain's current data and dimensions and clears its
// build cursors: the version cursors force PhysicsInit's rebuild path, and a zero
// built sample count refuses its in-place update, whose baked scale would be stale.
void ResyncTerrainCollider(ECS::World& world, ECS::EntityHandle entity)
{
    const auto* terrain = world.GetComponent<Components::Terrain>(entity);
    auto* shape = world.GetComponentForWrite<Components::HeightFieldColliderShape>(entity);
    if (!terrain || !shape)
        return;
    shape->dataHandle = terrain->TerrainDataHandle;
    shape->dataGeneration = terrain->TerrainDataGeneration;
    shape->sizeX = terrain->SizeX;
    shape->sizeZ = terrain->SizeZ;
    shape->heightScale = terrain->HeightScale;
    shape->lastBuiltVersion = 0;
    shape->lastSeenVersion = 0;
    shape->rebuildWaitSeconds = 0.0f;
    shape->builtSampleCount = 0;
    shape->inPlaceFailedVersion = 0;
    Logger::Log::Info("TerrainPhysicsSystem: terrain {} changed its data or dimensions; rebuilding its collider",
                      entity.id);
}

} // namespace

bool ColliderMatchesTerrain(const Components::HeightFieldColliderShape& shape, const Components::Terrain& terrain)
{
    return shape.dataHandle == terrain.TerrainDataHandle && shape.dataGeneration == terrain.TerrainDataGeneration &&
           shape.sizeX == terrain.SizeX && shape.sizeZ == terrain.SizeZ && shape.heightScale == terrain.HeightScale;
}

void TerrainPhysicsSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    // Only provision components when the physics world exists (play mode).
    if (!PhysicsECS::PhysicsWorldService::IsInitialized())
        return;

    auto* terrainService = TerrainService::TryGet();
    if (!terrainService)
        return;

    if (m_TrackedWorld != world.GetWorldId() || m_TrackedReset != world.GetLifecycleResetGeneration())
    {
        m_PlanarColliders.clear();
        m_TrackedWorld = world.GetWorldId();
        m_TrackedReset = world.GetLifecycleResetGeneration();
    }
    std::vector<ECS::EntityHandle> staleColliders;

    // ---- Single (non-tiled) terrain provisioning ----
    // One HeightFieldColliderShape on the terrain entity itself; PhysicsInitSystem
    // builds the Jolt body via its heightfield gather block, and tears it down when
    // the body's entity is switched off.
    world.Query<ECS::Read<Components::Terrain>, ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle entity,
                  const Components::Terrain& terrain,
                  const Components::WorldTransform& /*wt*/)
        {
            // Spherical (planet) terrains get six per-face colliders (below), not a planar
            // heightfield collider off the SizeX/SizeZ heightfield extraction still builds.
            if (terrain.Domain == Components::TerrainDomain::Spherical)
                return;

            // Tiled terrains are provisioned per-tile below (extraction clears
            // their single TerrainDataHandle, so this block finds no data).
            if (terrain.TiledTerrainHandle != 0 || terrain.TiledTerrainGeneration != 0)
                return;

            TerrainHandle handle{terrain.TerrainDataHandle, terrain.TerrainDataGeneration};
            auto* data = terrainService->GetTerrainData(handle);
            const bool dataReady = data && !data->Heightfield.IsEmpty() && data->HeightfieldVersion != 0;

            // A structural terrain change gives it a new data handle, and an edit can
            // change its extent or height scale; the collider follows once the new
            // data exists.
            if (const auto* shape = world.GetComponent<Components::HeightFieldColliderShape>(entity))
            {
                if (std::find(m_PlanarColliders.begin(), m_PlanarColliders.end(), entity) == m_PlanarColliders.end())
                    m_PlanarColliders.push_back(entity);
                if (dataReady && !ColliderMatchesTerrain(*shape, terrain))
                    staleColliders.push_back(entity);
                return;
            }
            if (!dataReady)
                return;

            // Add physics components (deferred to avoid archetype mutation during query).
            Components::PhysicsBody body{};
            body.motionType = Physics::MotionType::Static;
            body.mass = 0.0f;
            body.gravityScale = 0.0f;
            body.allowSleep = true;
            world.AddComponent(entity, body);

            Components::PhysicsCollider collider{};
            collider.layer = Physics::Layers::Static;
            world.AddComponent(entity, collider);

            Components::HeightFieldColliderShape shape{};
            shape.dataHandle = terrain.TerrainDataHandle;
            shape.dataGeneration = terrain.TerrainDataGeneration;
            shape.sizeX = terrain.SizeX;
            shape.sizeZ = terrain.SizeZ;
            shape.heightScale = terrain.HeightScale;
            world.AddComponent(entity, shape);
            m_PlanarColliders.push_back(entity);
        });

    for (ECS::EntityHandle entity : staleColliders)
        ResyncTerrainCollider(world, entity);
    RemoveOrphanedPlanarColliders(world);

    // ---- Tiled terrain provisioning (E6): one collider entity per Full tile ----

    struct TileKey
    {
        uint32 TiledIndex, TiledGeneration;
        int32 X, Z;
        bool operator==(const TileKey& o) const
        {
            return TiledIndex == o.TiledIndex && TiledGeneration == o.TiledGeneration
                && X == o.X && Z == o.Z;
        }
    };

    // 1. Walk existing tile-collider entities: keep those whose tile is still a
    //    resident Full tile; tear down (destroy + release handle) the orphans
    //    (tile unloaded, dropped below Full, or the tiled terrain is gone).
    //    Every collider entity this system owns is walked whatever its enable
    //    state: one switched off still claims its tile, so the tile is not
    //    provisioned a second collider.
    std::vector<TileKey> live;
    std::vector<ECS::EntityHandle> orphanEntities;
    std::vector<TerrainService::TilePhysicsHandle> orphanHandles;
    world.Query<ECS::Read<Components::TerrainTileCollider>>()
        .IncludeDisabled()
        .Each([&](ECS::EntityHandle e, const Components::TerrainTileCollider& tc)
        {
            bool alive = false;
            TiledTerrainHandle th{tc.TiledIndex, tc.TiledGeneration};
            if (auto* tiled = terrainService->GetTiledTerrainData(th))
            {
                auto it = tiled->Tiles.find(TileCoord{tc.TileX, tc.TileZ});
                if (it != tiled->Tiles.end() && it->second
                    && it->second->LodState == TileLodState::Full
                    && !it->second->Heightfield.IsEmpty())
                    alive = true;
            }

            if (alive)
                live.push_back(TileKey{tc.TiledIndex, tc.TiledGeneration, tc.TileX, tc.TileZ});
            else
            {
                orphanEntities.push_back(e);
                orphanHandles.push_back(
                    TerrainService::TilePhysicsHandle{tc.PhysicsHandleIndex, tc.PhysicsHandleGeneration});
            }
        });

    auto hasCollider = [&](const TileKey& k) {
        for (const auto& l : live)
            if (l == k)
                return true;
        return false;
    };

    // 2. Find Full tiles without a collider entity yet.
    struct PendingTile
    {
        TiledTerrainHandle Tiled;
        TileCoord Coord;
        float32 CenterX, CenterY, CenterZ;
        float32 TileSize, HeightScale;
    };
    std::vector<PendingTile> pending;
    // Tiled handles already provisioned this frame. Duplicating a terrain entity
    // copies the runtime TiledTerrainHandle, so two entities would each enumerate
    // the same tiles and provision double bodies. First entity wins; later ones
    // sharing its handle are skipped.
    std::vector<TiledTerrainHandle> claimedTiled;
    world.Query<ECS::Read<Components::Terrain>, ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle /*entity*/,
                  const Components::Terrain& terrain,
                  const Components::WorldTransform& wt)
        {
            if (terrain.Domain == Components::TerrainDomain::Spherical)
                return; // planets use per-face colliders, not planar tile colliders
            if (terrain.TiledTerrainHandle == 0 && terrain.TiledTerrainGeneration == 0)
                return;

            TiledTerrainHandle th{terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration};
            auto* tiled = terrainService->GetTiledTerrainData(th);
            if (!tiled)
                return;

            if (std::find(claimedTiled.begin(), claimedTiled.end(), th) != claimedTiled.end())
            {
                if (!m_WarnedDuplicateTiledClaim)
                {
                    Logger::Log::Warning(
                        "TerrainPhysicsSystem: multiple terrain entities share tiled handle "
                        "(index={}, gen={}) — provisioning colliders for the first only",
                        th.Index, th.Generation);
                    m_WarnedDuplicateTiledClaim = true;
                }
                return;
            }
            claimedTiled.push_back(th);

            const float32 tileSize = tiled->Config.TileWorldSize;
            const float32 terrainWorldY = wt.matrix[13];
            for (auto& [coord, tilePtr] : tiled->Tiles)
            {
                if (!tilePtr || tilePtr->LodState != TileLodState::Full)
                    continue;
                if (tilePtr->Heightfield.IsEmpty() || tilePtr->HeightfieldVersion == 0)
                    continue;

                const TileKey key{th.Index, th.Generation, coord.X, coord.Z};
                if (hasCollider(key))
                    continue;

                pending.push_back(PendingTile{
                    th, coord,
                    tilePtr->WorldOriginX + tileSize * 0.5f,
                    terrainWorldY,
                    tilePtr->WorldOriginZ + tileSize * 0.5f,
                    tileSize, terrain.HeightScale});
                // Mark provisioned this frame so a second tile map entry (or a
                // duplicate handle that slipped past the claim guard) can't
                // enqueue the same (terrain, tile) twice.
                live.push_back(key);
            }
        });

    // 3. Apply structural changes after the queries. Tear down orphans first so
    //    a re-loaded tile can re-acquire a fresh slot the same frame. The
    //    OnRemove<PhysicsBody> hook would free the body, but DestroyEntity is
    //    deferred to the next frame's flush — so free it here, in the same
    //    window PhysicsInit mutates bodies in and before the step, and let the
    //    hook find the handles already cleared.
    for (ECS::EntityHandle e : orphanEntities)
    {
        ReleaseColliderBody(world, e);
        world.DestroyEntity(e);
    }
    for (const TerrainService::TilePhysicsHandle& handle : orphanHandles)
        terrainService->ReleaseTilePhysicsHandle(handle.Index, handle.Generation);

    for (const PendingTile& p : pending)
    {
        const auto physHandle = terrainService->AcquireTilePhysicsHandle(p.Tiled, p.Coord);

        // CreateEntity allocates the handle immediately; the AddComponent calls
        // are deferred. The engine's per-frame ProcessCommands flush runs at the
        // START of the next frame, before any system (Engine.cpp) — so
        // PhysicsInit sees the fully-formed collider entity on frame N+1, one
        // frame after provisioning, not the same frame. (Tests flush explicitly
        // via world.ProcessCommands().)
        ECS::EntityHandle e = world.CreateEntity();

        world.AddComponent(e, MakeTranslation(p.CenterX, p.CenterY, p.CenterZ));
        world.AddComponent(e, MakeWorldTranslation(p.CenterX, p.CenterY, p.CenterZ));

        Components::PhysicsBody body{};
        body.motionType = Physics::MotionType::Static;
        body.mass = 0.0f;
        body.gravityScale = 0.0f;
        body.allowSleep = true;
        world.AddComponent(e, body);

        Components::PhysicsCollider collider{};
        collider.layer = Physics::Layers::Static;
        world.AddComponent(e, collider);

        Components::HeightFieldColliderShape shape{};
        shape.dataHandle = physHandle.Index;         // includes kTilePhysicsHandleBit
        shape.dataGeneration = physHandle.Generation;
        shape.sizeX = p.TileSize;
        shape.sizeZ = p.TileSize;
        shape.heightScale = p.HeightScale;
        world.AddComponent(e, shape);

        Components::TerrainTileCollider tag{};
        tag.TiledIndex = p.Tiled.Index;
        tag.TiledGeneration = p.Tiled.Generation;
        tag.TileX = p.Coord.X;
        tag.TileZ = p.Coord.Z;
        tag.PhysicsHandleIndex = physHandle.Index;
        tag.PhysicsHandleGeneration = physHandle.Generation;
        world.AddComponent(e, tag);

        // Engine-created runtime state: reconstructed from the tiled terrain on
        // load, so it must never be written to a scene file (SaveSceneToFile
        // excludes RuntimeOnlyEntity-tagged entities). Without this, a
        // save-during-Play would persist N ghost collider entities per save.
        world.AddComponent(e, Components::RuntimeOnlyEntity{});
    }

    // ---- Spherical (planet) provisioning + sculpt-edit refresh ----
    UpdatePlanetColliders(world);
}

void TerrainPhysicsSystem::RemoveOrphanedPlanarColliders(ECS::World& world)
{
    std::erase_if(m_PlanarColliders, [&](ECS::EntityHandle entity)
    {
        if (!world.IsValid(entity))
            return true;
        const auto* terrain = world.GetComponent<Components::Terrain>(entity);
        const bool planarData = terrain && terrain->Domain == Components::TerrainDomain::Planar &&
                                terrain->TiledTerrainHandle == 0 && terrain->TiledTerrainGeneration == 0 &&
                                (terrain->TerrainDataHandle != 0 || terrain->TerrainDataGeneration != 0);
        if (planarData)
            return false; // Still tracked while its provisioned components are in flight.
        if (!world.HasComponent<Components::HeightFieldColliderShape>(entity))
            return true;
        ReleaseColliderBody(world, entity);
        world.RemoveComponent<Components::HeightFieldColliderShape>(entity);
        world.RemoveComponent<Components::PhysicsCollider>(entity);
        world.RemoveComponent<Components::PhysicsBody>(entity);
        Logger::Log::Info("TerrainPhysicsSystem: entity {} no longer has planar terrain data; removed its collider",
                          entity.id);
        return true;
    });
}

void TerrainPhysicsSystem::UpdatePlanetColliders(ECS::World& world)
{
    auto* terrainService = TerrainService::TryGet();
    if (!terrainService)
        return;

    const uint32 dim = kPlanetFaceColliderDim;

    // 1. Walk existing planet-face collider entities. Keep those whose owner is still a
    //    live enabled spherical terrain; tear down (destroy + free body + release handle)
    //    the orphans (terrain destroyed, disabled, or switched back to Planar — the
    //    world-seam audit class). Collect the live faces per owner so provisioning skips
    //    them and the refresh can find each face's physics handle.
    struct LiveFace
    {
        uint32 EntityIndex, EntityVersion, Face, HandleIndex, HandleGeneration;
        ECS::EntityHandle ColliderEntity; // the face collider entity, NOT the owner terrain
    };
    std::vector<LiveFace> liveFaces;
    std::vector<ECS::EntityHandle> orphanEntities;
    std::vector<Components::TerrainPlanetFaceCollider> orphanTags;

    auto ownerIsLivePlanet = [&](const Components::TerrainPlanetFaceCollider& tag) {
        ECS::EntityHandle owner(static_cast<ECS::EntityIndex>(tag.TerrainEntityIndex),
                                static_cast<ECS::EntityVersion>(tag.TerrainEntityVersion));
        if (!world.IsValid(owner))
            return false;
        const ECS::Entity ownerEntity(&world, owner);
        const auto* t = world.GetComponent<Components::Terrain>(owner);
        return t && ownerEntity.IsEnabledInHierarchy() && ownerEntity.IsEnabled<Components::Terrain>() &&
               t->Domain == Components::TerrainDomain::Spherical;
    };

    // Every face collider this system owns is walked whatever its enable state, so
    // one whose planet is gone or switched off is torn down either way.
    world.Query<ECS::Read<Components::TerrainPlanetFaceCollider>>()
        .IncludeDisabled()
        .Each([&](ECS::EntityHandle e, const Components::TerrainPlanetFaceCollider& tag)
        {
            if (ownerIsLivePlanet(tag))
                liveFaces.push_back(LiveFace{tag.TerrainEntityIndex, tag.TerrainEntityVersion,
                                             tag.Face, tag.PhysicsHandleIndex,
                                             tag.PhysicsHandleGeneration, e});
            else
            {
                orphanEntities.push_back(e);
                orphanTags.push_back(tag);
            }
        });

    auto* pw = PhysicsECS::PhysicsWorldService::TryGet();
    for (size_t i = 0; i < orphanEntities.size(); ++i)
    {
        ECS::EntityHandle e = orphanEntities[i];
        if (pw)
        {
            if (auto* body = world.GetComponentForWrite<Components::PhysicsBody>(e);
                body && body->initialized)
            {
                // The OnRemove<PhysicsBody> hook frees the body, but DestroyEntity is
                // deferred to the next frame's flush — free it here (same window
                // PhysicsInit mutates bodies in, before the step) and clear the runtime
                // handles so nothing dereferences the freed body meanwhile.
                if (body->body.IsValid())
                    pw->DestroyBody(body->body);
                if (body->shape.IsValid())
                    pw->DestroyShape(body->shape);
                body->body = {};
                body->shape = {};
                body->initialized = false;
            }
        }
        world.DestroyEntity(e);
        terrainService->ReleasePlanetFacePhysicsHandle(orphanTags[i].PhysicsHandleIndex,
                                                       orphanTags[i].PhysicsHandleGeneration);
    }

    // 2. Find the active planet: the first enabled spherical terrain. Only one planet gets
    //    face colliders (CBT likewise renders terrain 0); a second is warned once.
    ECS::EntityHandle activePlanet{};
    PlanetColliderParams params{};
    float32 centerX = 0.0f, centerY = 0.0f, centerZ = 0.0f;
    bool foundPlanet = false;
    world.Query<ECS::Read<Components::Terrain>, ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle entity, const Components::Terrain& terrain,
                  const Components::WorldTransform& wt)
        {
            if (terrain.Domain != Components::TerrainDomain::Spherical)
                return;
            if (foundPlanet)
            {
                if (!m_WarnedDuplicatePlanetClaim)
                {
                    Logger::Log::Warning(
                        "TerrainPhysicsSystem: multiple spherical terrains — provisioning "
                        "planet colliders for the first only");
                    m_WarnedDuplicatePlanetClaim = true;
                }
                return;
            }
            foundPlanet = true;
            activePlanet = entity;
            params.Radius = terrain.PlanetRadius;
            // Base relief is the companion TerrainPlanetRelief component (default when
            // absent) — the same values CBTUpdateSystem feeds the renderer. Reading them is
            // not sufficient on its own: these params only reach a face patch when something
            // re-cooks it, so an edit to them must cross the shape baseline below.
            Components::TerrainPlanetRelief relief{};
            if (const auto* r = world.GetComponent<Components::TerrainPlanetRelief>(entity))
                relief = *r;
            params.ReliefAmplitude = relief.Amplitude;
            params.ReliefFrequency = relief.Frequency;
            params.ReliefOctaves = relief.Octaves;
            centerX = wt.matrix[12];
            centerY = wt.matrix[13];
            centerZ = wt.matrix[14];
        });

    if (!foundPlanet)
        return; // no planet — nothing to provision or refresh (orphans already torn down)

    // A terrain switched from Planar to Spherical keeps the planar heightfield collider the
    // single-terrain branch put on the entity itself; shed it (and its Jolt body) so a
    // planet never carries a stray planar body under its six face colliders.
    if (world.HasComponent<Components::HeightFieldColliderShape>(activePlanet))
    {
        if (pw)
            if (auto* body = world.GetComponentForWrite<Components::PhysicsBody>(activePlanet);
                body && body->initialized)
            {
                if (body->body.IsValid())
                    pw->DestroyBody(body->body);
                if (body->shape.IsValid())
                    pw->DestroyShape(body->shape);
                body->body = {};
                body->shape = {};
                body->initialized = false;
            }
        world.RemoveComponent<Components::HeightFieldColliderShape>(activePlanet);
        world.RemoveComponent<Components::PhysicsBody>(activePlanet);
        world.RemoveComponent<Components::PhysicsCollider>(activePlanet);
    }

    auto faceIsLive = [&](uint32 face) {
        for (const LiveFace& lf : liveFaces)
            if (lf.EntityIndex == activePlanet.index && lf.EntityVersion == activePlanet.version &&
                lf.Face == face)
                return true;
        return false;
    };

    const float32 sizeMeters = params.Radius * 2.0f; // patch spans the tangent square [-R, R]

    // Did the planet's shape (radius or base relief) change since the last cook? Seeding the
    // baseline on the first sighting must not count as a change: the faces provisioned below
    // are generated at the current shape anyway.
    const uint64 planetShapeHash = HashPlanetShape(params);
    const bool shapeChanged = m_HasPlanetShapeBaseline && planetShapeHash != m_LastPlanetShapeHash;
    m_LastPlanetShapeHash = planetShapeHash;
    m_HasPlanetShapeBaseline = true;

    // Faces already cooked before this frame. Anything provisioned below is appended after
    // this mark and is born at the current shape, so the re-cook must skip it.
    const size_t preExistingFaceCount = liveFaces.size();

    // 3. Provision any missing faces (0-5) for the active planet. Deferred component adds,
    //    like the tiled path; PhysicsInit builds the bodies on the next flush.
    for (uint32 face = 0; face < kPlanetFaceCount; ++face)
    {
        if (faceIsLive(face))
            continue;

        const auto physHandle = terrainService->AcquirePlanetFacePhysicsHandle();

        // Generate the full face patch from the base + relief height field (no sculpt yet
        // when the planet has no edits — the mirror atlas is null then).
        const auto mirror = terrainService->GetPlanetSculptMirror();
        if (auto* data = terrainService->ResolvePlanetFaceColliderForWrite(
                physHandle.Index, physHandle.Generation, dim))
        {
            float32 minH = 0.0f, maxH = 0.0f;
            const CBTTerrain::SphereAnalyticModifierSet analytic{
                mirror.Analytic.data(), mirror.AnalyticCount, mirror.TransientDabs.data(),
                mirror.TransientDabCount};
            GeneratePlanetFacePatch(face, params, dim, mirror.Sampler, analytic, 0, 0,
                                    static_cast<int32>(dim) - 1, static_cast<int32>(dim) - 1,
                                    data->Samples, minH, maxH);
            terrainService->CommitPlanetFaceFull(physHandle.Index, physHandle.Generation);
        }

        ECS::EntityHandle e = world.CreateEntity();
        world.AddComponent(e, MakeFaceTransform(face, centerX, centerY, centerZ));
        world.AddComponent(e, MakeFaceWorldTransform(face, centerX, centerY, centerZ));

        Components::PhysicsBody body{};
        body.motionType = Physics::MotionType::Static;
        body.mass = 0.0f;
        body.gravityScale = 0.0f;
        body.allowSleep = true;
        world.AddComponent(e, body);

        Components::PhysicsCollider collider{};
        collider.layer = Physics::Layers::Static;
        world.AddComponent(e, collider);

        Components::HeightFieldColliderShape shape{};
        shape.dataHandle = physHandle.Index;         // includes kPlanetFacePhysicsHandleBit
        shape.dataGeneration = physHandle.Generation;
        shape.sizeX = sizeMeters;
        shape.sizeZ = sizeMeters;
        shape.heightScale = 1.0f;                    // samples are already metres
        world.AddComponent(e, shape);

        Components::TerrainPlanetFaceCollider tag{};
        tag.TerrainEntityIndex = activePlanet.index;
        tag.TerrainEntityVersion = activePlanet.version;
        tag.Face = face;
        tag.PhysicsHandleIndex = physHandle.Index;
        tag.PhysicsHandleGeneration = physHandle.Generation;
        world.AddComponent(e, tag);

        world.AddComponent(e, Components::RuntimeOnlyEntity{});

        // Provisioned this frame — track so a duplicate query pass can't re-enqueue it, and
        // so the refresh below can address its handle without waiting for the flush.
        liveFaces.push_back(LiveFace{activePlanet.index, activePlanet.version, face,
                                     physHandle.Index, physHandle.Generation, e});

        Logger::Log::Info("Planet.Collider face={} provisioned dim={}", face, dim);
    }

    // 4. Shape-edit re-cook: a radius or base-relief edit displaces the surface at EVERY
    //    direction, so no dirty rect can scope it — the sculpt mirror's per-face rects
    //    describe sculpt strokes and would refresh nothing here. Regenerate all six patches
    //    whole, ahead of the sculpt-version gate below (which returns early on the frames a
    //    shape edit lands, since relief feeds neither of that version's terms), and re-declare
    //    each shape's grid extent alongside the samples it describes. Runs only on the frame
    //    the shape actually moves; idle planets never enter this loop.
    if (shapeChanged)
    {
        const auto shapeMirror = terrainService->GetPlanetSculptMirror();
        const CBTTerrain::SphereAnalyticModifierSet shapeAnalytic{
            shapeMirror.Analytic.data(), shapeMirror.AnalyticCount,
            shapeMirror.TransientDabs.data(), shapeMirror.TransientDabCount};
        const int32 lastCell = static_cast<int32>(dim) - 1;
        for (size_t i = 0; i < preExistingFaceCount; ++i)
        {
            const LiveFace& lf = liveFaces[i];
            if (lf.EntityIndex != activePlanet.index || lf.EntityVersion != activePlanet.version)
                continue; // a second planet's face — only the active planet is provisioned
            auto* data = terrainService->ResolvePlanetFaceColliderForWrite(lf.HandleIndex,
                                                                          lf.HandleGeneration, dim);
            if (!data)
                continue;
            float32 minH = 0.0f, maxH = 0.0f;
            GeneratePlanetFacePatch(lf.Face, params, dim, shapeMirror.Sampler, shapeAnalytic, 0, 0,
                                    lastCell, lastCell, data->Samples, minH, maxH);
            terrainService->CommitPlanetFaceFull(lf.HandleIndex, lf.HandleGeneration);
            // The grid is radius-parametrized — cell i sits at local x = R*u for normalized
            // tangent offset u in [-1, 1], which is exactly the mapping PhysicsInitSystem
            // rebuilds from sizeX/sizeZ — so the declared extent is half of the cook, not a
            // provisioning-time constant. Samples at the new radius under the old extent would
            // stretch the new surface over the old grid, displacing each cell tangentially by
            // (newRadius - oldRadius) * u.
            if (auto* shape = world.GetComponentForWrite<Components::HeightFieldColliderShape>(
                    lf.ColliderEntity))
            {
                shape->sizeX = sizeMeters;
                shape->sizeZ = sizeMeters;
            }
            Logger::Log::Info("Planet.Collider face={} re-cooked (planet shape changed)", lf.Face);
        }
    }

    // 5. Sculpt-edit refresh: when a dab landed, regenerate the touched face patch regions
    //    in place (base + relief + the edited sculpt atlas), which the provider reports to
    //    the E1 two-tier collider path (in-place SetHeights + wake). EVERY touched face is
    //    refreshed, not just the last dab's primary face — an edge-straddling dab writes two
    //    faces' atlas bands, so both colliders must update (the #488 crack-free contract).
    //    Idle planets never enter this loop.
    const auto mirror = terrainService->GetPlanetSculptMirror();
    if (mirror.Version <= m_LastConsumedSculptVersion)
        return;
    m_LastConsumedSculptVersion = mirror.Version;

    for (uint32 face = 0; face < kPlanetFaceCount; ++face)
    {
        const auto& faceRect = mirror.Faces[face];
        if (!faceRect.Touched)
            continue;

        uint32 faceHandleIndex = 0, faceHandleGen = 0;
        bool haveHandle = false;
        for (const LiveFace& lf : liveFaces)
            if (lf.EntityIndex == activePlanet.index &&
                lf.EntityVersion == activePlanet.version && lf.Face == face)
            {
                faceHandleIndex = lf.HandleIndex;
                faceHandleGen = lf.HandleGeneration;
                haveHandle = true;
                break;
            }
        if (!haveHandle)
            continue; // face not provisioned yet — the next full provisioning bakes it in

        int32 minI, minJ, maxI, maxJ;
        PlanetFaceUVRectToGridRect(face, dim, faceRect.MinU, faceRect.MinV, faceRect.MaxU,
                                   faceRect.MaxV, minI, minJ, maxI, maxJ);
        if (minI <= maxI && minJ <= maxJ)
        {
            if (auto* data = terrainService->ResolvePlanetFaceColliderForWrite(faceHandleIndex,
                                                                               faceHandleGen, dim))
            {
                float32 regionMinH = 0.0f, regionMaxH = 0.0f;
                const CBTTerrain::SphereAnalyticModifierSet analytic{
                    mirror.Analytic.data(), mirror.AnalyticCount, mirror.TransientDabs.data(),
                    mirror.TransientDabCount};
                GeneratePlanetFacePatch(face, params, dim, mirror.Sampler, analytic, minI, minJ,
                                        maxI, maxJ, data->Samples, regionMinH, regionMaxH);
                // DirtyRegionLog max bounds are exclusive; the grid rect above is inclusive.
                terrainService->CommitPlanetFaceRegion(faceHandleIndex, faceHandleGen, minI, minJ,
                                                       maxI + 1, maxJ + 1);
                Logger::Log::Info("Planet.Collider face={} refresh rect=({},{},{},{})", face, minI,
                                  minJ, maxI, maxJ);
            }
        }
        terrainService->ClearPlanetSculptDirtyFace(face);
    }
}

} // namespace GameEngine::TerrainECS
