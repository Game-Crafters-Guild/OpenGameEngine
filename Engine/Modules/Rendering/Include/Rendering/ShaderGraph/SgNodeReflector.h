#pragma once

#include "Rendering/ShaderGraph/SgTypes.h"

#include <filesystem>

namespace GameEngine::ShaderGraph
{

class SgNodeReflector
{
  public:
    static SgNodeLibraryIndex BuildIndex(const std::filesystem::path& engineNodesRoot,
                                         const std::filesystem::path& projectNodesRoot);

    static bool WriteCache(const std::filesystem::path& cachePath, const SgNodeLibraryIndex& index);
    static bool TryLoadCache(const std::filesystem::path& cachePath,
                             const std::filesystem::path& engineNodesRoot,
                             const std::filesystem::path& projectNodesRoot,
                             SgNodeLibraryIndex& outIndex);
};

} // namespace GameEngine::ShaderGraph
