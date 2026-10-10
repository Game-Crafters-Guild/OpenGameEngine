#include "Ocean/Systems/OceanBuoyancySystem.h"
#include "Ocean/OceanCollisionProvider.h"
#include "Ocean/OceanRenderFeature.h"

#include "Components/Rendering/Ocean.h"
#include "Components/Transform.h"
#include "Engine/Rendering/RenderServices.h"
#include "Physics/PhysicsWorld.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/PhysicsWorldService.h"

#include "ECS/ECS.h"
#include "ECS/Components.h"
#include "ECS/Query.h"

#include <cmath>
#include <vector>
namespace GameEngine::Ocean
{

OceanBuoyancySystem::OceanBuoyancySystem(Engine::Renderer::RenderServices* renderServices)
    : m_RenderServices(renderServices)
{
}

void OceanBuoyancySystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    ++m_Frame;
    if (!m_RenderServices)
        return;
    auto* feature = m_RenderServices->GetFeature<OceanRenderFeature>();
    if (!feature || !feature->HasOcean())
        return;
    auto* physics = PhysicsECS::PhysicsWorldService::TryGet();
    if (!physics)
        return;
    IOceanCollisionProvider& collision = feature->GetCollisionProvider();

    // Four probes at the corners of a square footprint around the body origin.
    static constexpr float32 kProbe[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};

    world.Query<ECS::Read<Components::OceanBuoyancy>, ECS::Read<Components::PhysicsBody>,
                ECS::Read<Components::WorldTransform>>()
        .Each(
            [&](ECS::EntityHandle entity, const Components::OceanBuoyancy& buoy,
                const Components::PhysicsBody& pb, const Components::WorldTransform& wt)
            {
                if (!pb.body.IsValid())
                    return;

                const float32* m = wt.matrix;
                const float32 ox = m[12];
                const float32 oy = m[13];
                const float32 oz = m[14];
                const float32 r = buoy.ProbeRadius;

                // Normalize the rotation columns so ProbeRadius is a world-space
                // footprint (meters), independent of the entity's transform scale —
                // matching Draft/WaterLineOffset, which are already world-space.
                // Column 0 is local +X, column 2 is local +Z; without this an
                // imported model scaled down (e.g. 0.01) collapses the four probes
                // onto a point and loses all self-righting torque.
                const float32 sx = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
                const float32 sz = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
                const float32 invSx = sx > 1e-6f ? 1.0f / sx : 0.0f;
                const float32 invSz = sz > 1e-6f ? 1.0f / sz : 0.0f;
                const float32 r0x = m[0] * invSx, r0y = m[1] * invSx, r0z = m[2] * invSx;
                const float32 r2x = m[8] * invSz, r2y = m[9] * invSz, r2z = m[10] * invSz;

                OceanSurfaceQueryPoint points[4]{};
                float32 probeY[4]{};
                for (uint32 i = 0u; i < 4u; ++i)
                {
                    const float32 lx = kProbe[i][0] * r;
                    const float32 lz = kProbe[i][1] * r;
                    points[i].X = ox + r0x * lx + r2x * lz;
                    points[i].Z = oz + r0z * lx + r2z * lz;
                    probeY[i] = oy + r0y * lx + r2y * lz;
                }

                QueryState& queryState = m_Queries[entity.id];
                queryState.LastSeenFrame = m_Frame;
                OceanSurfaceSample water[4]{};
                OceanCurrentSample current[4]{};
                uint32 resultCount = 0u;
                bool haveProviderResult = queryState.Handle != 0u &&
                    collision.CopyResults(queryState.Handle, water, current, 4u, &resultCount) &&
                    resultCount == 4u;
                if (!haveProviderResult)
                {
                    // Only the first registration normally reaches this path. It
                    // is deterministic Gerstner/available-readback sampling and
                    // never waits for GPU work or reports flat-valid water.
                    feature->SampleSurfaces(points, 4u, water);
                    for (uint32 i = 0u; i < 4u; ++i)
                        current[i] = feature->SampleFlow(points[i].X, points[i].Z);
                }

                OceanCollisionQueryDesc query{};
                query.Owner = static_cast<OceanCollisionOwnerId>(entity.id) + 1u;
                query.MinimumSpatialLength = std::max(r * 0.5f, 0.0f);
                query.Points = points;
                query.Count = 4u;
                query.Fields = OceanQueryField::All;
                queryState.Handle = collision.Submit(query);

                int32 submerged = 0;
                float32 waterVelocity[3] = {};
                for (int32 i = 0; i < 4; ++i)
                {
                    const float32 wx = points[i].X;
                    const float32 wy = probeY[i];
                    const float32 wz = points[i].Z;

                    if (!water[i].Valid)
                        continue;
                    const float32 waterY = water[i].Height;
                    // Measure submergence at the body's draft depth (its underside)
                    // so it floats with freeboard, and cap at twice the draft (fully
                    // submerged) so the restoring force can't run away when it plunges.
                    float32 submergence = (waterY - wy) + buoy.Draft + buoy.WaterLineOffset;
                    submergence = std::min(submergence, 2.0f * buoy.Draft);
                    if (submergence > 0.0f)
                    {
                        ++submerged;
                        waterVelocity[0] += water[i].VelocityWS[0] + current[i].FlowX;
                        waterVelocity[1] += water[i].VelocityWS[1];
                        waterVelocity[2] += water[i].VelocityWS[2] + current[i].FlowZ;
                        const float32 f = pb.mass * buoy.BuoyancyStrength * submergence * 0.25f;
                        physics->AddForceAtPosition(pb.body, Physics::Vector3(0.0f, f, 0.0f),
                                                    Physics::Vector3(wx, wy, wz));
                    }
                }

                if (submerged == 0)
                    return;

                const float32 submergedFrac = static_cast<float32>(submerged) * 0.25f;
                const Physics::Vector3 linVel = physics->GetLinearVelocity(pb.body);
                const Physics::Vector3 angVel = physics->GetAngularVelocity(pb.body);
                const float32 ld = pb.mass * buoy.LinearDrag * submergedFrac;
                const float32 ad = pb.mass * buoy.AngularDrag * submergedFrac;
                const float32 invSubmerged = 1.0f / static_cast<float32>(submerged);
                physics->AddForce(
                    pb.body,
                    Physics::Vector3(-ld * (linVel.x - waterVelocity[0] * invSubmerged),
                                     -ld * (linVel.y - waterVelocity[1] * invSubmerged),
                                     -ld * (linVel.z - waterVelocity[2] * invSubmerged)));
                physics->AddTorque(pb.body,
                                   Physics::Vector3(-ad * angVel.x, -ad * angVel.y, -ad * angVel.z));
            });

    for (auto it = m_Queries.begin(); it != m_Queries.end();)
    {
        if (it->second.LastSeenFrame == m_Frame)
        {
            ++it;
            continue;
        }
        if (it->second.Handle != 0u)
            collision.Cancel(it->second.Handle);
        it = m_Queries.erase(it);
    }
}

} // namespace GameEngine::Ocean
