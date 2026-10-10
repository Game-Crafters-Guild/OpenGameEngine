#include "Core/Application.h"
#include "Logger/Logger.h"
#if defined(__EMSCRIPTEN__)
#include "Platform/WebPersistentStorage.h"
#endif
#include <cstdlib> // getenv
#include <filesystem>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <Windows.h> // For GetModuleFileNameW and related Win32 APIs
#include <ShlObj.h>  // SHGetKnownFolderPath
#else
#include <fstream>
#include <limits.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace GameEngine
{

std::filesystem::path PathUtils::GetExecutableDirectory()
{
    static std::filesystem::path cachedPath;

    // Return cached path if already computed
    if (!cachedPath.empty())
    {
        return cachedPath;
    }

#ifdef _WIN32
    // Windows implementation
    wchar_t exePath[MAX_PATH];
    DWORD result = GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    if (result == 0 || result == MAX_PATH)
    {
        Logger::Log::Error("Failed to get executable path on Windows");
        cachedPath = std::filesystem::current_path(); // Fallback
        return cachedPath;
    }

    std::filesystem::path fullPath(exePath);
    cachedPath = fullPath.parent_path();

#elif defined(__APPLE__)
    // macOS implementation using _NSGetExecutablePath
    char exePath[PATH_MAX];
    uint32_t size = static_cast<uint32_t>(sizeof(exePath));

    if (_NSGetExecutablePath(exePath, &size) != 0)
    {
        Logger::Log::Error("Failed to get executable path on macOS");
        cachedPath = std::filesystem::current_path(); // Fallback
        return cachedPath;
    }

    // Resolve any symlinks and get the real path
    std::error_code ec;
    auto canonicalPath = std::filesystem::canonical(exePath, ec);
    if (ec)
    {
        Logger::Log::Warning(
            "Failed to canonicalize executable path on macOS: {}", ec.message());
        std::filesystem::path fullPath(exePath);
        cachedPath = fullPath.parent_path();
    }
    else
    {
        cachedPath = canonicalPath.parent_path();
    }

#elif defined(__EMSCRIPTEN__)
    // There is no executable file on the web. The dist unpacks into the MEMFS
    // root with the same next-to-exe layout desktop staging produces (/Assets,
    // /Packages, /.Cache), so "/" IS the executable directory. Must not
    // fall through to the cwd fallback: EngineCore chdirs to the workspace
    // root during Initialize, which would make this cache the project dir.
    cachedPath = "/";

#else
    // Linux (and other Unix-like systems with /proc/self/exe)
    char exePath[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);

    if (len == -1)
    {
        Logger::Log::Error("Failed to get executable path on Unix");
        cachedPath = std::filesystem::current_path(); // Fallback
        return cachedPath;
    }

    exePath[len] = '\0';
    std::filesystem::path fullPath(exePath);
    cachedPath = fullPath.parent_path();
#endif

    Logger::Log::Debug("Executable directory: {}", cachedPath.string());
    return cachedPath;
}

namespace
{
// "<dir>/" and "<dir>" name the same directory, but only the second has the filename the
// bundle shape below compares, so callers may spell a directory either way.
std::filesystem::path NormalizeDirectory(const std::filesystem::path& directory)
{
    std::filesystem::path normalized = directory.lexically_normal();
    if (normalized.filename().empty())
        normalized = normalized.parent_path();
    return normalized;
}
} // namespace

std::filesystem::path PathUtils::GetBundleResourcesDirectory(const std::filesystem::path& executableDirectory)
{
#if defined(__APPLE__)
    const std::filesystem::path exeDir = NormalizeDirectory(executableDirectory);
    if (exeDir.filename() != "MacOS")
        return {};
    const std::filesystem::path contentsDir = exeDir.parent_path();
    if (contentsDir.filename() != "Contents")
        return {};
    return contentsDir / "Resources";
#else
    (void)executableDirectory;
    return {};
#endif
}

std::filesystem::path PathUtils::GetInstallAssetsRoot()
{
    // The executable does not move while it runs, so the layout rule is evaluated once; every
    // later call is a copy of the answer.
    static const std::filesystem::path installAssetsRoot = InstallAssetsRootFor(GetExecutableDirectory());
    return installAssetsRoot;
}

std::filesystem::path PathUtils::InstallAssetsRootFor(const std::filesystem::path& executableDirectory)
{
    const std::filesystem::path contentRoot = InstallContentRootFor(executableDirectory);
    return contentRoot.empty() ? contentRoot : contentRoot / "Assets";
}

std::filesystem::path PathUtils::InstallContentRootFor(const std::filesystem::path& executableDirectory)
{
    const std::filesystem::path exeDir = NormalizeDirectory(executableDirectory);
    if (exeDir.empty())
    {
        return {};
    }

    // Both app bundles (Editor and Player, and every game exported from the Player) stage
    // their data as resources. Outside a bundle the primitive is empty: every test executable
    // shares bin/<Config>/Tests, and a directory another target staged beside that tree must
    // not become this executable's content root.
    const std::filesystem::path bundleResources = GetBundleResourcesDirectory(exeDir);
    return bundleResources.empty() ? exeDir : bundleResources;
}

std::filesystem::path PathUtils::GetScriptsAssemblyDirectory()
{
    // Scripts assembly OUTPUT directory: <exe>/ScriptAssemblies
    auto exeDir = GetExecutableDirectory();
    auto outDir = exeDir / "ScriptAssemblies";

    // Ensure the directory exists (best-effort)
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (ec)
    {
        Logger::Log::Warning("Failed to create scripts assembly output directory: {} - {}", outDir.string(), ec.message());
    }

    return outDir;
}

void PathUtils::EnsureWorkingDirectoryMatchesExecutable()
{
    EnsureWorkingDirectoryMatches(GetExecutableDirectory());
}

void PathUtils::EnsureWorkingDirectoryMatches(const std::filesystem::path& directory)
{
    namespace fs = std::filesystem;

    if (directory.empty())
    {
        return;
    }

    std::error_code ec;

    fs::path target = directory;
    if (!target.is_absolute())
    {
        fs::path abs = fs::absolute(target, ec);
        if (!ec)
        {
            target = abs;
        }
        ec.clear();
    }

    target = target.lexically_normal();

    fs::path current = fs::current_path(ec);
    if (ec)
    {
        ec.clear();
        current.clear();
    }

    if (!current.empty() && current == target)
    {
        Logger::Log::Debug("Working directory already matches requested location");
        return;
    }

    Logger::Log::Info("Working directory mismatch detected:");
    if (!current.empty())
    {
        Logger::Log::Info("  Current: {}", current.string());
    }
    Logger::Log::Info("  Desired: {}", target.string());
    Logger::Log::Info("Setting working directory to match desired location...");

    fs::current_path(target, ec);
    if (ec)
    {
        Logger::Log::Error("Failed to set working directory: {}", ec.message());
    }
    else
    {
        Logger::Log::Info("✅ Working directory updated to: {}", target.string());
    }
}

static std::filesystem::path NormalizeUserDir(std::filesystem::path p)
{
    namespace fs = std::filesystem;
    if (p.empty())
        return p;

    std::error_code ec;
    if (!p.is_absolute())
    {
        p = fs::absolute(p, ec);
        ec.clear();
    }

    // Avoid canonical() here to prevent IO and failures on non-existent paths; callers can create dirs.
    return p.lexically_normal();
}

#if defined(__EMSCRIPTEN__)
// User data and caches live under the persistent (OPFS) projects mount —
// $HOME here is emscripten's MEMFS /home/web_user, which dies with the tab,
// and a second OPFS mount would alias the projects tree (see
// Platform/WebPersistentStorage.h). Creation is gated on the mount existing:
// create_directories called before WebMain mounts OPFS would plant a MEMFS
// directory at the mount point and poison the mount itself.
static std::filesystem::path WebPersistentDir(const char* root)
{
    std::filesystem::path p(root);
    std::error_code ec;
    if (std::filesystem::is_directory(p.parent_path(), ec))
        std::filesystem::create_directories(p, ec);
    return p;
}
#endif

std::filesystem::path PathUtils::GetUserCacheDirectory()
{
    namespace fs = std::filesystem;
    std::error_code ec;

#if defined(__EMSCRIPTEN__)
    return WebPersistentDir(Platform::Web::kUserCacheDir);
#elif defined(_WIN32)
    // Prefer LocalAppData for caches (non-roaming). Fall back to AppData if needed.
    if (const char* p = std::getenv("LOCALAPPDATA"); p && *p)
        return NormalizeUserDir(fs::path(p));
    if (const char* p = std::getenv("APPDATA"); p && *p)
        return NormalizeUserDir(fs::path(p));
    return NormalizeUserDir(fs::temp_directory_path(ec));
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home)
        return NormalizeUserDir(fs::path(home) / "Library" / "Caches");
    return NormalizeUserDir(fs::temp_directory_path(ec));
#else
    if (const char* p = std::getenv("XDG_CACHE_HOME"); p && *p)
        return NormalizeUserDir(fs::path(p));
    if (const char* home = std::getenv("HOME"); home && *home)
        return NormalizeUserDir(fs::path(home) / ".cache");
    return NormalizeUserDir(fs::temp_directory_path(ec));
#endif
}

std::filesystem::path PathUtils::GetUserDataDirectory()
{
    namespace fs = std::filesystem;
    std::error_code ec;

#if defined(__EMSCRIPTEN__)
    return WebPersistentDir(Platform::Web::kUserDataDir);
#elif defined(_WIN32)
    // Prefer roaming AppData for user data/settings. Fall back to LocalAppData.
    if (const char* p = std::getenv("APPDATA"); p && *p)
        return NormalizeUserDir(fs::path(p));
    if (const char* p = std::getenv("LOCALAPPDATA"); p && *p)
        return NormalizeUserDir(fs::path(p));
    return NormalizeUserDir(fs::temp_directory_path(ec));
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home)
        return NormalizeUserDir(fs::path(home) / "Library" / "Application Support");
    return NormalizeUserDir(fs::temp_directory_path(ec));
#else
    if (const char* p = std::getenv("XDG_DATA_HOME"); p && *p)
        return NormalizeUserDir(fs::path(p));
    if (const char* home = std::getenv("HOME"); home && *home)
        return NormalizeUserDir(fs::path(home) / ".local" / "share");
    return NormalizeUserDir(fs::temp_directory_path(ec));
#endif
}

std::filesystem::path PathUtils::GetUserDocumentsDirectory()
{
    namespace fs = std::filesystem;
    std::error_code ec;

#if defined(_WIN32)
    // Prefer the Known Folder API (more correct than env vars).
    PWSTR widePath = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &widePath);
    if (SUCCEEDED(hr) && widePath && widePath[0] != L'\0')
    {
        fs::path p(widePath);
        CoTaskMemFree(widePath);
        return NormalizeUserDir(p);
    }
    if (widePath)
    {
        CoTaskMemFree(widePath);
    }

    // Fallback: %USERPROFILE%\\Documents
    if (const char* profile = std::getenv("USERPROFILE"); profile && *profile)
    {
        return NormalizeUserDir(fs::path(profile) / "Documents");
    }
    return NormalizeUserDir(fs::temp_directory_path(ec));
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home)
        return NormalizeUserDir(fs::path(home) / "Documents");
    return NormalizeUserDir(fs::temp_directory_path(ec));
#else
    auto Trim = [](std::string& s)
    {
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
            s.pop_back();
        size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
            ++i;
        if (i > 0)
            s.erase(0, i);
    };

    // Best-effort parse of XDG user-dirs config (user-dirs.dirs) for XDG_DOCUMENTS_DIR.
    // Example line: XDG_DOCUMENTS_DIR="$HOME/Documents"
    auto TryParseXdgDocumentsDir = [&]() -> fs::path
    {
        const char* home = std::getenv("HOME");
        if (!home || !*home)
            return {};

        fs::path configHome;
        if (const char* xdgCfg = std::getenv("XDG_CONFIG_HOME"); xdgCfg && *xdgCfg)
            configHome = fs::path(xdgCfg);
        else
            configHome = fs::path(home) / ".config";

        const fs::path userDirs = (configHome / "user-dirs.dirs").lexically_normal();
        std::ifstream in(userDirs);
        if (!in.is_open())
            return {};

        std::string line;
        while (std::getline(in, line))
        {
            Trim(line);
            if (line.empty() || line[0] == '#')
                continue;

            constexpr const char* kKey = "XDG_DOCUMENTS_DIR=";
            if (line.rfind(kKey, 0) != 0)
                continue;

            std::string rhs = line.substr(std::char_traits<char>::length(kKey));
            Trim(rhs);

            // Expect quoted value.
            if (rhs.size() >= 2 && (rhs.front() == '"' || rhs.front() == '\'') && rhs.back() == rhs.front())
            {
                rhs = rhs.substr(1, rhs.size() - 2);
            }

            // Replace $HOME prefix when present.
            constexpr const char* kHomeVar = "$HOME";
            if (rhs.rfind(kHomeVar, 0) == 0)
            {
                rhs = std::string(home) + rhs.substr(std::char_traits<char>::length(kHomeVar));
            }

            if (rhs.empty())
                return {};

            fs::path p(rhs);
            return NormalizeUserDir(p);
        }
        return {};
    };

    if (fs::path p = TryParseXdgDocumentsDir(); !p.empty())
        return p;

    // Fallback: ~/Documents
    if (const char* home = std::getenv("HOME"); home && *home)
        return NormalizeUserDir(fs::path(home) / "Documents");
    return NormalizeUserDir(fs::temp_directory_path(ec));
#endif
}

std::string PathUtils::SanitizeForFolderName(std::string_view name)
{
    auto isBad = [](unsigned char c) -> bool
    {
        // Windows-forbidden or generally problematic: <>:"/\\|?* and control chars.
        if (c < 32)
            return true;
        switch (c)
        {
            case '<':
            case '>':
            case ':':
            case '"':
            case '/':
            case '\\':
            case '|':
            case '?':
            case '*': return true;
            default: return false;
        }
    };

    std::string out;
    out.reserve(name.size());
    for (char ch : name)
    {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (isBad(c))
        {
            out.push_back('_');
            continue;
        }
        if (ch == '\t' || ch == '\r' || ch == '\n')
        {
            out.push_back(' ');
            continue;
        }
        out.push_back(ch);
    }

    // Trim leading/trailing spaces.
    size_t start = 0;
    while (start < out.size() && out[start] == ' ')
        ++start;
    size_t end = out.size();
    while (end > start && out[end - 1] == ' ')
        --end;
    if (start != 0 || end != out.size())
        out = out.substr(start, end - start);

#if defined(_WIN32)
    // Windows disallows trailing '.' and ' ' in a path segment.
    while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
        out.pop_back();
#endif

    if (out.empty())
        out = "App";
    return out;
}

std::filesystem::path PathUtils::GetBundledShaderCacheRoot()
{
#if defined(__EMSCRIPTEN__)
    // The cook ships in the wasm preload at the MEMFS root.
    const std::filesystem::path bundleCache{"/.Cache/Shaders"};
    std::error_code ec;
    return std::filesystem::is_directory(bundleCache, ec) ? bundleCache : std::filesystem::path{};
#else
    return {};
#endif
}

} // namespace GameEngine
