#pragma once

#include "Rendering/ShaderGraph/SgTypes.h"

#include <filesystem>
#include <string>

namespace GameEngine::ShaderGraph
{

struct SgGraphFile
{
    SgGraphDocument Document;
    std::string TagBlock;
    std::string Body;
};

SgGraphFile LoadGraphFile(const std::filesystem::path& path);
/** The complete file text for `doc` + `body`: the `@sg-*` tag block followed by
    the GLSL body. SaveGraphFile is this plus a write-if-changed. */
std::string SerializeGraphFileText(const SgGraphDocument& doc, const std::string& body);
bool SaveGraphFile(const std::filesystem::path& path, const SgGraphDocument& doc, const std::string& body);
/** Writes `text` only when it differs from what is already on disk. Rewriting
    identical bytes re-arms the shader-edit file watcher, whose recompile
    rewrites the file again — a self-sustaining loop on an idle editor. */
bool WriteGraphFileTextIfChanged(const std::filesystem::path& path, const std::string& text);
bool SaveCompiledGraphFile(const std::filesystem::path& path, const SgGraphDocument& doc,
                             const SgNodeLibraryIndex& library);

/** Engine shader-graph node helper root: "Shaders/Graph/Nodes" under the install assets
    root the build stages for this executable (the PathUtils::GetInstallAssetsRoot() rule).
    The host registers it through SetEngineGraphNodesRoot (the engine knows its install
    assets root; this module does not); until then the module's own copy of the layout rule
    resolves it. Empty when nothing is staged; a binary that reflects the node library must
    stage the tree beside itself. */
void SetEngineGraphNodesRoot(std::filesystem::path root);
std::filesystem::path GetEngineGraphNodesRoot();
std::filesystem::path GetDefaultNodeLibraryCachePath(const std::filesystem::path& projectRoot);

/** Cached reflector index (engine + optional project helper roots). */
const SgNodeLibraryIndex& GetSharedNodeLibraryIndex(
    const std::filesystem::path& projectNodesRoot = std::filesystem::path{});
void InvalidateSharedNodeLibraryIndex();

} // namespace GameEngine::ShaderGraph
