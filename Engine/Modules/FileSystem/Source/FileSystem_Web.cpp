#include "FileSystem/FileSystem.h"

#include "Platform/WebPersistentStorage.h"

namespace GameEngine::FileSystem
{

std::filesystem::path ProjectLibraryRoot()
{
    return std::filesystem::path(Platform::Web::kProjectsMount);
}

bool IsCaseSensitive()
{
    return true;
}

} // namespace GameEngine::FileSystem
