#pragma once

#include "Graph/GraphModel.h"
#include "Graph/NodeParamOption.h"

#include <string>
#include <vector>

namespace GameEngine
{

struct GameLogicActionParameter
{
    std::string Id;
    std::string DefaultValue;
    /** Non-empty = enum: every editor surface offers exactly these choices,
        matching the value vocabulary the runtime accepts. */
    Graph::NodeParamOptions Options;
};

struct GameLogicActionSpec
{
    std::string TypeId;
    std::string DisplayName;
    std::string Category;
    std::vector<Graph::Port> Ports;
    std::vector<GameLogicActionParameter> Parameters;
    bool Precompiled = true;
};

const std::vector<GameLogicActionSpec>& GetGameLogicActionCatalog();
const GameLogicActionSpec* FindGameLogicActionSpec(const std::string& typeId);

} // namespace GameEngine
