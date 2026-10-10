#include "Engine/Build/WebExportRunner.h"

#include "Core/Application.h"
#include "Engine/Build/BuildPipeline.h"
#include "Engine/Build/GameConfig.h"
#include "Engine/GameUI/UIScaleProjectSettings.h"

#include <cstdlib>
#include <sstream>
#include <vector>

namespace GameEngine {
namespace {


/// Files a directory must hold to be a usable wasm Player template. The shell
/// is expanded into WebPlayer.html at link time, so a template is exactly the
/// three artifacts the emscripten link produces.
constexpr const char* kPlayerTemplateFiles[] = {"WebPlayer.html", "WebPlayer.js", "WebPlayer.wasm"};

/// wasm build trees the engine's own presets produce, newest-first by
/// preference: a Release template ships smaller, a Debug one is what a
/// developer usually has.
constexpr const char* kWasmPresetBinDirs[] = {"build/wasm-release/bin", "build/wasm-debug/bin"};

/// First existing file at `relative` under `start` or any of its ancestors.
///
/// The walk runs to the filesystem root rather than a fixed depth: the staged
/// Editor sits nine directories inside a build tree on macOS (bundle Contents,
/// Apps/Editor, bin/<config>, the preset directory) and fewer elsewhere, and a
/// depth guess that is right on one platform silently finds nothing on another.
std::filesystem::path FindFileUpward(std::filesystem::path start,
                                     const std::filesystem::path& relative)
{
    std::error_code ec;
    while (!start.empty())
    {
        const auto candidate = start / relative;
        if (std::filesystem::is_regular_file(candidate, ec))
            return candidate;
        const auto parent = start.parent_path();
        if (parent == start)
            break;
        start = parent;
    }
    return {};
}

std::string Quote(const std::filesystem::path& path)
{
    return "\"" + path.generic_string() + "\"";
}

/// Locate Tools/Web/export_web_player.py: the project root first (a project
/// living inside an engine tree), then upwards from the Editor executable.
/// Both walks leave the staged output on purpose: the script reads engine and
/// package shader sources from the repository it sits in, so Web export is a
/// development feature that runs only against an engine source tree.
std::filesystem::path FindExportScript(const std::filesystem::path& projectRoot)
{
    const std::filesystem::path relative = std::filesystem::path("Tools") / "Web" / "export_web_player.py";

    if (!projectRoot.empty())
    {
        const auto inProject = FindFileUpward(projectRoot, relative);
        if (!inProject.empty())
            return inProject;
    }
    return FindFileUpward(PathUtils::GetExecutableDirectory(), relative);
}

std::filesystem::path FindPythonExecutable()
{
#if defined(_WIN32)
    const char* names[] = {"python.exe", "python3.exe"};
    const char kPathSeparator = ';';
#else
    const char* names[] = {"python3", "python"};
    const char kPathSeparator = ':';
#endif

    std::vector<std::filesystem::path> searchDirs;
#if defined(__APPLE__)
    // A bundled Editor launched from Finder inherits a minimal PATH, so the
    // usual package-manager prefixes are searched explicitly.
    searchDirs.emplace_back("/opt/homebrew/bin");
    searchDirs.emplace_back("/usr/local/bin");
    searchDirs.emplace_back("/usr/bin");
#endif
    if (const char* pathEnv = std::getenv("PATH"))
    {
        std::stringstream stream(pathEnv);
        std::string entry;
        while (std::getline(stream, entry, kPathSeparator))
        {
            if (!entry.empty())
                searchDirs.emplace_back(entry);
        }
    }

    std::error_code ec;
    for (const char* name : names)
    {
        for (const auto& dir : searchDirs)
        {
            const auto candidate = dir / name;
            if (std::filesystem::is_regular_file(candidate, ec))
                return candidate;
        }
    }
    return {};
}

/// MaterialVariantCook is staged into <bin>/Tools by ge_set_output_tools, which
/// is a sibling of the Apps/ tree the Editor is staged into rather than the
/// Editor's own directory.
std::filesystem::path FindMaterialVariantCook()
{
#if defined(_WIN32)
    const std::filesystem::path relative = std::filesystem::path("Tools") / "MaterialVariantCook.exe";
#else
    const std::filesystem::path relative = std::filesystem::path("Tools") / "MaterialVariantCook";
#endif
    return FindFileUpward(PathUtils::GetExecutableDirectory(), relative);
}

bool IsPlayerTemplateDir(const std::filesystem::path& directory)
{
    if (directory.empty())
        return false;
    std::error_code ec;
    for (const char* file : kPlayerTemplateFiles)
    {
        if (!std::filesystem::is_regular_file(directory / file, ec))
            return false;
    }
    return true;
}

/// Resolve the wasm Player template, recording every location tried so a
/// failure can say where it looked.
std::filesystem::path FindPlayerTemplate(const std::filesystem::path& projectRoot,
                                         const std::filesystem::path& engineRoot,
                                         const std::filesystem::path& configured,
                                         std::vector<std::filesystem::path>& outTried)
{
    if (!configured.empty())
    {
        std::filesystem::path path = configured;
        if (path.is_relative() && !projectRoot.empty())
            path = projectRoot / path;
        outTried.push_back(path);
        return IsPlayerTemplateDir(path) ? path : std::filesystem::path{};
    }

    if (!engineRoot.empty())
    {
        for (const char* binDir : kWasmPresetBinDirs)
        {
            const auto candidate = engineRoot / binDir;
            outTried.push_back(candidate);
            if (IsPlayerTemplateDir(candidate))
                return candidate;
        }
    }
    return {};
}

std::string DescribePaths(const std::vector<std::filesystem::path>& paths)
{
    std::string joined;
    for (const auto& path : paths)
    {
        if (!joined.empty())
            joined += ", ";
        joined += path.generic_string();
    }
    return joined.empty() ? std::string("nowhere — no engine tree found") : joined;
}

} // namespace

bool RunWebExport(const BuildSettings& settings,
                  BuildPipeline& pipeline,
                  const WebExportInputs& inputs,
                  const std::function<void(const std::string& line)>& onLine,
                  std::string& outError)
{
    if (settings.projectRoot.empty())
    {
        outError = "Web export needs an open project. Open a project, then build again.";
        return false;
    }
    if (settings.outputDirectory.empty() || settings.outputDirectory == settings.projectRoot)
    {
        outError = "Web export output directory resolves to the project root, and the export "
                   "replaces that directory. Set a build folder in Settings > Build > Web.";
        return false;
    }

    const auto exportScript = FindExportScript(settings.projectRoot);
    if (exportScript.empty())
    {
        outError = "Web export script not found (Tools/Web/export_web_player.py). Web export is a "
                   "development feature: it runs from an engine source tree and a packaged Editor "
                   "does not ship it. Run an Editor built from the engine repo, or place the "
                   "project inside that repo.";
        return false;
    }
    // <engine root>/Tools/Web/export_web_player.py
    const auto engineRoot = exportScript.parent_path().parent_path().parent_path();

    const auto python = FindPythonExecutable();
    if (python.empty())
    {
        outError = "Web export needs Python 3, which was not found on PATH. Install Python 3 "
                   "(brew install python on macOS, python.org on Windows) and restart the Editor.";
        return false;
    }

    std::vector<std::filesystem::path> templatesTried;
    const auto playerTemplate = FindPlayerTemplate(settings.projectRoot, engineRoot, inputs.playerTemplate, templatesTried);
    if (playerTemplate.empty())
    {
        outError = "No wasm Player template found (looked in: " + DescribePaths(templatesTried) +
                   "). Build one with: Tools/emsdk/setup.sh && cmake --preset wasm-debug && "
                   "cmake --build --preset wasm-debug --target WebPlayer — then point "
                   "Settings > Build > Web > Player template at the directory holding "
                   "WebPlayer.html, WebPlayer.js and WebPlayer.wasm.";
        return false;
    }

    const auto materialCook = FindMaterialVariantCook();
    if (materialCook.empty())
    {
        outError = "MaterialVariantCook was not found next to this Editor. Build it with: "
                   "cmake --build <build dir> --target MaterialVariantCook.";
        return false;
    }

    const auto shaderPackages = inputs.shaderPackagesRoot;
    std::error_code ec;
    if (!std::filesystem::is_directory(shaderPackages, ec))
    {
        outError = "This Editor has no staged shader packages at " + shaderPackages.generic_string() +
                   ", which the Web export cooks to WGSL. Rebuild the Editor so its assets are staged.";
        return false;
    }

    // The project directory holds no game.config; the export needs one, and the
    // Editor's build settings are what a desktop package would be built from.
    const auto generatedConfig = settings.outputDirectory.parent_path() / ".web-export-game.config";
    std::filesystem::create_directories(generatedConfig.parent_path(), ec);
    GameConfig config = settings.playerConfig;
    config.uiScale = UIScaleProjectSettings::Load(settings.projectRoot);
    if (!SaveGameConfig(generatedConfig, config))
    {
        outError = "Failed to write the generated game.config at " + generatedConfig.generic_string() +
                   ". Check that the build folder is writable.";
        return false;
    }

    std::string command;
#if !defined(_WIN32)
    // The cook shells out to glslc; a Finder-launched Editor inherits a PATH
    // without the usual package-manager prefixes.
    command = "PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:$PATH ";
#endif
    command += Quote(python) + " " + Quote(exportScript) +
               " --project " + Quote(settings.projectRoot) +
               " --game-config " + Quote(generatedConfig) +
               " --out " + Quote(settings.outputDirectory) +
               " --template " + Quote(playerTemplate) +
               " --shaderpkg-dir " + Quote(shaderPackages) +
               " --material-cook " + Quote(materialCook) +
               " 2>&1";

    std::string lastLine;
    const ShellProcessResult result = pipeline.RunShellCommand(command, [&](const std::string& line) {
        if (!line.empty())
            lastLine = line;
        if (onLine)
            onLine(line);
    });

    std::filesystem::remove(generatedConfig, ec);

    if (result.cancelled || pipeline.IsCancelled())
    {
        outError = "Web export cancelled";
        return false;
    }
    if (result.exitCode != 0)
    {
        outError = "Web export failed";
        if (!lastLine.empty())
            outError += ": " + lastLine;
        return false;
    }
    return true;
}

} // namespace GameEngine
