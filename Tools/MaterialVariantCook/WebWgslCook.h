#pragma once

// Translation of one cooked material variant into the WGSL a browser ingests.
//
// A browser's WebGPU accepts no SPIR-V, so the .shaderpkg the web runtime
// resolves must carry a "<stage>-wgsl" chunk beside the SPIR-V one. The chain
// is Tools/ShaderCook/shadercook.py verbatim — glslc --target-env=vulkan1.1
// (naga rejects SPIR-V 1.4+), spirv-opt --split-combined-image-sampler with
// the +64 sampler renumber, naga --keep-coordinate-space, then tint as the
// Chrome conformance gate. Reusing the script rather than re-encoding those
// constraints here is the point: there is one place where they can drift.
//
// The input is the composed GLSL the SPIR-V was compiled FROM, so the two
// artifacts in a package can never describe different shaders.

#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine::Tools
{

struct WebWgslCookRequest
{
    std::filesystem::path ShaderCookScript; // Tools/ShaderCook/shadercook.py
    std::filesystem::path ScratchDir;
    std::filesystem::path PackagePath;      // the variant's cooked program.shaderpkg
    std::string VertexSource;
    std::string FragmentSource;
    std::vector<std::string> Defines;
    std::vector<std::filesystem::path> IncludeRoots;
    std::string DebugName;                  // scratch file naming + diagnostics
};

// Cook both stages and rewrite the package with the WGSL chunks added. Returns
// false and fills outError on any failure, including a tint rejection — an
// unvalidated WGSL chunk is exactly what the browser refuses at runtime, so it
// must never reach a package.
bool CookWebWgslIntoPackage(const WebWgslCookRequest& request, std::string& outError);

} // namespace GameEngine::Tools
