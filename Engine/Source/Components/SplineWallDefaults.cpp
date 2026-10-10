// A swept wall's defaults derived from the spline it is added to.
//
// A corner is something the author already says with the spline's type: a
// wall on a spline of straight segments starts with mitred corners and one on a
// curved spline with round ones, so a wall drawn the obvious way needs no touch.
// The component factory applies this whenever it adds a SplineWall; a scene load
// applies the saved Corner over it. The inspector's Reset restores the values the
// component had when the inspector first showed it, not these.

#include "Components/Spline/SplineComponent.h"
#include "Components/Spline/SplineWall.h"
#include "ECS/ComponentFactory.h"
#include "ECS/ECSTemplates.h"
#include "Spline/SplineData.h"
#include "SplineECS/SplineService.h"

#include <cstring>

namespace GameEngine::Components
{
namespace
{

void DeriveWallDefaults(const ECS::World& world, ECS::EntityHandle entity,
                        std::span<std::uint8_t> bytes)
{
    if (bytes.size() != sizeof(SplineWall))
        return;
    const auto* spline = world.GetComponent<SplineComponent>(entity);
    const auto* service = SplineECS::SplineService::TryGet();
    if (!spline || !service)
        return;
    const Spline::SplineData* data = service->GetSplineData(
        SplineECS::SplineHandle{spline->SplineDataIndex, spline->SplineDataGeneration});
    if (!data)
        return;

    SplineWall wall;
    std::memcpy(&wall, bytes.data(), sizeof(SplineWall));
    wall.Corner = data->Type == Spline::SplineType::Linear ? SplineWallCorner::Mitre
                                                           : SplineWallCorner::Round;
    std::memcpy(bytes.data(), &wall, sizeof(SplineWall));
}

[[maybe_unused]] const bool kWallDefaultsRegistered = []
{
    ECS::ComponentFactory::RegisterDerivedDefaults(ECS::GetComponentTypeId<SplineWall>(),
                                                   &DeriveWallDefaults);
    return true;
}();

} // namespace
} // namespace GameEngine::Components
