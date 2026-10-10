#pragma once

// CMakeInvoker — runs `cmake` configure + `cmake --build` for a user project.
//
// C10: synchronous (blocks the calling thread). The async/JobSystem + 8s-throttle
// wrapper is a later slice; this is the unit the editor-runtime path will submit to
// a worker. Output (merged stdout+stderr) is captured for the Output console.

#include "NativeScripting/NativeBuildConfig.h"

#include <filesystem>
#include <functional>
#include <string>

namespace GameEngine
{
namespace NativeScripting
{

// Counts parsed from the scanner's machine-readable summary line, so the build pipeline
// can surface a console warning when a scan reflects nothing, conservatively skips a
// struct, or finds a macro-free component in a .cpp. -1 means the summary wasn't found.
struct ComponentScanStats
{
    int Reflected = -1;
    int Skipped = -1;
    int SourceFileWarnings = -1;
};

class CMakeInvoker
{
public:
    // Configure (only when the build dir has no CMakeCache.txt, unless forceConfigure)
    // then build. Returns the built user-DLL path on success, or empty with outError
    // set. outOutput receives the merged build log. shouldCancel is polled by the
    // underlying process runner; pass {} for no cancellation.
    static std::filesystem::path ConfigureAndBuild(const NativeBuildConfig& config,
                                                    bool forceConfigure,
                                                    std::string& outOutput,
                                                    std::string& outError,
                                                    std::function<bool()> shouldCancel = {});

    // True if the build dir already holds a CMake cache (configure-once gate).
    static bool CachePresent(const NativeBuildConfig& config);

    // Test seam: when set, every process this class runs (cmake configure and build, the
    // scanners) calls `runner` with the shell command instead of starting it; the runner
    // appends the process's output and returns its exit code. Pass {} to restore the shell.
    using ProcessRunner = std::function<int(const std::string& command, std::string& outOutput)>;
    static void SetProcessRunnerForTests(ProcessRunner runner);

    // Run the ComponentScanner (dotnet) over config.SourceDir to emit the user
    // component-registration .cpp into config.BuildDir. Returns the generated file's
    // path on success, or empty (with outError set) on failure. Captures scanner
    // output into outOutput and fills outStats from the scanner's summary line. Caller
    // gates on DotnetExe + ComponentScannerDll being set.
    static std::filesystem::path RunComponentScanner(const NativeBuildConfig& config,
                                                     std::string& outOutput,
                                                     std::string& outError,
                                                     ComponentScanStats& outStats);

    // Run the scanner in --detect-system-base mode over config.SourceDir to emit the user
    // system-registration .cpp (one RegisterUserSystem per ECS::SystemBase struct) into
    // config.BuildDir. Returns the generated file's path on success, or empty (with outError
    // set) on failure. Scanner output is appended to outOutput.
    static std::filesystem::path RunSystemScanner(const NativeBuildConfig& config,
                                                  std::string& outOutput,
                                                  std::string& outError);

private:
    static std::filesystem::path FindBuiltDll(const NativeBuildConfig& config);
};

} // namespace NativeScripting
} // namespace GameEngine
