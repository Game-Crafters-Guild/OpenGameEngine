#include "Engine/Build/RuntimeDependencyStaging.h"

#include "Scripting/PathResolver.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace GameEngine {

namespace {

constexpr RuntimeDependency kEngineRuntimeSet[] = {
    {"Engine", "Engine", true},
    {"GameEngine.Native (C ABI shim)", "GameEngine.Native", true},
    // The loader is a driver/runtime-installed system component (System32);
    // modern LunarG SDKs ship no redistributable vulkan-1.dll, so no build
    // stages one beside the Editor — it only ever appeared there hand-copied.
    // Ship it when present; otherwise the end user's driver-installed loader
    // serves (a machine without one cannot run the game regardless).
    {"Vulkan loader", "vulkan-1", false},
    {"GLFW", "glfw3", true},
    {"FreeType", "freetyped;freetype", true},
    {"HarfBuzz", "harfbuzz", true},
    {"lexbor", "lexbor", true},
    {"pugixml", "pugixml", true},
    {"utf8proc", "utf8proc", true},
    {"tinyexpr", "tinyexpr", true},
    {"ThorVG", "thorvg-*", true},
    {"meshoptimizer", "meshoptimizer", true},
    // KTX2 texture container loading (#550). vcpkg ships ktx.dll for both the
    // release and debug flavors (no 'd' suffix).
    {"KTX (libktx)", "ktx", true},
    // BCn texture cook. Same file name in both vcpkg flavors (no 'd' suffix):
    // source-dir order picks the flavor. The vcpkg dependency is platform-gated
    // (windows | linux | osx), so not every engine imports it — ship when present.
    {"DirectXTex", "DirectXTex", false},
    {"zstd", "zstd", true},
    {"zlib", "zd;z;zlib1", true},
    {"SQLite", "sqlite3", true},
    {"libpng", "libpng16d;libpng16", true},
    {"Brotli (common)", "brotlicommon", true},
    {"Brotli (decoder)", "brotlidec", true},
    {"bzip2", "bz2d;bz2", true},
    {"libcurl", "libcurl-d;libcurl", true},
    {"FFmpeg avutil", "avutil-*", true},
    {"FFmpeg avcodec", "avcodec-*", true},
    {"FFmpeg avformat", "avformat-*", true},
    {"FFmpeg swscale", "swscale-*", true},
    // Present only when the engine builds against the shaderc-shared overlay
    // port (cmake/ports/shaderc) — then Engine.dll imports it, so ship when
    // present; static-shaderc builds ship none. Same file name in both vcpkg
    // flavors: source-dir order picks the flavor.
    {"shaderc", "shaderc_shared", false},
    // Present only in presets that build the engine with the mimalloc override —
    // then the redirect MUST sit beside the executable, so ship when present.
    // Debug name first: cross-flavor debug staging searches the release bin as
    // a fallback dir, so candidate order is what picks the flavor.
    {"mimalloc", "mimalloc-debug;mimalloc", false},
    {"mimalloc redirect", "mimalloc-redirect", false},
};

constexpr RuntimeDependency kManagedHostingSet[] = {
    {".NET nethost", "nethost", true},
    {".NET hostfxr", "hostfxr", true},
};

constexpr const char* kManagedSidecarExtensions[] = {".deps.json", ".runtimeconfig.json", ".xml"};

// Engine ABI surfaces a game's scripts compile against: every GameEngine.*.ABI.dll beside the
// engine except the editor's own (GameEngine.Editor.*), which never ships in a game.
bool IsRuntimeAbiAssembly(std::string_view fileName)
{
    constexpr std::string_view kEnginePrefix = "GameEngine.";
    constexpr std::string_view kEditorPrefix = "GameEngine.Editor.";
    constexpr std::string_view kAbiSuffix = ".ABI.dll";
    return fileName.size() > kEnginePrefix.size() + kAbiSuffix.size() && fileName.starts_with(kEnginePrefix) &&
           fileName.ends_with(kAbiSuffix) && !fileName.starts_with(kEditorPrefix);
}

// Appends the assembly file names a deps.json declares under targets/<framework>/<library>/runtime:
// what the CLR resolves when it loads the assembly the file describes. An absent file declares
// nothing; one that does not parse fails, since the CLR would refuse it at load.
bool ReadDepsJsonRuntimeAssemblies(const fs::path& depsJson, std::vector<std::string>& fileNames,
                                   std::vector<std::string>& errors)
{
    std::ifstream in(depsJson, std::ios::binary);
    if (!in)
        return true;
    const nlohmann::json deps = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
    const auto targets = deps.is_object() ? deps.find("targets") : deps.end();
    if (deps.is_discarded() || !deps.is_object() || targets == deps.end() || !targets->is_object())
    {
        errors.push_back("Malformed managed dependency manifest '" + depsJson.string() +
                         "' (no 'targets' object); rebuild the Editor so it restages its managed assemblies.");
        return false;
    }
    for (const nlohmann::json& libraries : *targets)
    {
        if (!libraries.is_object())
            continue;
        for (const nlohmann::json& library : libraries)
        {
            const auto runtime = library.is_object() ? library.find("runtime") : library.end();
            if (runtime == library.end() || !runtime->is_object())
                continue;
            for (const auto& assembly : runtime->items())
                fileNames.push_back(fs::path(assembly.key()).filename().string());
        }
    }
    return true;
}

// Lowercased stem with any "lib" prefix dropped — applied to files AND candidates
// so the tables stay platform-neutral (Engine.dll vs libEngine.so).
std::string NormalizedDependencyStem(std::string stem)
{
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (stem.rfind("lib", 0) == 0 && stem.size() > 3)
        stem.erase(0, 3);
    return stem;
}

} // namespace

std::span<const RuntimeDependency> EngineRuntimeDependencySet()
{
    return kEngineRuntimeSet;
}

std::span<const RuntimeDependency> ManagedHostingDependencySet()
{
    return kManagedHostingSet;
}

bool StageCuratedDependencySet(const std::vector<fs::path>& sourceDirs, const fs::path& destDir,
                               std::span<const RuntimeDependency> entries,
                               std::vector<std::string>& errors)
{
    struct SharedLibrary
    {
        fs::path Path;
        std::string Stem; // normalized
    };
    // Directory order is priority order: a file name collected from an earlier
    // dir shadows the same name in a later one, for exact and prefix candidates
    // alike.
    std::vector<SharedLibrary> libraries;
    std::unordered_set<std::string> seenFileNames;
    std::error_code ec;
    for (const fs::path& sourceDir : sourceDirs)
    {
        for (const auto& entry : fs::directory_iterator(sourceDir, ec))
        {
            if (!entry.is_regular_file())
                continue;
            const std::string ext = entry.path().extension().string();
            if (ext != ".dll" && ext != ".so" && ext != ".dylib")
                continue;
            if (!seenFileNames.insert(NormalizedDependencyStem(entry.path().filename().string())).second)
                continue;
            libraries.push_back({entry.path(), NormalizedDependencyStem(entry.path().stem().string())});
        }
        ec.clear();
    }

    fs::create_directories(destDir, ec);

    std::string missing;
    for (const RuntimeDependency& dep : entries)
    {
        bool satisfied = false;
        std::istringstream candidates(dep.Candidates);
        std::string candidate;
        while (!satisfied && std::getline(candidates, candidate, ';'))
        {
            const bool prefix = !candidate.empty() && candidate.back() == '*';
            if (prefix)
                candidate.pop_back();
            candidate = NormalizedDependencyStem(std::move(candidate));
            for (const SharedLibrary& lib : libraries)
            {
                const bool matches = prefix ? lib.Stem.rfind(candidate, 0) == 0 : lib.Stem == candidate;
                if (!matches)
                    continue;
                std::error_code copyEc;
                fs::copy_file(lib.Path, destDir / lib.Path.filename(), fs::copy_options::overwrite_existing,
                              copyEc);
                if (copyEc)
                {
                    errors.push_back("Failed to stage runtime dependency '" + lib.Path.filename().string() +
                                     "': " + copyEc.message());
                    return false;
                }
                satisfied = true;
                if (!prefix)
                    break; // exact candidate: one file is the match
            }
        }
        if (!satisfied && dep.Required)
        {
            if (!missing.empty())
                missing += ", ";
            missing += dep.Name;
        }
    }

    if (!missing.empty())
    {
        std::string searched;
        for (const fs::path& sourceDir : sourceDirs)
        {
            if (!searched.empty())
                searched += "; ";
            searched += sourceDir.string();
        }
        errors.push_back("Packaged game is missing required runtime dependencies: " + missing +
                         " (searched: " + searched +
                         "). If the engine's dynamic dependency set changed, update the curated list in "
                         "RuntimeDependencyStaging.cpp.");
        return false;
    }
    return true;
}

bool StageManagedRuntimeAssemblies(const fs::path& sourceDir, const fs::path& destDir,
                                   std::vector<std::string>& errors)
{
    std::error_code ec;
    fs::create_directories(destDir, ec);

    // Work list of file names, each with what asked for it (for the missing-assembly error).
    std::vector<std::pair<std::string, std::string>> pending;
    std::unordered_set<std::string> queued;
    for (const std::string_view root : ScriptingPaths::kHostLoadedEngineAssemblyFileNames)
    {
        queued.emplace(root);
        pending.emplace_back(std::string(root), "loaded by the Player");
    }
    for (const auto& entry : fs::directory_iterator(sourceDir, ec))
    {
        const std::string fileName = entry.path().filename().string();
        if (entry.is_regular_file() && IsRuntimeAbiAssembly(fileName) && queued.insert(fileName).second)
            pending.emplace_back(fileName, "engine ABI surface");
    }

    std::string missing;
    for (size_t i = 0; i < pending.size(); ++i)
    {
        const auto [fileName, requiredBy] = pending[i];
        const fs::path src = sourceDir / fileName;
        if (!fs::exists(src, ec))
        {
            if (!missing.empty())
                missing += ", ";
            missing += fileName + " (" + requiredBy + ")";
            continue;
        }
        std::error_code copyEc;
        fs::copy_file(src, destDir / fileName, fs::copy_options::overwrite_existing, copyEc);
        if (copyEc)
        {
            errors.push_back("Failed to stage managed assembly '" + fileName + "': " + copyEc.message());
            return false;
        }
        for (const char* sidecarExt : kManagedSidecarExtensions)
        {
            fs::path sidecar = src;
            sidecar.replace_extension(sidecarExt);
            if (fs::exists(sidecar, ec))
                fs::copy_file(sidecar, destDir / sidecar.filename(), fs::copy_options::overwrite_existing, copyEc);
        }

        fs::path depsJson = src;
        depsJson.replace_extension(".deps.json");
        std::vector<std::string> dependencies;
        if (!ReadDepsJsonRuntimeAssemblies(depsJson, dependencies, errors))
            return false;
        for (std::string& dependency : dependencies)
        {
            if (queued.insert(dependency).second)
                pending.emplace_back(std::move(dependency), depsJson.filename().string());
        }
    }

    if (!missing.empty())
    {
        errors.push_back("Packaged game has managed scripts, but the engine managed directory '" +
                         sourceDir.string() + "' lacks assemblies the Player loads: " + missing +
                         ". Rebuild the Editor with scripting enabled so it stages them.");
        return false;
    }
    return true;
}

} // namespace GameEngine
