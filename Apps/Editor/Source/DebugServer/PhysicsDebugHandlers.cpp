#include "DebugServer/PhysicsDebugHandlers.h"

#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"
#include "DebugServer/PhysicsRaycastRequest.h"

#include "Components/Name.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "PhysicsECS/PhysicsWorldService.h"
#include "Physics/PhysicsQuery.h"
#include "Physics/PhysicsWorld.h"

#include <nlohmann/json.hpp>

#include <string>

namespace GameEngine
{

using json = nlohmann::json;

void RegisterPhysicsDebugHandlers(EditorDebugServer& server)
{
    // physics_raycast — cast a ray against the live physics world and report the nearest
    // hit. The collision oracle for automation: a terrain edit that renders but whose
    // heightfield collider was not refreshed is invisible to every rendering check, and
    // this is the only query that can tell them apart. Read-only; it runs against the
    // world exactly as it stands.
    //
    // The physics world only exists while play mode is active: the editor gates the
    // PhysicsECS systems off outside play and shuts the world down on exit, so in edit mode
    // this answers "No physics world" rather than a miss. A caller enters play mode, lets
    // PhysicsInitSystem build the shapes, probes, and stops.
    //
    // Params:
    //   origin      [x,y,z] world position, finite (required)
    //   direction   [x,y,z] ray direction, finite, need not be unit (required)
    //   maxDistance search cap in metres (default 1000, must be finite and > 0)
    //   layerMask   collision-layer bitmask (default all layers)
    server.RegisterHandler("physics_raycast", [](const EditorDebugServer::RequestContext& ctx) -> json
    {
        auto* pw = PhysicsECS::PhysicsWorldService::TryGet();
        if (!pw)
            return Editor::RefuseRequest("No physics world — PhysicsWorldService is not initialized");

        std::string err;
        Physics::RayCastQuery query{};
        if (!Editor::ReadRayCastQuery(ctx.params, query, err))
            return Editor::RefuseRequest(err);

        Physics::RayCastResult result{};
        if (!pw->RayCast(query, result) || !result.HasHit())
            return json{{"hit", false},
                        {"maxDistance", query.maxDistance},
                        {"note", "no body along the ray — an unbuilt collider looks identical to "
                                 "empty space here, so check the entity carries a collider shape"}};

        // PhysicsECS stores the owning ECS entity id in the backend body's userData.
        const auto entityId = static_cast<uint32_t>(result.userData);
        const ECS::EntityHandle entity(entityId);
        std::string name;
        if (auto* world = EngineCore::GetInstance().GetPrimaryWorld(); world && world->IsValid(entity))
        {
            if (const auto* n = world->GetComponent<Components::Name>(entity))
                name = std::string(n->View());
        }

        return json{
            {"hit", true},
            {"entityId", entityId},
            {"entityName", name},
            {"point", {result.hitPoint.x, result.hitPoint.y, result.hitPoint.z}},
            {"normal", {result.hitNormal.x, result.hitNormal.y, result.hitNormal.z}},
            {"distance", result.distance}
        };
    });
}

} // namespace GameEngine
