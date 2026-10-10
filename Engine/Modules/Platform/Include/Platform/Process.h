#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace GameEngine::Platform
{
// A process identity for diagnostics and names shared by concurrent processes.
inline uint64_t GetCurrentProcessId()
{
#if defined(_WIN32)
    return static_cast<uint64_t>(::_getpid());
#else
    return static_cast<uint64_t>(::getpid());
#endif
}

/// One edit to the environment a child process inherits from this process.
struct EnvironmentEdit
{
    /// The variable's name: not empty and free of '='. Compared case-insensitively
    /// on Windows, exactly elsewhere.
    std::string Name;
    /// The value to set, replacing any inherited one; no value removes the variable.
    std::optional<std::string> Value;
};

#if defined(_WIN32)
/// The CreateProcessW environment block for a child: this process's environment
/// with `edits` applied in order, sorted by name without regard to case as
/// CreateProcess requires, each entry NUL-terminated and the block closed by one
/// more NUL. Empty when `edits` is empty, so the child inherits. Nullopt when an
/// edit's name is empty or holds '=': such a name would match or write the
/// drive-directory entries ("=C:=C:\...") instead of a variable.
std::optional<std::wstring> BuildChildEnvironmentBlock(std::span<const EnvironmentEdit> edits);
#else
/// A child's environment as NAME=value entries: this process's environment with
/// `edits` applied in order. Build it before fork(): the child may only make
/// async-signal-safe calls before exec. Nullopt when an edit's name is empty or
/// holds '='.
std::optional<std::vector<std::string>> BuildChildEnvironment(std::span<const EnvironmentEdit> edits);
#endif
} // namespace GameEngine::Platform
