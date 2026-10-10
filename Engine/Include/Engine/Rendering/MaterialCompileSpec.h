#pragma once

#include <string>
#include <vector>

namespace GameEngine
{
namespace Engine::Renderer
{

struct MaterialCompileSpec
{
    std::string surfaceShaderPath;
    std::string vertexModifierPath;
    std::string lightingModel;
    // Carries MaterialDocument::customVertexShader into the ShaderCompilationCache
    // synthetic doc so the build-service clamp (vertexFlags->None, no vertex
    // buffer) fires for cache-driven variant compiles, not just direct builds.
    bool customVertexShader = false;
    // Sanitized user keyword names (MaterialDocument::keywords). Rides the spec
    // exactly like customVertexShader so a cache-driven recompile reconstitutes
    // the GE_USER_<NAME> defines from the synthetic doc — a hash-only carrier
    // would compile define-less (the danger the userKeywordHash lane exposes).
    std::vector<std::string> userKeywords;
    // Mask materials cook with ALPHA_TEST folded in from the document. The
    // cache-driven intern path synthesizes a document that otherwise defaults
    // to Opaque, so without this bit a keyword-less intern hashes a different
    // program than the cook wrote.
    bool alphaTest = false;
    // The Parallax keyword registration derived (ApplyParallaxKeyword: a bound heightMap on a
    // surface that declares one, with hex tiling off). The cache-driven synthetic doc carries no
    // texture bindings, so it reconstitutes the height binding from this bit; carrying the raw
    // binding instead would re-derive the march for a hex-tiled material, whose hexTiling the
    // synthetic doc does not carry either. Always equal to the key's Parallax bit.
    bool parallax = false;
};

} // namespace Engine::Renderer
} // namespace GameEngine
