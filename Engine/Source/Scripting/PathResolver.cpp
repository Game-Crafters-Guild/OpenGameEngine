#include "Scripting/PathResolver.h"
#include "Core/Application.h" // PathUtils
#include <vector>

namespace GameEngine
{
namespace ScriptingPaths
{

namespace
{
std::filesystem::path FirstExisting(const std::vector<std::filesystem::path>& cands)
{
    for (const auto& p : cands)
    {
        if (std::filesystem::exists(p))
            return p;
    }
    return {};
}

bool DirectoryHasNativeLibrary(const std::filesystem::path& dir)
{
    using std::filesystem::path;
    if (dir.empty())
        return false;

    std::vector<path> cands;
#if defined(_WIN32)
    cands.emplace_back(dir / "GameEngine.Native.dll");
#elif defined(__APPLE__)
    cands.emplace_back(dir / "GameEngine.Native.dylib");
    cands.emplace_back(dir / "libGameEngine.Native.dylib");
#else
    // Linux and other Unix-like platforms: CMake sets OUTPUT_NAME to
    // "GameEngine.Native", which results in libGameEngine.Native.so.
    cands.emplace_back(dir / "libGameEngine.Native.so");
    cands.emplace_back(dir / "GameEngine.Native.so");
#endif

    for (const auto& p : cands)
    {
        if (std::filesystem::exists(p))
            return true;
    }
    return false;
}
} // namespace

std::filesystem::path ResolveEngineManagedDirectoryFrom(const std::filesystem::path& exeDir)
{
    const std::filesystem::path bundleResources = PathUtils::GetBundleResourcesDirectory(exeDir);
    if (!bundleResources.empty())
    {
        std::error_code ec;
        const std::filesystem::path managedDir = bundleResources / "Managed";
        if (std::filesystem::is_directory(managedDir, ec))
            return managedDir;
    }
    return exeDir;
}

std::filesystem::path ResolveEngineManagedDirectory()
{
    return ResolveEngineManagedDirectoryFrom(PathUtils::GetExecutableDirectory());
}

std::filesystem::path ResolveCoreBridgeDll(const std::filesystem::path& preferred)
{
    const std::filesystem::path candidate = ResolveEngineManagedDirectory() / kCoreBridgeAssemblyFileName;
    if (std::filesystem::exists(candidate))
        return candidate;
    return preferred;
}

std::filesystem::path ResolveHotReloadDll()
{
    const std::filesystem::path candidate = ResolveEngineManagedDirectory() / kHotReloadAssemblyFileName;
    if (std::filesystem::exists(candidate))
        return candidate;
    return {};
}

std::filesystem::path ResolveScriptsAssemblyDirectory()
{
    // Scripts assembly OUTPUT directory: <exe>/ScriptAssemblies
    return PathUtils::GetScriptsAssemblyDirectory();
}

std::filesystem::path ResolveNativeLibraryDirectoryFrom(const std::filesystem::path& exeDir,
                                                        const std::filesystem::path& currentDir)
{
    using std::filesystem::path;

    // 1) Prefer the executable directory when it already contains the native shim.
    if (DirectoryHasNativeLibrary(exeDir))
        return exeDir;

    // 1b) macOS bundle layout: prefer Contents/Frameworks when present.
#if defined(__APPLE__)
    if (!exeDir.empty())
    {
        path frameworksDir = (exeDir / ".." / "Frameworks").lexically_normal();
        if (DirectoryHasNativeLibrary(frameworksDir))
            return frameworksDir;
    }
#endif

    // 2) Common test layout: exe is under bin/<config>/Tests while GameEngine.Native
    //    lives one level up under bin/<config>.
    if (!exeDir.empty())
    {
        path parent = exeDir.parent_path();
        if (!parent.empty() && parent != exeDir && DirectoryHasNativeLibrary(parent))
            return parent;
    }

    // 3) During local development some hosts may copy the native shim next to the
    //    working directory; respect that when present.
    if (DirectoryHasNativeLibrary(currentDir))
        return currentDir;

    // 4) Conservative fallback: prefer the executable directory, even if the
    //    library is currently missing. This keeps the result stable and
    //    discoverable in diagnostics.
    if (!exeDir.empty())
        return exeDir;
    return currentDir;
}

std::filesystem::path ResolveNativeLibraryDirectory()
{
    return ResolveNativeLibraryDirectoryFrom(PathUtils::GetExecutableDirectory(), std::filesystem::current_path());
}

std::filesystem::path ResolveNativeLibraryPath()
{
    return ResolveNativeLibraryPathFrom(PathUtils::GetExecutableDirectory(), std::filesystem::current_path());
}

std::filesystem::path ResolveNativeLibraryPathFrom(const std::filesystem::path& exeDir,
                                                   const std::filesystem::path& currentDir)
{
    using std::filesystem::path;

    path dir = ResolveNativeLibraryDirectoryFrom(exeDir, currentDir);
    if (dir.empty())
    {
        return {};
    }

    std::vector<path> candidates;

#if defined(_WIN32)
    candidates.emplace_back(dir / "GameEngine.Native.dll");
#elif defined(__APPLE__)
    candidates.emplace_back(dir / "GameEngine.Native.dylib");
    candidates.emplace_back(dir / "libGameEngine.Native.dylib");
#else
    // Linux and other Unix-like platforms
    candidates.emplace_back(dir / "libGameEngine.Native.so");
    candidates.emplace_back(dir / "GameEngine.Native.so");
#endif

    return FirstExisting(candidates);
}

std::filesystem::path ResolveScriptsRuntimeConfig()
{
    // CoreBridge's runtimeconfig first, then HotReload's, then an optional Scripts one.
    // The Scripts candidate is spelled out rather than taken from
    // ResolveScriptsAssemblyDirectory(): that resolver creates the directory as a side
    // effect, and a probe must not write to the executable directory.
    const std::filesystem::path managedDir = ResolveEngineManagedDirectory();
    const std::vector<std::filesystem::path> cands = {
        managedDir / "GameEngine.CoreBridge.runtimeconfig.json",
        managedDir / "GameEngine.HotReload.runtimeconfig.json",
        PathUtils::GetExecutableDirectory() / "ScriptAssemblies" / "GameEngine.Scripts.runtimeconfig.json"};
    return FirstExisting(cands);
}

} // namespace ScriptingPaths
} // namespace GameEngine
