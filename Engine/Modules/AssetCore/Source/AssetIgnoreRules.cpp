#include "AssetCore/AssetIgnoreRules.h"
#include "AssetCore/PathNormalization.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace GameEngine
{
namespace
{

// Unicode case-fold + NFC normalize, used for both ignore-rule storage and
// path-side matching so a non-ASCII rule (e.g. "Café/") matches a non-ASCII
// folder (e.g. "café/") regardless of the case the user typed. Folds onto
// the same canonical form as the asset registry's path keys (Phase 0a),
// which is what made this consistency fix necessary in the first place.
//
// Strict-improvement vs. ToLowerAscii: identical output for ASCII (the
// common case) but correct for non-ASCII filenames where ASCII tolower
// would leave bytes untouched and produce mismatches between rule and path.
static std::string CaseFoldForMatch(std::string_view s)
{
    return AssetPaths::CaseFoldUtf8(s);
}

static std::string NormalizeCanonicalPath(std::string_view in)
{
    // Normalize to forward slashes, remove leading "./", collapse duplicate slashes, trim trailing slashes.
    std::string s;
    s.reserve(in.size());
    for (char c : in)
    {
        if (c == '\\')
            s.push_back('/');
        else
            s.push_back(c);
    }

    // Strip leading "./"
    while (s.size() >= 2 && s[0] == '.' && s[1] == '/')
    {
        s.erase(s.begin(), s.begin() + 2);
    }

    // Strip leading '/'
    while (!s.empty() && s.front() == '/')
    {
        s.erase(s.begin());
    }

    // Collapse '//' (simple pass)
    for (;;)
    {
        auto pos = s.find("//");
        if (pos == std::string::npos)
            break;
        s.erase(pos, 1);
    }

    // Remove trailing slashes
    while (!s.empty() && s.back() == '/')
    {
        s.pop_back();
    }

    return s;
}

static std::string NormalizeIgnoreRule(std::string_view in)
{
    // Similar to NormalizeCanonicalPath, but preserves a trailing '/' so rules like "DirName/" keep meaning.
    std::string s;
    s.reserve(in.size());
    for (char c : in)
    {
        if (c == '\\')
            s.push_back('/');
        else
            s.push_back(c);
    }

    // Strip leading "./"
    while (s.size() >= 2 && s[0] == '.' && s[1] == '/')
    {
        s.erase(s.begin(), s.begin() + 2);
    }

    // Strip leading '/'
    while (!s.empty() && s.front() == '/')
    {
        s.erase(s.begin());
    }

    // Collapse '//' (simple pass)
    for (;;)
    {
        auto pos = s.find("//");
        if (pos == std::string::npos)
            break;
        s.erase(pos, 1);
    }

    return s;
}

static std::string TryGetCanonicalRelativeNoThrow(const std::filesystem::path& assetRoot,
                                                  const std::filesystem::path& absOrRel)
{
    std::error_code ec;
    std::filesystem::path abs = absOrRel;
    if (abs.is_relative())
    {
        abs = (assetRoot / abs).lexically_normal();
    }

    std::filesystem::path rel = std::filesystem::relative(abs, assetRoot, ec);
    if (ec)
    {
        // Fallback to best-effort normalized string; this is primarily for logging/debugging
        // and should not crash.
        return NormalizeCanonicalPath(abs.generic_string());
    }

    return NormalizeCanonicalPath(rel.generic_string());
}

static bool StartsWith(std::string_view s, std::string_view prefix)
{
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

// One directory-segment test for both the recursion check and the path check, so a
// name rule and a name-prefix rule mean the same thing wherever a segment is read.
// `foldedSegment` has already been through CaseFoldForMatch.
static bool IsIgnoredDirectorySegment(const AssetIgnoreRules& rules, const std::string& foldedSegment)
{
    if (rules.ignoredDirNamesLower.find(foldedSegment) != rules.ignoredDirNamesLower.end())
        return true;

    for (const std::string& namePrefix : rules.ignoredDirNamePrefixesLower)
    {
        if (StartsWith(foldedSegment, namePrefix))
            return true;
    }

    return false;
}

} // namespace

AssetIgnoreRules AssetIgnoreRules::CreateDefault()
{
    AssetIgnoreRules r;

    // Directories
    const char* dirs[] = {
        ".git",
        ".cache",   // some tools use lowercase
        ".cache/",  // defensive
        ".cache\\", // defensive
        ".cache ",
        ".cache.",
        ".cache_",
        ".cache-",
        ".cache+",
        ".cache~",
        ".Cache",
        ".MyEngine",
        ".Editor",
        "Generated",
        "CMakeFiles", // a CMake build tree's own sources (CMakeCXXCompilerId.cpp defines main)
        ".vcpkg",
        "Library",
        "build",
        "out",
        "bin",
        "obj",
        "node_modules",
        "__pycache__",
        "site-packages",
        ".venv",
        "venv",
        "__MACOSX"};

    for (const char* d : dirs)
    {
        std::string s = d;
        // Strip accidental trailing slashes/backslashes/spaces from the static list.
        while (!s.empty() && (s.back() == '/' || s.back() == '\\' || s.back() == ' '))
            s.pop_back();
        if (!s.empty())
            r.ignoredDirNamesLower.insert(CaseFoldForMatch(s));
    }

    // Directory-name prefixes. CLion writes one build tree per configuration
    // beside the sources it builds ("cmake-build-debug", "cmake-build-release").
    r.ignoredDirNamePrefixesLower.push_back(CaseFoldForMatch("cmake-build-"));

    // Files
    const char* files[] = {
        "AssetDatabase.assetdb", // authoritative DB
        ".assetmanifest",        // published stored-identity manifest (lives inside Assets/)
        ".assetignore",          // per-project ignore file
        "CMakeLists.txt",        // build definition, not content
        ".ds_store",
        "thumbs.db",
        "desktop.ini",
        "pyvenv.cfg"};

    for (const char* f : files)
    {
        r.ignoredFileNamesLower.insert(CaseFoldForMatch(f));
    }

    // Extensions. Build/tooling project files are never assets: a
    // user-authored .csproj inside the asset root belongs to the scripting
    // system, which watches it through its own FileWatcher, not the asset
    // registry.
    const char* exts[] = {
        ".meta",
        ".tmp",
        ".pyc",
        ".pyo",
        ".pyd",
        ".csproj",
        ".sln",
        ".slnx",
        ".vcxproj"};

    for (const char* e : exts)
    {
        std::string ext = CaseFoldForMatch(e);
        if (!ext.empty() && ext[0] != '.')
            ext.insert(ext.begin(), '.');
        r.ignoredExtensionsLower.insert(ext);
    }

    return r;
}

AssetIgnoreRules AssetIgnoreRules::LoadForAssetRoot(const std::filesystem::path& assetRoot)
{
    AssetIgnoreRules r = CreateDefault();

    const std::filesystem::path ignoreFile = assetRoot / ".assetignore";
    std::error_code ec;
    if (!std::filesystem::exists(ignoreFile, ec))
    {
        return r;
    }

    std::ifstream in(ignoreFile);
    if (!in.is_open())
    {
        return r;
    }

    std::string line;
    while (std::getline(in, line))
    {
        // Strip CR
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        // Trim leading whitespace
        size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
            ++start;
        if (start >= line.size())
            continue;
        if (line[start] == '#')
            continue;

        // Trim trailing whitespace
        size_t end = line.size();
        while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t'))
            --end;
        if (end <= start)
            continue;

        std::string rule = line.substr(start, end - start);

        // Normalize slashes and leading '/' but preserve trailing '/' markers.
        rule = NormalizeIgnoreRule(rule);
        if (rule.empty())
            continue;

        // Prefix ignore: "prefix/**"
        if (rule.size() >= 3 && rule.ends_with("/**"))
        {
            std::string prefix = rule.substr(0, rule.size() - 3);
            prefix = NormalizeCanonicalPath(prefix);
            if (!prefix.empty())
            {
                prefix = CaseFoldForMatch(prefix);
                if (!prefix.ends_with('/'))
                    prefix.push_back('/');
                r.ignoredPrefixesLower.push_back(std::move(prefix));
            }
            continue;
        }

        // Directory rule: "DirName/" OR "Some/Prefix/"
        if (rule.ends_with('/'))
        {
            rule.pop_back();
            rule = NormalizeCanonicalPath(rule);
            if (rule.empty())
                continue;

            if (rule.find('/') == std::string::npos)
            {
                r.ignoredDirNamesLower.insert(CaseFoldForMatch(rule));
            }
            else
            {
                std::string prefix = CaseFoldForMatch(rule);
                if (!prefix.ends_with('/'))
                    prefix.push_back('/');
                r.ignoredPrefixesLower.push_back(std::move(prefix));
            }
            continue;
        }

        // Extension ignore: "*.ext" or ".ext"
        if (rule.starts_with("*.") || rule.starts_with('.'))
        {
            std::string ext = rule.starts_with("*.") ? rule.substr(1) : rule;
            ext = CaseFoldForMatch(ext);
            if (!ext.empty() && ext[0] != '.')
                ext.insert(ext.begin(), '.');
            r.ignoredExtensionsLower.insert(std::move(ext));
            continue;
        }

        // Fallback: treat as prefix (exact directory/file prefix)
        {
            std::string prefix = CaseFoldForMatch(rule);
            if (!prefix.ends_with('/'))
                prefix.push_back('/');
            r.ignoredPrefixesLower.push_back(std::move(prefix));
        }
    }

    // Keep prefixes deterministic and efficient to scan.
    std::sort(r.ignoredPrefixesLower.begin(), r.ignoredPrefixesLower.end());
    r.ignoredPrefixesLower.erase(
        std::unique(r.ignoredPrefixesLower.begin(), r.ignoredPrefixesLower.end()),
        r.ignoredPrefixesLower.end());

    return r;
}

void AssetIgnoreRules::IgnorePackageTestsFolder(const std::filesystem::path& sourceRoot,
                                                const std::filesystem::path& packageRoot)
{
    if (packageRoot.empty())
        return;
    const std::filesystem::path testsFolder = (packageRoot / "Tests").lexically_normal();
    const std::filesystem::path relative = testsFolder.lexically_relative(sourceRoot.lexically_normal());
    if (relative.empty() || *relative.begin() == "..")
        return;

    std::string prefix = CaseFoldForMatch(NormalizeCanonicalPath(relative.generic_string()));
    prefix.push_back('/');
    ignoredPrefixesLower.push_back(std::move(prefix));
    std::sort(ignoredPrefixesLower.begin(), ignoredPrefixesLower.end());
    ignoredPrefixesLower.erase(std::unique(ignoredPrefixesLower.begin(), ignoredPrefixesLower.end()),
                               ignoredPrefixesLower.end());
}

bool AssetIgnoreRules::ShouldIgnoreCanonicalRelativePath(std::string_view canonicalRel) const
{
    if (canonicalRel.empty())
        return true;

    const std::string norm = CaseFoldForMatch(NormalizeCanonicalPath(canonicalRel));
    if (norm.empty())
        return true;

    const std::filesystem::path p(norm);
    const std::string filename = CaseFoldForMatch(p.filename().string());

    if (!filename.empty() && filename[0] == '.')
        return true;

    if (ignoredFileNamesLower.find(filename) != ignoredFileNamesLower.end())
        return true;

    const std::string ext = CaseFoldForMatch(p.extension().string());
    if (!ext.empty())
    {
        if (ignoredExtensionsLower.find(ext) != ignoredExtensionsLower.end())
            return true;
    }

    // Directory segment ignore
    for (const auto& seg : p.parent_path())
    {
        const std::string s = CaseFoldForMatch(seg.string());
        if (!s.empty() && IsIgnoredDirectorySegment(*this, s))
            return true;
    }

    // Prefix ignore
    for (const auto& pref : ignoredPrefixesLower)
    {
        if (!pref.empty() && StartsWith(norm, pref))
            return true;
    }

    return false;
}

bool AssetIgnoreRules::ShouldIgnoreDirectory(const std::filesystem::path& directoryPath,
                                            const std::filesystem::path& assetRoot) const
{
    // Quick check by directory name.
    const std::string nameLower = CaseFoldForMatch(directoryPath.filename().string());
    if (!nameLower.empty() && nameLower[0] == '.')
        return true;
    if (!nameLower.empty() && IsIgnoredDirectorySegment(*this, nameLower))
        return true;

    // Prefix ignore (root-relative)
    const std::string relLower = CaseFoldForMatch(TryGetCanonicalRelativeNoThrow(assetRoot, directoryPath));
    if (!relLower.empty())
    {
        std::string relWithSlash = relLower;
        if (!relWithSlash.ends_with('/'))
            relWithSlash.push_back('/');

        for (const auto& pref : ignoredPrefixesLower)
        {
            if (!pref.empty() && StartsWith(relWithSlash, pref))
                return true;
        }
    }

    return false;
}

bool AssetIgnoreRules::ShouldIgnoreFile(const std::filesystem::path& filePath,
                                       const std::filesystem::path& assetRoot) const
{
    const std::string rel = TryGetCanonicalRelativeNoThrow(assetRoot, filePath);
    return ShouldIgnoreCanonicalRelativePath(rel);
}

uint64_t AssetIgnoreRules::Signature() const
{
    // FNV-1a 64 over a deterministic, sorted, type-prefixed serialization
    // of every rule. Type prefix prevents a rule that happens to match
    // across categories (e.g. an extension "*.txt" colliding with a prefix
    // "*.txt/") from producing identical signatures.
    constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
    constexpr uint64_t kFnvPrime  = 0x00000100000001b3ull;

    auto fold = [](uint64_t h, std::string_view s) noexcept
    {
        for (unsigned char c : s)
        {
            h ^= static_cast<uint64_t>(c);
            h *= kFnvPrime;
        }
        return h;
    };

    auto sortedFold = [&](uint64_t h, char prefix, const std::unordered_set<std::string>& set) noexcept
    {
        std::vector<std::string_view> sorted;
        sorted.reserve(set.size());
        for (const auto& s : set)
            sorted.emplace_back(s);
        std::sort(sorted.begin(), sorted.end());
        for (auto sv : sorted)
        {
            h ^= static_cast<uint64_t>(static_cast<unsigned char>(prefix));
            h *= kFnvPrime;
            h = fold(h, sv);
            // 0-byte sentinel between entries so "ab"+"c" doesn't hash like "a"+"bc".
            h *= kFnvPrime;
        }
        return h;
    };

    uint64_t h = kFnvOffset;
    h = sortedFold(h, 'D', ignoredDirNamesLower);
    h = sortedFold(h, 'F', ignoredFileNamesLower);
    h = sortedFold(h, 'E', ignoredExtensionsLower);

    // ignoredPrefixesLower is already a vector; preserve its existing order
    // so the signature is stable across sessions that read the same .assetignore
    // (rule order matters for prefix shadowing).
    h ^= static_cast<uint64_t>('P');
    h *= kFnvPrime;
    for (const auto& p : ignoredPrefixesLower)
    {
        h = fold(h, p);
        h *= kFnvPrime;
    }

    // Directory-name prefixes, same treatment: a vector whose order is its own.
    h ^= static_cast<uint64_t>('N');
    h *= kFnvPrime;
    for (const auto& p : ignoredDirNamePrefixesLower)
    {
        h = fold(h, p);
        h *= kFnvPrime;
    }

    // Reserve 0 as "no signature recorded".
    return h == 0 ? 1 : h;
}

} // namespace GameEngine

