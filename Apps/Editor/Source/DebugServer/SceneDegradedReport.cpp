#include "SceneDegradedReport.h"

#include <cstddef>
#include <utility>

namespace GameEngine::Editor
{

nlohmann::json BuildSceneDegradedReport(const std::optional<SceneLoadDegraded>& atLoad,
                                        const std::optional<SceneLoadDegraded>& outstanding)
{
    if (!atLoad.has_value())
        return nlohmann::json();

    nlohmann::json skips = nlohmann::json::array();
    if (outstanding.has_value())
    {
        for (const Scene::SceneLoadSkip& skip : outstanding->Census.skips)
        {
            skips.push_back(nlohmann::json{{"entity", skip.entityId},
                                           {"component", skip.component},
                                           {"field", skip.field},
                                           {"message", skip.message},
                                           {"file", skip.file.string()},
                                           {"line", skip.line},
                                           {"preserved", skip.preserved}});
        }
    }

    return nlohmann::json{
        {"path", atLoad->DocumentPath.string()},
        {"loadTimeCount", atLoad->SkippedCount()},
        {"outstandingCount", outstanding.has_value() ? outstanding->SkippedCount() : std::size_t{0}},
        {"droppedCount", atLoad->DroppedCount()},
        {"skips", std::move(skips)}};
}

} // namespace GameEngine::Editor
