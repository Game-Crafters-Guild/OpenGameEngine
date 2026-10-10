#pragma once
#include <string>

namespace GameEngine {

// Generates a stable per-workspace identifier used for namespacing IPC resources.
// Default implementation: SHA-1 of the absolute workspace (project root) path, hex-encoded and truncated.
class WorkspaceId {
public:
    static std::string Compute(const std::string& projectRootAbsUtf8);
};

} // namespace GameEngine

