#pragma once

#include "Graph/GraphModel.h"

#include <optional>
#include <string>
#include <string_view>

namespace GameEngine {
namespace Graph {

inline constexpr std::string_view kGlslJsonFenceBegin = "GE-GRAPH-JSON-BEGIN";
inline constexpr std::string_view kGlslJsonFenceEnd = "GE-GRAPH-JSON-END";

/** Pull the authoring JSON from leading `//` comments, if the fence is present.
 *  A begun fence (empty, unclosed, or closed) returns an engaged optional so
 *  the loader can fail closed instead of falling through to `@sg-*` tags. */
std::optional<std::string> ExtractAuthoringJson(std::string_view comments);

/** Append a canonical JSON fence after existing `//` tags. */
std::string AppendAuthoringJsonFence(std::string tagBlock, const std::string& json);

/** Load a material graph from a .glsl comment block. A JSON fence, if present,
 *  is the only source — invalid JSON fails closed. `@sg-*` tags are fallback
 *  only when no fence appears. */
bool LoadModelFromShaderGraphComments(std::string_view tagBlock, Model& outModel);

} // namespace Graph
} // namespace GameEngine
