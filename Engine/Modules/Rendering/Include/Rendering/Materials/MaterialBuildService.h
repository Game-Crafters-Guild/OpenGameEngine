#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Rendering/Materials/MaterialBuildContext.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/ShaderVariantKey.h"

namespace GameEngine::Rendering
{
struct ShaderPackage;
enum class ShaderSourceKind : uint8_t;

struct ShaderPackageDeleter
{
    void operator()(ShaderPackage* p) const noexcept;
};

struct MaterialBuildResult
{
    bool success = false;
    std::vector<std::string> errors;

    // Cached output location (typically under <workspace>/.Cache/Shaders/...).
    std::string generatedShaderPkgPath;

    // Populated on success.
    std::unique_ptr<ShaderPackage, ShaderPackageDeleter> package;

    // The exact GLSL each stage was compiled from, and the macro definitions it
    // was compiled with. Populated as soon as composition succeeds — including
    // on a later compile failure. The web variant cook re-runs this same text
    // through the browser toolchain (glslc/naga/tint), so it must be the text
    // the SPIR-V came from rather than a recomposition of it.
    std::string composedVertexSource;
    std::string composedFragmentSource;
    std::vector<std::string> composedDefines;

    // Every file the build read beyond the composed source itself: the adapter
    // template files (inlined into the composed source, so they never appear as
    // #include directives) plus the compile service's include closure — which
    // covers the surface shader, the vertex modifier, and every transitive
    // include. Absolute, lexically normal. Populated whenever the build reached
    // the compile service, INCLUDING on stage-compile failure (see
    // ShaderProgramCompileResult::includeClosurePaths); empty when composition
    // itself failed. Feeds the shader-edit invalidation trigger.
    std::vector<std::string> includeClosurePaths;
};

// Builds (compiles + reflects) a parsed material document into a shader package.
// `context` provides pre-resolved paths for the adapter shader directory, cache
// root, and include directories -- the caller (Engine/Editor) resolves these via
// AssetManager so the Rendering module stays independent of the asset system.
// `pipelineKeywords` are merged into the variant key -- they represent pipeline-level
// decisions (e.g. ForwardPlus, Instanced) that are not intrinsic to the material.
// `kind` is the shader form the consuming device ingests
// (IDevice::PreferredShaderSource) and decides what the returned package's
// stage bytes carry -- see ShaderCompileService::CompileProgramToCache.
MaterialBuildResult BuildMaterialToShaderPackage(const GameEngine::MaterialDocument& doc,
                                                 const std::filesystem::path& materialPath,
                                                 const std::string& debugName,
                                                 const MaterialBuildContext& context,
                                                 ShaderSourceKind kind,
                                                 MaterialKeyword pipelineKeywords = MaterialKeyword::None,
                                                 VertexAttributeFlags meshVertexFlags = VertexAttributeFlags::None);
} // namespace GameEngine::Rendering

