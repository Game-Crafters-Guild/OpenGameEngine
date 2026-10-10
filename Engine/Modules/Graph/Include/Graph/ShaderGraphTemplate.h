#pragma once

// ShaderGraphTemplate: the starting point behind the editor's
// Create → Shader Graph action. A new shader graph ships as a PAIR — a .glsl
// carrying the authoring model with the Surface Output master node already
// placed (so it compiles and renders the moment it exists) plus a companion
// .material pre-wired to it. Lives beside the model/compile plumbing so the
// template and the format it produces are tested against the real pipeline.

#include <string>

namespace GameEngine
{
struct MaterialDocument;

namespace Graph
{

// Shader-graph .glsl text for a new graph saved as <stem>.glsl: tag block,
// authoring JSON fence, and the compiled EvaluateSurface body.
std::string MakeShaderGraphTemplateSource(const std::string& stem);

// Companion material document referencing <stem>.glsl (material-dir-first
// resolution). surfaceShaderGuid is left empty — the creating editor fills it
// once the graph file is registered with the asset system.
MaterialDocument MakeShaderGraphTemplateMaterial(const std::string& stem);

} // namespace Graph
} // namespace GameEngine
