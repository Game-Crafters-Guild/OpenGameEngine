#include "NativeScripting/SdkManifest.h"

#include "ExecutablePathScan.h" // FindExecutableOnPath (fork-free $PATH scan)
#include "Logger/Logger.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

namespace GameEngine
{
namespace NativeScripting
{

namespace
{
// Parse a key=value text file: trims whitespace, skips blank lines and '#' comments.
std::map<std::string, std::string> ParseKeyValue(const std::filesystem::path& path)
{
    std::map<std::string, std::string> kv;
    std::ifstream in(path);
    if (!in)
        return kv;
    std::string line;
    while (std::getline(in, line))
    {
        const auto trim = [](std::string s) {
            const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
            s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
            return s;
        };
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return kv;
}

// Split a ';'-delimited list (skipping empties), e.g. CMake's INTERFACE_COMPILE_DEFINITIONS.
void AppendSemicolonList(const std::string& value, std::vector<std::string>& out)
{
    std::stringstream ss(value);
    std::string item;
    while (std::getline(ss, item, ';'))
        if (!item.empty())
            out.push_back(item);
}

// Resolve a build tool (cmake/dotnet) for THIS machine. The manifest records the dev
// machine's absolute path; on a teammate's machine that path won't exist, so fall back
// to $PATH and common install locations. Returns empty if nothing resolves (callers
// decide whether the tool is required).
std::filesystem::path ResolveBuildTool(const std::string& manifestValue,
                                       const char* toolName,
                                       const std::vector<const char*>& commonLocations)
{
    std::error_code ec;
    if (!manifestValue.empty() && std::filesystem::exists(manifestValue, ec))
        return manifestValue;

    if (std::filesystem::path found = FindExecutableOnPath(toolName); !found.empty())
        return found;

    for (const char* loc : commonLocations)
        if (std::filesystem::exists(loc, ec))
            return std::filesystem::path(loc);

    return {};
}
} // namespace

bool LoadSdkManifest(const std::filesystem::path& sdkRoot,
                     NativeBuildConfig& outConfig,
                     std::string& outError)
{
    const std::filesystem::path manifest = sdkRoot / "nativescripting" / "manifest.txt";
    std::error_code ec;
    if (!std::filesystem::exists(manifest, ec))
    {
        outError = "SDK manifest not found at " + manifest.string();
        return false;
    }

    const std::map<std::string, std::string> kv = ParseKeyValue(manifest);
    const auto get = [&](const char* key) -> std::string {
        const auto it = kv.find(key);
        return it == kv.end() ? std::string{} : it->second;
    };

    // Resolve the essential build inputs (relative to the SDK root). Missing any of these
    // means we cannot build a user DLL at all. cmake is recorded as the dev machine's
    // absolute path; resolve it for THIS machine (PATH / common locations), and fall back
    // to a bare name (the shell resolves it) so a build can still be attempted.
    outConfig.CMakeExe = ResolveBuildTool(get("cmake"), "cmake",
                                          {"/opt/homebrew/bin/cmake", "/usr/local/bin/cmake",
                                           "/Applications/CMake.app/Contents/bin/cmake"});
    if (outConfig.CMakeExe.empty())
        outConfig.CMakeExe = "cmake";
    const std::string includeDir = get("includedir");
    const std::string importLib = get("importlib");
    if (includeDir.empty() || importLib.empty())
    {
        outError = "SDK manifest is missing includedir / importlib";
        return false;
    }
    // The SDK root anchors every engine path below; the generator emits them through
    // the GAMEENGINE_SDK_DIR variable so the persisted user project follows the live
    // SDK instead of pinning this machine's install location.
    outConfig.SdkRoot = sdkRoot;
    outConfig.IncludeDirs.clear();
    outConfig.IncludeDirs.push_back(sdkRoot / includeDir);
    // Transitive include dirs the engine's public headers pull in — vcpkg headers (glm,
    // nlohmann, concurrentqueue, …) and generated headers. These are staged INTO the SDK
    // and recorded SDK-relative; resolving against sdkRoot keeps the SDK self-contained on
    // a machine without the engine source tree. (sdkRoot / abs == abs, so any legacy
    // absolute entries still pass through unchanged.)
    std::vector<std::string> engineIncludes;
    AppendSemicolonList(get("engineincludes"), engineIncludes);
    for (const std::string& dir : engineIncludes)
        outConfig.IncludeDirs.emplace_back(sdkRoot / dir);
    outConfig.EngineImportLib = sdkRoot / importLib;
    outConfig.SdkEntrySource = sdkRoot / get("sdkentry");

    // Compile definitions: the engine's public interface defs + the always-on globals.
    outConfig.CompileDefinitions.clear();
    AppendSemicolonList(get("defs"), outConfig.CompileDefinitions);
    AppendSemicolonList(get("globaldefs"), outConfig.CompileDefinitions);

    // Map the engine's config to a CRT-compatible standalone config for the user DLL.
    // Only Debug uses the MSVC debug CRT (/MDd, iterator-debug); DebugFast/Release/
    // RelWithDebInfo all use the release CRT (/MD), so they map to Release.
    outConfig.Config = (get("config") == "Debug") ? "Debug" : "Release";

    // EditorSDK interface — present only in the editor's staged SDK. Optional:
    // a manifest without it simply cannot build Editor-kind native modules
    // (the editor wiring skips them loudly). Stored into the config as the
    // canonical values; the per-module editor wiring copies them onto
    // Editor-kind module configs and CLEARS them on Runtime/module-project
    // configs so runtime ABI digests stay editor-independent.
    outConfig.EditorImportLib.clear();
    outConfig.EditorIncludeDirs.clear();
    if (const std::string editorImportLib = get("editorimportlib"); !editorImportLib.empty())
    {
        outConfig.EditorImportLib = sdkRoot / editorImportLib;
        std::vector<std::string> editorIncludes;
        AppendSemicolonList(get("editorincludes"), editorIncludes);
        for (const std::string& dir : editorIncludes)
            outConfig.EditorIncludeDirs.emplace_back(sdkRoot / dir);
    }

    // Scanner is optional — when dotnet or the scanner DLL is absent, BuildAndLoad runs
    // in macros-only mode (no auto-detection of `: ECS::ComponentBase`).
    outConfig.DotnetExe = ResolveDotnetExecutable(get("dotnet"));
    const std::string scanner = get("scannerdll");
    outConfig.ComponentScannerDll = scanner.empty() ? std::filesystem::path{} : (sdkRoot / scanner);
    outConfig.DetectBase = "ComponentBase";

    if (outConfig.DotnetExe.empty() || outConfig.ComponentScannerDll.empty())
        Logger::Log::Info("[NativeScripting] SDK manifest has no scanner; user DLLs build in macros-only mode");

    return true;
}

std::filesystem::path ResolveDotnetExecutable(const std::string& recordedPath)
{
    std::error_code ec;
    if (!recordedPath.empty() && std::filesystem::exists(recordedPath, ec))
        return recordedPath;

    // DOTNET_ROOT is the .NET host's own install-root override. On machines
    // where PATH lacks dotnet (agent/CI shells, Finder/Dock launches) it is
    // the one explicit signal, so it outranks the PATH scan.
    if (const char* dotnetRoot = std::getenv("DOTNET_ROOT"); dotnetRoot && *dotnetRoot)
    {
#if defined(_WIN32)
        const std::filesystem::path candidate = std::filesystem::path(dotnetRoot) / "dotnet.exe";
#else
        const std::filesystem::path candidate = std::filesystem::path(dotnetRoot) / "dotnet";
#endif
        if (std::filesystem::exists(candidate, ec))
            return candidate;
    }

    if (std::filesystem::path found = FindExecutableOnPath("dotnet"); !found.empty())
        return found;

#if defined(_WIN32)
    // %ProgramFiles% first (handles non-C: system drives), literal fallbacks
    // for stripped-down environments that lack the env vars.
    for (const char* envName : {"ProgramFiles", "ProgramFiles(x86)"})
        if (const char* programFiles = std::getenv(envName); programFiles && *programFiles)
        {
            const std::filesystem::path candidate =
                std::filesystem::path(programFiles) / "dotnet" / "dotnet.exe";
            if (std::filesystem::exists(candidate, ec))
                return candidate;
        }
    const std::vector<const char*> commonLocations = {
        "C:\\Program Files\\dotnet\\dotnet.exe",
        "C:\\Program Files (x86)\\dotnet\\dotnet.exe",
    };
#else
    const std::vector<const char*> commonLocations = {
        "/usr/local/share/dotnet/dotnet",
        "/opt/homebrew/bin/dotnet",
        "/usr/local/bin/dotnet",
    };
#endif
    for (const char* loc : commonLocations)
        if (std::filesystem::exists(loc, ec))
            return std::filesystem::path(loc);

    return {};
}

} // namespace NativeScripting
} // namespace GameEngine
