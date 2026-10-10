#pragma once

#include <string>

namespace GameEngine {

namespace Graph {
struct Model;
struct Variable;
}

/** The variable a node addresses through its variableName parameter, or null
    (no model, unknown node, empty name, or no variable of that name). */
Graph::Variable* FindNodeVariable(Graph::Model* model, const std::string& nodeId);

/* Variable values are the comma-separated float lists the Variables board
   writes ("r, g, b"). Missing components parse as 0. */
void ParseVariableFloats(const std::string& value, float* out, int count);
std::string FormatVariableFloats(const float* values, int count);

} // namespace GameEngine
