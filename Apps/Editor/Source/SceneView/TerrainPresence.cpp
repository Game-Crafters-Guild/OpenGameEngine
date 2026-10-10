#include "SceneView/TerrainPresence.h"

#include "Components/Terrain/Terrain.h"
#include "ECS/ECS.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"

#include <utility>

namespace GameEngine::Editor
{

bool TerrainPresence::HasTerrain(ECS::World* world)
{
    if (!world)
        return false;
    const Key key{world, world->GetWorldId(), world->GetLifecycleResetGeneration(),
                  world->GetStructuralChangeVersion()};
    if (m_Valid && key == m_Key)
        return m_HasTerrain;
    // Query::Count walks the matching archetypes, not every entity.
    m_HasTerrain = world->Query<ECS::Read<Components::Terrain>>().Count() != 0;
    m_Key = key;
    m_Valid = true;
    return m_HasTerrain;
}

} // namespace GameEngine::Editor
