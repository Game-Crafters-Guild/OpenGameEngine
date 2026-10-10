#include "NativeScripting/CMakeInvoker.h"

#include "Engine/Build/CancellableShellProcess.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string_view>
#include <utility>

namespace GameEngine
{
namespace NativeScripting
{

namespace
{
std::string Quote(const std::filesystem::path& p)
{
    return "\"" + p.generic_string() + "\"";
}

std::mutex& ProcessRunnerMutex()
{
    static std::mutex mutex;
    return mutex;
}

CMakeInvoker::ProcessRunner& ProcessRunnerForTests()
{
    static CMakeInvoker::ProcessRunner runner;
    return runner;
}

// Run a command via the platform shell (or the test runner, when one is set),
// capturing merged stdout+stderr. Returns the exit code; appends the captured
// log to outOutput.
int RunCaptured(const std::string& command, std::string& outOutput, std::function<bool()> shouldCancel)
{
    CMakeInvoker::ProcessRunner runner;
    {
        std::lock_guard<std::mutex> lock(ProcessRunnerMutex());
        runner = ProcessRunnerForTests();
    }
    if (runner)
        return runner(command, outOutput);

    CancellableShellProcess proc;
    auto cancelFn = shouldCancel ? shouldCancel : std::function<bool()>([] { return false; });
    // "2>&1" merges stderr into the captured output (see ShellProcessResult docs).
    ShellProcessResult result = proc.Run(command + " 2>&1", cancelFn, {});
    outOutput += result.output;
    if (result.cancelled)
        return -2;
    return result.exitCode;
}

// Parse the scanner's "ComponentScanner.summary reflected=N skipped=N sourcewarnings=N"
// line (last occurrence) out of the captured output. Missing keys stay -1.
ComponentScanStats ParseScanSummary(const std::string& output)
{
    ComponentScanStats stats;
    constexpr const char* kKey = "ComponentScanner.summary";
    const std::size_t pos = output.rfind(kKey);
    if (pos == std::string::npos)
        return stats;

    std::istringstream iss(output.substr(pos));
    std::string token;
    while (iss >> token)
    {
        const std::size_t eq = token.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = token.substr(0, eq);
        int value = 0;
        try
        {
            value = std::stoi(token.substr(eq + 1));
        }
        catch (...)
        {
            continue;
        }
        if (key == "reflected")
            stats.Reflected = value;
        else if (key == "skipped")
            stats.Skipped = value;
        else if (key == "sourcewarnings")
            stats.SourceFileWarnings = value;
    }
    return stats;
}

// The cmake build dir for the selected generator. Ninja and the VS generator can't share a
// build dir (incompatible CMakeCache), so the Ninja build uses a 'ninja' subdir; the VS
// generator keeps the configured BuildDir.
std::filesystem::path EffectiveBuildDir(const NativeBuildConfig& config)
{
    return config.NinjaExe.empty() ? config.BuildDir : (config.BuildDir / "ninja");
}

void ResetCacheForProjectMove(const std::filesystem::path& buildDir,
                              const std::filesystem::path& projectDir)
{
    const std::filesystem::path cachePath = buildDir / "CMakeCache.txt";
    std::ifstream cache(cachePath);
    std::string line;
    constexpr std::string_view kHomeKey = "CMAKE_HOME_DIRECTORY:INTERNAL=";
    while (cache && std::getline(cache, line))
    {
        if (line.rfind(kHomeKey, 0) != 0)
            continue;

        const std::filesystem::path previousProject = line.substr(kHomeKey.size());
        std::string prev = previousProject.lexically_normal().generic_string();
        std::string next = projectDir.lexically_normal().generic_string();
#if defined(_WIN32)
        // Windows paths are case-insensitive and CMake stores the -S path as given:
        // a pure case difference is the same directory, and treating it as a move
        // would silently reset the cache (a full reconfigure) on every launch.
        // ASCII fold suffices — a non-ASCII case miss costs one extra reconfigure,
        // never a wrong build.
        const auto fold = [](std::string& s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        };
        fold(prev);
        fold(next);
#endif
        if (prev == next)
            return;

        cache.close();
        std::error_code ec;
        std::filesystem::remove(cachePath, ec);
        ec.clear();
        std::filesystem::remove_all(buildDir / "CMakeFiles", ec);
        Logger::Log::Info("[NativeScripting] reset CMake cache after generated project moved from '{}' to '{}'",
                          previousProject.string(), projectDir.string());
        return;
    }
}

// 'call "<env.bat>" && ' prefix that loads the cached MSVC environment so cl.exe finds its
// headers/libs without re-running vcvars; empty when no env batch (POSIX / VS generator).
// The batch path uses the native (backslash) form for cmd's `call`.
std::string MsvcEnvPrefix(const NativeBuildConfig& config)
{
    if (config.MsvcEnvBatch.empty())
        return {};
    return "call \"" + config.MsvcEnvBatch.string() + "\" && ";
}
} // namespace

bool CMakeInvoker::CachePresent(const NativeBuildConfig& config)
{
    std::error_code ec;
    return std::filesystem::exists(EffectiveBuildDir(config) / "CMakeCache.txt", ec);
}

std::filesystem::path CMakeInvoker::RunComponentScanner(const NativeBuildConfig& config,
                                                        std::string& outOutput,
                                                        std::string& outError,
                                                        ComponentScanStats& outStats)
{
    std::error_code ec;
    std::filesystem::create_directories(config.BuildDir, ec);
    // The scanner errors if --in is absent; ensure it exists so a not-yet-populated
    // project scans as zero components (matching how Generate tolerates an empty dir).
    std::filesystem::create_directories(config.SourceDir, ec);
    const std::filesystem::path genCpp = config.BuildDir / "UserComponentReflection.gen.cpp";

    std::ostringstream cmd;
    cmd << Quote(config.DotnetExe) << " " << Quote(config.ComponentScannerDll)
        << " --in " << Quote(config.SourceDir)
        << " --out " << Quote(genCpp)
        << " --detect-base \"" << config.DetectBase << "\"";
    Logger::Log::Info("[NativeScripting] scanning components in {}", config.SourceDir.string());
    const int rc = RunCaptured(cmd.str(), outOutput, {});
    if (rc != 0)
    {
        outError = "component scanner failed (exit " + std::to_string(rc) + ")";
        return {};
    }
    outStats = ParseScanSummary(outOutput);
    return genCpp;
}

std::filesystem::path CMakeInvoker::RunSystemScanner(const NativeBuildConfig& config,
                                                     std::string& outOutput,
                                                     std::string& outError)
{
    std::error_code ec;
    std::filesystem::create_directories(config.BuildDir, ec);
    std::filesystem::create_directories(config.SourceDir, ec);
    const std::filesystem::path genCpp = config.BuildDir / "UserSystemRegistration.gen.cpp";

    constexpr const char* kSystemDetectBase = "SystemBase";
    std::ostringstream cmd;
    cmd << Quote(config.DotnetExe) << " " << Quote(config.ComponentScannerDll)
        << " --in " << Quote(config.SourceDir)
        << " --out " << Quote(genCpp)
        << " --detect-system-base \"" << kSystemDetectBase << "\"";
    Logger::Log::Info("[NativeScripting] scanning systems in {}", config.SourceDir.string());

    // RunCaptured appends, so pass outOutput straight through (it already holds the component scan).
    const int rc = RunCaptured(cmd.str(), outOutput, {});
    if (rc != 0)
    {
        outError = "system scanner failed (exit " + std::to_string(rc) + ")";
        return {};
    }
    return genCpp;
}

std::filesystem::path CMakeInvoker::FindBuiltDll(const NativeBuildConfig& config)
{
#if defined(_WIN32)
    const std::string ext = ".dll";
#elif defined(__APPLE__)
    const std::string ext = ".dylib";
#else
    const std::string ext = ".so";
#endif
    // The generated project lands the DLL in <BuildDir>/out, but be tolerant of
    // generator-specific config subdirs by walking the whole build dir for the
    // newest matching artifact.
    std::filesystem::path best;
    std::filesystem::file_time_type bestTime{};
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(EffectiveBuildDir(config), ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); ++it)
    {
        const auto& p = it->path();
        if (p.extension() != ext)
            continue;
        if (p.stem().generic_string().find(config.ModuleName) == std::string::npos)
            continue;
        const auto t = std::filesystem::last_write_time(p, ec);
        if (ec)
            continue;
        if (best.empty() || t > bestTime)
        {
            best = p;
            bestTime = t;
        }
    }
    return best;
}

void CMakeInvoker::SetProcessRunnerForTests(ProcessRunner runner)
{
    std::lock_guard<std::mutex> lock(ProcessRunnerMutex());
    ProcessRunnerForTests() = std::move(runner);
}

std::filesystem::path CMakeInvoker::ConfigureAndBuild(const NativeBuildConfig& config,
                                                      bool forceConfigure,
                                                      std::string& outOutput,
                                                      std::string& outError,
                                                      std::function<bool()> shouldCancel)
{
    const std::filesystem::path buildDir = EffectiveBuildDir(config);
    const std::filesystem::path projectDir = EffectiveProjectDir(config);
    std::error_code ec;
    std::filesystem::create_directories(buildDir, ec);
    if (ec)
    {
        outError = "cannot create build dir " + buildDir.string() + ": " + ec.message();
        return {};
    }

    ResetCacheForProjectMove(buildDir, projectDir);

    const bool useNinja = !config.NinjaExe.empty();
    const std::string envPrefix = MsvcEnvPrefix(config); // loads the cached MSVC env (Ninja)
    const std::string cmake = Quote(config.CMakeExe);

    if (forceConfigure || !CachePresent(config))
    {
        std::ostringstream cfg;
        cfg << envPrefix << cmake << " -S " << Quote(projectDir) << " -B " << Quote(buildDir)
            << " -DCMAKE_BUILD_TYPE=" << config.Config;
        // The generated project references engine paths through GAMEENGINE_SDK_DIR; pass
        // the LIVE SDK root on every engine-driven configure so the baked last-known-good
        // default (an IDE convenience) is never load-bearing here.
        if (!config.SdkRoot.empty())
            cfg << " -DGAMEENGINE_SDK_DIR=" << Quote(config.SdkRoot);
        if (useNinja)
        {
            // Ninja: minimal per-build overhead (no MSBuild/ZERO_CHECK). The MSVC env is
            // supplied by envPrefix so cmake's compiler probe + cl.exe work.
            cfg << " -G Ninja -DCMAKE_MAKE_PROGRAM=" << Quote(config.NinjaExe);
        }
#if defined(_WIN32)
        else
        {
            // VS generator defaults to Win32; force x64 so the user DLL matches the host.
            cfg << " -A x64";
        }
#endif
        Logger::Log::Info("[NativeScripting] cmake configure ({}): {}",
                          useNinja ? "Ninja" : "default", projectDir.string());
        const int rc = RunCaptured(cfg.str(), outOutput, shouldCancel);
        if (rc != 0)
        {
            outError = "cmake configure failed (exit " + std::to_string(rc) + ")";
            return {};
        }
    }

    std::ostringstream build;
    build << envPrefix << cmake << " --build " << Quote(buildDir);
    if (!useNinja)
        build << " --config " << config.Config; // VS is multi-config; Ninja is single-config
    Logger::Log::Info("[NativeScripting] cmake --build ({}): {}", useNinja ? "Ninja" : "default", buildDir.string());
    const int rc = RunCaptured(build.str(), outOutput, shouldCancel);
    if (rc != 0)
    {
        outError = "cmake --build failed (exit " + std::to_string(rc) + ")";
        return {};
    }

    std::filesystem::path dll = FindBuiltDll(config);
    if (dll.empty())
        outError = "build succeeded but no '" + config.ModuleName + "' DLL found under " + buildDir.string();
    return dll;
}

} // namespace NativeScripting
} // namespace GameEngine
