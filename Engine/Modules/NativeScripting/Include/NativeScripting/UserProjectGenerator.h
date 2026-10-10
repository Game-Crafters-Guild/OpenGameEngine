#pragma once

// UserProjectGenerator — writes the standalone CMakeLists.txt that builds a user
// module DLL against this engine. Mirrors the C# ScriptManager::EnsureAutoProject
// pattern: content-compared regen (only rewrites on byte-diff).
//
// The generated project is intentionally standalone (no Engine CMake target): it
// links the Engine IMPORT LIB and consumes the engine's include dirs / compile defs
// directly, since a user DLL is built out-of-tree from the engine. SDK-rooted paths
// go through the GAMEENGINE_SDK_DIR variable — passed live on every engine-driven
// configure, baked only as an IDE last-known-good default — so the persisted file
// never pins a dead machine path (#370). The header marker + full content-compared
// rewrite make this file exclusively generator-owned; user edits are overwritten.

#include "NativeScripting/NativeBuildConfig.h"

#include <filesystem>
#include <string>

namespace GameEngine
{
namespace NativeScripting
{

class UserProjectGenerator
{
public:
    // Write <ProjectDir>/CMakeLists.txt for the config (SourceDir when ProjectDir
    // is empty). Returns its path (empty on failure, with outError set).
    // outChanged is true iff the file was (re)written.
    static std::filesystem::path Generate(const NativeBuildConfig& config,
                                          bool& outChanged,
                                          std::string& outError);

    // The CMakeLists.txt content for a config (pure; exposed for tests).
    static std::string BuildCMakeListsContent(const NativeBuildConfig& config);
};

} // namespace NativeScripting
} // namespace GameEngine
