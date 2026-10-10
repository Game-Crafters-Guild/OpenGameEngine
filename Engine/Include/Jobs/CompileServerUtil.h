#pragma once
#include <string>
#include <filesystem>
#include "Jobs/WorkspaceId.h"

namespace GameEngine {

// Returns the per-workspace compile server pipe name, e.g., "GE_CompileServer_<id>".
// Uses a stable WorkspaceId derived from the absolute project root path.
inline std::string ComputeCompileServerPipeName(const std::filesystem::path& projectRoot)
{
    try {
        std::filesystem::path root = std::filesystem::weakly_canonical(projectRoot);
        std::string wsId = WorkspaceId::Compute(root.string());
        return std::string("GE_CompileServer_") + wsId;
    } catch (...) {
        // Fallback without canonicalization
        std::string wsId = WorkspaceId::Compute(projectRoot.string());
        return std::string("GE_CompileServer_") + wsId;
    }
}

} // namespace GameEngine

