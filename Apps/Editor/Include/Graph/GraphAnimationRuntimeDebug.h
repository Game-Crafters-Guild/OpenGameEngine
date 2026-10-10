#pragma once

#include "Animation/AnimationGraphPlayer.h"
#include "Animation/Nodes/StateMachineNode.h"
#include "Graph/GraphModel.h"

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace GameEngine {

struct GraphAnimationRuntimeHighlight
{
    std::unordered_set<std::string> NodeIds;
    std::vector<std::string> LinkIds;
};

class GraphAnimationRuntimeDebug
{
public:
    static std::string FindStateNodeId(const Graph::Model& nested, std::string_view stateName);
    static GraphAnimationRuntimeHighlight ForStateMachine(const Graph::Model& nested,
                                                          const Animation::StateMachineNode& sm);
    static bool StateIsEvaluating(const Graph::Model& stateMachineNested, std::string_view stateNodeId,
                                  const Animation::StateMachineNode& sm);
    static GraphAnimationRuntimeHighlight ForPoseGraph(const Graph::Model& nested);
    static bool TryPreviewFloat(const Animation::AnimationGraphPlayer& player, std::string_view name,
                                float& out);
};

} // namespace GameEngine
