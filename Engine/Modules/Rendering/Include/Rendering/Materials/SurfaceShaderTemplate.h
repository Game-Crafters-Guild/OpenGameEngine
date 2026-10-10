#pragma once

// SurfaceShaderTemplate: the teaching template behind the editor's
// Create → Surface Shader action. A new surface ships as a PAIR — a complete,
// compiling .glsl demonstrating every user-facing mechanism (the
// EvaluateSurface contract, @texture declarations, GE_USER_TEXTURE, @property
// declarations read through Props, a user keyword, the time uniform,
// nits-based emission) plus a companion .material pre-wired to it. Lives in the Rendering module so the
// template and the compose/compile contract it teaches are tested together
// against the real pipeline.

#include <string>

namespace GameEngine
{
struct MaterialDocument;

namespace Rendering
{

// GLSL body for a new surface shader. Deliberately name-free: the file is renamed
// immediately after creation (Create drops straight into inline rename), and a header
// that quoted the creation-time name would be wrong from the first rename onward.
std::string MakeSurfaceShaderTemplateSource();

// Companion material document referencing <stem>.glsl (material-dir-first
// resolution) with the keyword the template consumes and no property
// overrides — every parameter starts at its declared default.
// surfaceShaderGuid is left empty — the creating editor fills it once the
// shader file is registered with the asset system.
MaterialDocument MakeSurfaceShaderTemplateMaterial(const std::string& stem);

} // namespace Rendering
} // namespace GameEngine
