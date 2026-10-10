#include "SplineECS/Systems/SplineExtractionSystem.h"
#include "SplineECS/SplineService.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Transform.h"
#include "ECS/Components.h"
#include "ECS/ECS.h"
#include "ECS/WorldTemplateImplementations.inl"
#include "ECS/ECSTemplates.h"
#include "Spline/SplineEvaluator.h"

#include <vector>

namespace GameEngine::SplineECS
{

void SplineExtractionSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    auto* service = SplineService::TryGet();
    if (!service)
        return;

    // The per-frame scan is a pure READ: a Write<> query stamps every visited
    // chunk's column whether or not anything changed (stamp-at-visit), which
    // would mark every spline chunk dirty on every frame and make Changed<
    // SplineComponent> useless to any consumer. Entities that still need their
    // data slot are collected and written after the scan — empty in steady state.
    std::vector<ECS::EntityHandle> needsSplineData;

    world.Query<ECS::Read<Components::SplineComponent>>()
        .Each([&](ECS::EntityHandle entity, const Components::SplineComponent& comp)
        {
            // Auto-create spline data on first encounter.
            if (comp.SplineDataIndex == 0 && comp.SplineDataGeneration == 0)
            {
                needsSplineData.push_back(entity);
                return;
            }

            // Rebuild cache if dirty.
            const SplineHandle handle(comp.SplineDataIndex, comp.SplineDataGeneration);
            auto* data = service->GetSplineData(handle);
            if (data && data->Dirty && data->IsValid())
                service->RebuildCache(handle);
        });

    for (ECS::EntityHandle entity : needsSplineData)
    {
        auto* comp = world.GetComponentForWrite<Components::SplineComponent>(entity);
        if (!comp)
            continue;
        const SplineHandle handle = service->CreateSpline();
        comp->SplineDataIndex = handle.Index();
        comp->SplineDataGeneration = handle.Generation();
    }
}

} // namespace GameEngine::SplineECS
