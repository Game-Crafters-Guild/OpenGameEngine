#pragma once

#include "Graph/GraphModel.h"

#include <string>

namespace GameEngine {

/**
 * What a shader graph means by a "variable" node.
 *
 * Material vocabulary, so it lives with the material graph rather than in the
 * generic panel: a parameter node names a graph variable, and its type follows
 * from which parameter node it is.
 */
namespace MaterialGraphVariables {

/** A node that names a graph variable rather than carrying its own value. */
bool IsVariableNode(const Graph::Node& node);

/** The variable type a parameter node declares. */
std::string TypeFromNode(const Graph::Node& node);

/** A freshly declared variable's value, as text in the wire format. */
std::string DefaultValueForType(const std::string& type);

} // namespace MaterialGraphVariables
} // namespace GameEngine
