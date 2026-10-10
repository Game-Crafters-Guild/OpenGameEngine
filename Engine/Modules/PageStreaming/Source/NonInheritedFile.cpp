#include "NonInheritedFile.h"

#ifndef _WIN32
#include <fcntl.h>
#endif

namespace GameEngine::PageStreaming
{

std::FILE* OpenNonInheritedFile(const std::filesystem::path& path, NonInheritedFileMode mode)
{
#ifdef _WIN32
    // "N": the CRT opens the handle non-inheritable.
    const wchar_t* flags = mode == NonInheritedFileMode::CreateReadWrite ? L"w+bN"
                           : mode == NonInheritedFileMode::OpenReadWrite ? L"r+bN"
                                                                         : L"wbN";
    std::FILE* file = nullptr;
    return _wfopen_s(&file, path.c_str(), flags) == 0 ? file : nullptr;
#else
    // "e": O_CLOEXEC, so an exec'd child does not keep the descriptor.
    const char* flags = mode == NonInheritedFileMode::CreateReadWrite ? "w+be"
                        : mode == NonInheritedFileMode::OpenReadWrite ? "r+be"
                                                                      : "wbe";
    return std::fopen(path.c_str(), flags);
#endif
}

} // namespace GameEngine::PageStreaming
