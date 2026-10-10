#include "Editor/Application/BuildConfigStatus.h"

#include "Platform/Shell.h"

#include <chrono>
#include <ctime>
#include <filesystem>

namespace GameEngine::Editor
{
namespace
{
    std::string BuildTimestampSuffix()
    {
        std::error_code ec;
        const std::filesystem::path exe = Platform::GetExecutablePath();
        if (exe.empty())
            return {};
        const auto fsTime = std::filesystem::last_write_time(exe, ec);
        if (ec)
            return {};
        // file_clock -> system_clock via the epoch-offset trick; clock_cast is
        // not available across all deployed libc++ versions.
        const auto sysTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            fsTime - std::filesystem::file_time_type::clock::now() +
            std::chrono::system_clock::now());
        const std::time_t t = std::chrono::system_clock::to_time_t(sysTime);
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &t);
#else
        localtime_r(&t, &local);
#endif
        char buf[32];
        if (std::strftime(buf, sizeof(buf), " - %b %d %H:%M", &local) == 0)
            return {};
        return buf;
    }
} // namespace

std::string BuildConfigStatusText()
{
    return GE_BUILD_CONFIG " Build" + BuildTimestampSuffix();
}
} // namespace GameEngine::Editor
