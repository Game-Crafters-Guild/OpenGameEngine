#include "EZTreeECS/Systems/RegisterEZTreeSystems.h"

#include "ECS/SystemScheduling.h"
#include "EZTreeECS/Systems/EZTreeExtractionSystem.h"

namespace GameEngine::EZTreeECS
{

void AddEZTreeSystemsToSchedule(ECS::SystemScheduleBuilder& builder,
                                Engine::Renderer::RenderServices* renderServices)
{
    // Extraction reads WorldTransform (written by TransformHierarchy) and the
    // RenderServices view list (written by Camera, whose first tick appends to
    // the view vector). Systems inside one wave run concurrently on job workers
    // and ViewRegistry carries no synchronization of its own, so both orderings
    // have to be declared edges rather than a by-product of whatever else
    // happens to push the systems apart — the same pair OceanExtraction and
    // LensFlareExtraction declare for the same two reads.
    builder.Add<EZTreeExtractionSystem>("EZTreeExtraction",
                                        ECS::SystemPhase::Extraction,
                                        2,
                                        {"TransformHierarchy", "Camera"},
                                        renderServices);
}

} // namespace GameEngine::EZTreeECS
