#include "FileSystem/ScopedFileLock.h"

namespace GameEngine::FileSystem
{

// The origin is the only agent that can reach the file; see the class note.
ScopedFileLock::ScopedFileLock(const std::filesystem::path& /*lockFile*/, Mode /*mode*/, bool /*wait*/)
    : m_FailureReason("this platform takes no file locks")
{
}

ScopedFileLock::~ScopedFileLock() = default;

} // namespace GameEngine::FileSystem
