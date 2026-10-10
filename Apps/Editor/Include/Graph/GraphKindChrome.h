#pragma once

#include "Graph/GraphTypeRegistry.h"

#include <string>
#include <string_view>
#include <vector>

namespace GameEngine {

/* Kind-neutral panel chrome derived from the graph type registry: every
   function answers from GraphTypeDesc registrations, so a new kind gets its
   chrome by registering, not by editing this header. */

/** File-dialog filter for one graph kind ("Shader Graph", "*.glsl", ".glsl"). */
struct GraphKindFileFilter
{
    std::string Name;
    std::string Pattern;
    std::string DefaultExtension;
};

/** Ensures animation (and any other owning-module kinds) are registered, then returns All(). */
std::vector<Graph::GraphTypeDesc> RegisteredGraphTypes();

GraphKindFileFilter SaveFilterForKind(std::string_view kindId);
/** Tab title for a graph with no file yet ("Untitled Shader Graph"). */
std::string UntitledGraphTitle(std::string_view kindId);
/** Open-dialog filter spanning every registered kind's extension. */
std::string OpenGraphsFilterName();
std::string OpenGraphsFilterPattern();
bool ExtensionOpensInGraphPanel(std::string_view extension);
/** KindId encoded by this file extension (registry FileExtension). Empty if none. */
std::string KindIdFromGraphExtension(std::string_view extension);

} // namespace GameEngine

