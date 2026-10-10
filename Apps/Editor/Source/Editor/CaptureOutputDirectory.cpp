#include "Editor/CaptureOutputDirectory.h"

#include "Core/Application.h"
#include "Logger/Logger.h"

#include <string>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace GameEngine::Editor
{
namespace
{

constexpr const char* kCaptureRootName = "gameengine_mcp";
constexpr const char* kProcessDirPrefix = "pid-";

unsigned long CurrentProcessId()
{
#if defined(_WIN32)
    return static_cast<unsigned long>(::GetCurrentProcessId());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

std::filesystem::path MakeProcessCaptureDirectory()
{
    std::error_code ec;
    std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec || base.empty())
    {
        // Anchor to the executable, never the working directory: the editor is
        // launched from varying cwds and must not drop captures into whatever
        // tree it happened to start in.
        base = PathUtils::GetExecutableDirectory();
    }

    std::filesystem::path dir =
        base / kCaptureRootName / (kProcessDirPrefix + std::to_string(CurrentProcessId()));
    std::error_code removeEc;
    std::filesystem::remove_all(dir, removeEc);
    if (removeEc)
    {
        // A frame held open by a viewer or scanner survives the clear; its name
        // is one this process is about to reuse, so say so instead of letting a
        // stale pixel masquerade silently.
        Logger::Log::Warning("Capture output directory '{}' could not be cleared ({})",
                             dir.string(), removeEc.message());
    }
    return dir;
}

} // namespace

std::filesystem::path CaptureOutputDirectory()
{
    static const std::filesystem::path kDirectory = MakeProcessCaptureDirectory();

    std::error_code ec;
    std::filesystem::create_directories(kDirectory, ec);
    if (ec)
    {
        Logger::Log::Warning("Capture output directory '{}' could not be created ({})",
                             kDirectory.string(), ec.message());
    }
    return kDirectory;
}

} // namespace GameEngine::Editor
