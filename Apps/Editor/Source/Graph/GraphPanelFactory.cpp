#include "Graph/GraphPanelFactory.h"

#include "Logger/Logger.h"
#include "Panels/AnimationGraphPanel.h"
#include "Panels/GameLogicGraphPanel.h"
#include "Panels/GraphPanel.h"
#include "Panels/ShaderGraphPanel.h"

#include <string>

namespace GameEngine {

std::unique_ptr<GraphPanel> CreateGraphPanelForKind(std::string_view kindId)
{
    const std::string id = kindId.empty() ? std::string(Graph::kKindIdMaterial) : std::string(kindId);
    if (id == Graph::kKindIdMaterial)
        return std::make_unique<ShaderGraphPanel>();
    if (id == Graph::kKindIdAnimation)
        return std::make_unique<AnimationGraphPanel>();
    if (id == Graph::kKindIdGameLogic)
        return std::make_unique<GameLogicGraphPanel>();

    Logger::Log::Error("CreateGraphPanelForKind: unknown kind '{}'", id);
    return nullptr;
}

std::unique_ptr<GraphPanel> GraphPanel::CreateForKind(std::string_view kindId)
{
    return CreateGraphPanelForKind(kindId);
}

} // namespace GameEngine
