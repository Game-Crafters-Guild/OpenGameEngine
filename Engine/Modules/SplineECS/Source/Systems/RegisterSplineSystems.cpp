#include "SplineECS/Systems/RegisterSplineSystems.h"
#include "SplineECS/Systems/SplineExtractionSystem.h"
#include "ECS/SystemScheduling.h"

namespace GameEngine::SplineECS
{

void AddSplineSystemsToSchedule(ECS::SystemScheduleBuilder& b)
{
    b.Add<SplineExtractionSystem>("SplineExtraction", ECS::SystemPhase::Extraction, 1, {});
}

} // namespace GameEngine::SplineECS
