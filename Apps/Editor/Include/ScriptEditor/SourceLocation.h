#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string_view>

namespace GameEngine::Editor
{

/// A place in a source file. Line and column are 1-based; 0 means unknown.
struct SourceLocation
{
    std::filesystem::path Path;
    size_t Line = 0;
    size_t Column = 0;
};

/// The last C# file location a log line names, in one of the forms compilers
/// and stack traces print:
///   Path/File.cs:12:5    Path/File.cs:12
///   Path/File.cs(12,5)   Path/File.cs(12)    Path/File.cs
/// The path runs back from ".cs" to the nearest whitespace, quote or bracket.
/// Returns nothing when the line names no .cs file.
std::optional<SourceLocation> FindSourceLocationInLogLine(std::string_view text);

} // namespace GameEngine::Editor
