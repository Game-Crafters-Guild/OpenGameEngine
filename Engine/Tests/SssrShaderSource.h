#pragma once

// Reads an SSSR shader's repo source so a test can assert on the shipped text.
//
// The shader source is the source of truth: a hand-copied mirror of a constant or
// an expression is exactly what drifts away from the shader it claims to pin.
// Anchored to the repo via GE_RENDERER_REPO_ROOT (dev-only, never shipped) rather
// than to the staged copy, because a staged shader only refreshes when its staging
// target rebuilds — which a shader-only edit does not trigger. Same precedent as
// IblShaderContractTests and SssrRefineBudgetTests.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace GE::Tests
{

namespace Detail
{
// Read the file as bytes, then drop CR. .gitattributes gives *.glsl an explicit
// `eol=lf` but leaves *.comp on `text=auto`, so the two halves of one shader arrive
// with DIFFERENT line endings in a Windows working tree. Any assertion spanning a
// line boundary would then pass on one file kind and fail on the other, for a
// reason that has nothing to do with what it is testing.
inline std::string ReadNormalized(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f.is_open())
        return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}
} // namespace Detail

// `fileName` is relative to the renderer's shader directory, e.g.
// "ScreenSpaceReflections/sssr_prefilter.comp" or "Adapters/adapter_forward.glsl".
// Returns an empty string when the repo anchor is unavailable, which callers
// assert on.
inline std::string ReadSssrShaderSource(const std::string& fileName)
{
#ifndef GE_RENDERER_REPO_ROOT
    (void)fileName;
    return {};
#else
    return Detail::ReadNormalized(std::filesystem::path(GE_RENDERER_REPO_ROOT) /
                                  "Engine/Modules/Rendering/Shaders" / fileName);
#endif
}

// The pipeline node that owns the SSSR passes. Several of these contracts are half
// C++ — dispatch offsets, pass order, which sampler a shader's fetch resolves
// through — so the suites pinning them need the node's text next to the shaders'.
inline std::string ReadSssrNodeSource()
{
#ifndef GE_RENDERER_REPO_ROOT
    return {};
#else
    return Detail::ReadNormalized(
        std::filesystem::path(GE_RENDERER_REPO_ROOT) /
        "Engine/Source/Engine/Rendering/Pipeline/Nodes/ScreenSpaceReflectionsNode.cpp");
#endif
}

} // namespace GE::Tests
