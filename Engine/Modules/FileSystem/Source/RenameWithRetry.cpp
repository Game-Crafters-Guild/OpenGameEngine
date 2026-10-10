#include "FileSystem/RenameWithRetry.h"

#include <chrono>
#include <thread>

namespace GameEngine::FileSystem
{
namespace
{
constexpr int kRenameAttempts = 5;
constexpr std::chrono::milliseconds kRenameRetryDelay{200};
} // namespace

bool RenameWithRetry(const std::filesystem::path& from, const std::filesystem::path& to,
                     const std::function<bool()>& shouldCancel, std::error_code& error)
{
    for (int attempt = 0; attempt < kRenameAttempts; ++attempt)
    {
        if (shouldCancel && shouldCancel())
            return false;
        if (attempt > 0)
            std::this_thread::sleep_for(kRenameRetryDelay);
        error.clear();
        std::filesystem::rename(from, to, error);
        if (!error)
            return true;
        // A missing source or a move across volumes cannot clear by waiting.
        if (error != std::errc::permission_denied && error != std::errc::device_or_resource_busy)
            return false;
    }
    return false;
}
} // namespace GameEngine::FileSystem
