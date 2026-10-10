#include "FileSystem/FileSystem.h"

namespace GameEngine::FileSystem
{

std::filesystem::path ProjectLibraryRoot()
{
    return {};
}

bool IsCaseSensitive()
{
#if defined(__linux__)
    return true;
#else
    return false;
#endif
}

} // namespace GameEngine::FileSystem
