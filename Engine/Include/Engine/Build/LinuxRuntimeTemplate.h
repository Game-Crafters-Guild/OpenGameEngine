#pragma once

#include <filesystem>

namespace GameEngine {

// A Linux runtime template is a compiled Linux x64 Player with its libraries
// (BuildSettings::prebuiltPlayerDirectory). A template export copies the
// runtime and packages the project into it, so every per-game file in the
// template directory is left behind.

/// Whether `path` is an x86-64 little-endian ELF64 file, read from its header.
bool IsLinuxX64Elf(const std::filesystem::path& path);

/// Whether a top-level entry of a runtime template directory belongs to a
/// game rather than to the runtime: project content, asset identity, game
/// configuration, compiled gameplay, and the Steam Deck build's log and
/// archive. Compared case-insensitively.
bool IsRuntimeTemplateGamePayload(const std::filesystem::path& entry);

/// Separate symbol files or symbol directories supplied alongside a runtime.
bool IsRuntimeTemplateDebugSymbol(const std::filesystem::path& entry);

} // namespace GameEngine
