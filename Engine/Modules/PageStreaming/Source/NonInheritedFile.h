#pragma once

#include <cstdio>
#include <filesystem>

namespace GameEngine::PageStreaming
{

/// How a page-streaming file is opened.
enum class NonInheritedFileMode
{
    CreateReadWrite, ///< create or truncate, read and write ("w+b")
    OpenReadWrite,   ///< an existing file, read and write ("r+b")
    CreateWrite,     ///< create or truncate, write only ("wb")
};

/// Opens `path` for the stores' writers with a handle no child process inherits. The engine starts
/// child processes that inherit every inheritable handle (a shader compile, a version-control
/// command); a CRT handle is inheritable by default, so a child started while a store is written
/// would keep its temporary file open after the writer closed it, and the publishing rename would
/// fail with a sharing violation. Null when the file cannot be opened.
std::FILE* OpenNonInheritedFile(const std::filesystem::path& path, NonInheritedFileMode mode);

} // namespace GameEngine::PageStreaming
