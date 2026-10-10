#include "Engine/Build/DotnetHost.h"

#include "Engine/Build/CancellableShellProcess.h"
#include "Logger/Logger.h"
#include "NativeScripting/SdkManifest.h"

namespace GameEngine
{

const std::string& DotnetHostCommand()
{
    static const std::string command = []() -> std::string {
        const std::filesystem::path resolved = NativeScripting::ResolveDotnetExecutable({});
        if (!resolved.empty())
        {
            Logger::Log::Debug("[DotnetHost] Using dotnet at {}", resolved.string());
            return resolved.string();
        }
        Logger::Log::Warning(
            "[DotnetHost] dotnet not found via DOTNET_ROOT, PATH, or standard install "
            "locations; C# compile steps will fail. Install the .NET SDK or set DOTNET_ROOT.");
        return "dotnet";
    }();
    return command;
}

namespace
{

bool ProbeDotnetSdk()
{
    const ShellProcessResult versionResult = RunProcessCaptured(DotnetHostCommand(), {"--version"});
    if (versionResult.exitCode != 0)
    {
        Logger::Log::Error("[DotnetHost] 'dotnet --version' failed (exit code {}); C# compilation is unavailable. "
                           "Install the .NET SDK or set DOTNET_ROOT.",
                           versionResult.exitCode);
        return false;
    }

    std::string version = versionResult.output;
    const std::size_t newline = version.find_first_of("\r\n");
    if (newline != std::string::npos)
        version.erase(newline);
    if (version.empty())
    {
        Logger::Log::Error("[DotnetHost] 'dotnet --version' printed no version; C# compilation is unavailable.");
        return false;
    }

    Logger::Log::Debug("[DotnetHost] .NET SDK available, version {}", version);
    return true;
}

} // namespace

bool IsDotnetSdkAvailable()
{
    static const bool available = ProbeDotnetSdk();
    return available;
}

} // namespace GameEngine
