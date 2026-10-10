#include "SceneView/SplineOwnerQuery.h"

#include <utility>

namespace GameEngine::Editor
{

namespace
{
SplineOwnerQuery& OwnerQuery()
{
    static SplineOwnerQuery query;
    return query;
}
} // namespace

void SetSplineOwnerQuery(SplineOwnerQuery query)
{
    OwnerQuery() = std::move(query);
}

SplineOwnerClaim QuerySplineOwner(const ECS::World& world, ECS::EntityHandle entity)
{
    const SplineOwnerQuery& query = OwnerQuery();
    return query ? query(world, entity) : SplineOwnerClaim{};
}

} // namespace GameEngine::Editor
