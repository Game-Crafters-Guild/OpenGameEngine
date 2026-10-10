#include "Assets/Packages/PackageGitSource.h"

#include "Assets/Packages/PackageResolver.h"
#include "Logger/Logger.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace GameEngine
{

namespace
{

constexpr std::string_view kGitPlusPrefix = "git+";
constexpr std::string_view kHttpsPrefix = "https://";
constexpr std::string_view kHttpPrefix = "http://";
constexpr std::string_view kPathParam = "&path=";
constexpr size_t kShortShaLength = 12;
constexpr size_t kDerivedNativePrefixLength = 8;

std::uint64_t Fnv1a(std::string_view text)
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char c : text)
    {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string ShortHash(std::uint64_t hash)
{
    char hex[13];
    std::snprintf(hex, sizeof(hex), "%012llx",
                  static_cast<unsigned long long>(hash & 0xFFFFFFFFFFFFULL));
    return hex;
}

} // namespace

bool IsGitPackageSpec(std::string_view spec)
{
    return spec.rfind(kGitPlusPrefix, 0) == 0 || spec.rfind(kHttpsPrefix, 0) == 0 ||
           spec.rfind(kHttpPrefix, 0) == 0;
}

bool TryParseGitPackageSpec(const std::string& name,
                            const std::string& spec,
                            GitPackageSpec& out,
                            std::string& outError)
{
    out = GitPackageSpec{};

    std::string_view rest = spec;
    if (rest.rfind(kGitPlusPrefix, 0) == 0)
        rest.remove_prefix(kGitPlusPrefix.size());

    if (rest.rfind(kHttpsPrefix, 0) != 0 && rest.rfind(kHttpPrefix, 0) != 0)
    {
        outError = "package '" + name + "': git spec '" + spec +
                   "' must use an http(s) clone URL (\"git+https://...\")";
        return false;
    }

    const size_t hash = rest.find('#');
    if (hash == std::string_view::npos || hash + 1 == rest.size())
    {
        outError = "package '" + name + "': git spec '" + spec +
                   "' has no ref — pin one with '#<tag-or-branch-or-sha>'";
        return false;
    }

    std::string_view url = rest.substr(0, hash);
    std::string_view fragment = rest.substr(hash + 1);

    std::string_view ref = fragment;
    std::string_view subdir;
    if (const size_t param = fragment.find(kPathParam); param != std::string_view::npos)
    {
        ref = fragment.substr(0, param);
        subdir = fragment.substr(param + kPathParam.size());
        // Normalize away leading/trailing '/' so downstream path joins are exact.
        while (!subdir.empty() && subdir.front() == '/')
            subdir.remove_prefix(1);
        while (!subdir.empty() && subdir.back() == '/')
            subdir.remove_suffix(1);
        if (subdir.empty())
        {
            outError = "package '" + name + "': git spec '" + spec + "' has an empty &path= value";
            return false;
        }
        if (subdir.find("..") != std::string_view::npos)
        {
            outError = "package '" + name + "': git spec '" + spec +
                       "' path must stay inside the repository ('..' rejected)";
            return false;
        }
    }

    if (url.empty() || url == kHttpsPrefix || url == kHttpPrefix)
    {
        outError = "package '" + name + "': git spec '" + spec + "' has an empty clone URL";
        return false;
    }
    if (ref.empty())
    {
        outError = "package '" + name + "': git spec '" + spec +
                   "' has no ref — pin one with '#<tag-or-branch-or-sha>'";
        return false;
    }
    if (ref.find_first_of(" \t") != std::string_view::npos ||
        url.find_first_of(" \t") != std::string_view::npos)
    {
        outError = "package '" + name + "': git spec '" + spec + "' contains whitespace";
        return false;
    }

    out.Url = std::string(url);
    out.Ref = std::string(ref);
    out.Subdir = std::string(subdir);
    return true;
}

std::filesystem::path GlobalPackageCacheRoot()
{
    if (const char* overrideDir = std::getenv("GE_PACKAGE_CACHE_DIR"); overrideDir && *overrideDir)
        return std::filesystem::path(overrideDir);

#if defined(_WIN32)
    if (const char* localAppData = std::getenv("LOCALAPPDATA"); localAppData && *localAppData)
        return std::filesystem::path(localAppData) / "GameEngine" / "PackageCache";
    // LOCALAPPDATA is set on every interactive Windows session; a service-like
    // context without it falls back to a temp-rooted cache rather than failing.
    return std::filesystem::temp_directory_path() / "GameEngine" / "PackageCache";
#else
    if (const char* xdgCache = std::getenv("XDG_CACHE_HOME"); xdgCache && *xdgCache)
        return std::filesystem::path(xdgCache) / "GameEngine" / "PackageCache";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".cache" / "GameEngine" / "PackageCache";
    return std::filesystem::temp_directory_path() / "GameEngine" / "PackageCache";
#endif
}

std::string PackageCacheEntryName(std::string_view packageName,
                                  std::string_view version,
                                  std::string_view commitSha)
{
    const std::string_view shortSha = commitSha.substr(0, kShortShaLength);
    std::string name = SanitizePackageAlias(packageName);
    name += '@';
    name += version;
    name += '-';
    name += shortSha;
    return name;
}

std::filesystem::path PackageCacheDerivedNativeRoot(const std::filesystem::path& cacheRoot,
                                                    std::string_view entryName,
                                                    std::string_view engineBuildId)
{
    std::filesystem::path nativeRoot = cacheRoot;
    std::string packageIdentity(entryName);
#if defined(_WIN32)
    // Isolated editors can redirect LOCALAPPDATA into a deep directory. MSVC
    // also limits generated object/PDB paths, so shorten the whole build root.
    // Keep the original cache namespace in the package identity: two isolated
    // caches must not share generated projects, build outputs or leases.
    std::error_code error;
    nativeRoot = std::filesystem::temp_directory_path(error);
    if (error)
    {
        Logger::Log::Error("[Packages] cannot resolve TEMP for managed native builds: {}", error.message());
        return {};
    }
    nativeRoot /= "GameEngine";
    packageIdentity = cacheRoot.lexically_normal().generic_string() + '/' + packageIdentity;
#endif
    // Each package owns a separate retention scope, even when names share the
    // readable prefix. The generation beneath it identifies the Engine binary.
    std::string shortName(entryName.substr(0, kDerivedNativePrefixLength));
    shortName += '-';
    shortName += ShortHash(Fnv1a(packageIdentity));

#if defined(__EMSCRIPTEN__)
    // Browser stays on its established, no-lock derived-data layout.
    return nativeRoot / ".derived" / shortName;
#else
    // A generation remains isolated by the exact running Engine binary. A
    // dedicated segment avoids conflating this build identity with the package
    // scope while adding only thirteen characters to the deep CMake path.
    const std::string generation = ShortHash(Fnv1a(engineBuildId));
    return nativeRoot / ".native" / shortName / generation;
#endif
}

} // namespace GameEngine
