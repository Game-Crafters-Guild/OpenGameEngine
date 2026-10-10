#pragma once

#include <string>
#include <vector>

namespace GameEngine {
namespace Graph {

/** One choice of an enum-typed node parameter. Value is the wire string saved
    graphs store and runtimes match on; Label is what editor surfaces display. */
struct NodeParamOption {
    std::string Value;
    std::string Label;
};

using NodeParamOptions = std::vector<NodeParamOption>;

} // namespace Graph
} // namespace GameEngine
