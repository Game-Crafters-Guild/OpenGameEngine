#pragma once

#include "Graph/GraphModel.h"

#include <string>
#include <vector>

namespace GameEngine
{

struct GameLogicGraphCompileResult
{
    bool success = false;
    std::string cppSource;
    std::vector<std::string> errors;
};

class GameLogicGraphCompiler
{
public:
    static GameLogicGraphCompileResult CompileToCpp(const Graph::Model& model,
                                                    const std::string& functionName = "ExecuteGameLogicGraph");
};

} // namespace GameEngine
