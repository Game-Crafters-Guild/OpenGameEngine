#pragma once

#include "Graph/GraphModel.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/ShaderGraph/SgTypes.h"

#include <cstdint>
#include <string>

namespace GameEngine
{

ShaderGraph::SgGraphDocument GraphModelToSgDocument(const Graph::Model& model,
                                                    const std::string& graphName = {});
Graph::Model SgDocumentToGraphModel(const ShaderGraph::SgGraphDocument& doc);
ShaderGraph::SgGraphProperty GraphVariableToSgProperty(const Graph::Variable& variable);
void SyncMaterialParameterNodeSlots(Graph::Model& model);

bool IsMaterialParameterNodeType(const std::string& typeId);
const Graph::Variable* FindGraphVariableByName(const Graph::Model& model, const std::string& name);
void ApplyMaterialGraphVariablesToPreviewDocument(const Graph::Model& model, MaterialDocument& doc);

/** Digest of everything in `model` that can change the GLSL its surface
    materializes to: node identity, resolved types and pin defaults, wiring,
    textures, property declarations, lighting model and variant defines.

    Excluded, and this is the point of the digest: node positions and the
    viewport never reach the shader, and a property's *value* reaches it as a
    uniform — pushed at runtime, no recompile. So a drag, a pan or a value scrub
    leaves the digest still, and only an edit that can change the compiled body
    moves it. */
std::uint64_t MaterialGraphSurfaceDigest(const Graph::Model& model);

} // namespace GameEngine
