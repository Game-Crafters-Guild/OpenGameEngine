#include "Graph/GraphVariableValues.h"

#include "Graph/GraphModel.h"

#include <cstdio>
#include <cstdlib>

namespace GameEngine {

Graph::Variable* FindNodeVariable(Graph::Model* model, const std::string& nodeId)
{
    if (!model)
        return nullptr;
    const Graph::Node* node = model->FindNode(nodeId);
    if (!node)
        return nullptr;
    const std::string name = node->Parameters.GetString("variableName", "");
    if (name.empty())
        return nullptr;
    for (Graph::Variable& variable : model->Variables)
    {
        if (variable.Name == name)
            return &variable;
    }
    return nullptr;
}

void ParseVariableFloats(const std::string& value, float* out, int count)
{
    const char* cursor = value.c_str();
    for (int i = 0; i < count; ++i)
    {
        char* end = nullptr;
        out[i] = std::strtof(cursor, &end);
        cursor = end;
        while (*cursor == ',' || *cursor == ' ')
            ++cursor;
    }
}

std::string FormatVariableFloats(const float* values, int count)
{
    std::string out;
    char buf[32];
    for (int i = 0; i < count; ++i)
    {
        std::snprintf(buf, sizeof(buf), "%.6g", values[i]);
        if (i > 0)
            out += ", ";
        out += buf;
    }
    return out;
}

} // namespace GameEngine
