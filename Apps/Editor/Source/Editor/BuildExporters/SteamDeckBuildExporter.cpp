#include "Editor/BuildExporters/SteamDeckBuildExporter.h"

#include "Editor/Registries/BuildExporterRegistry.h"
#include "Editor/EditorPaths.h"
#include "Editor/Settings/BuildRenderPipelineSettings.h"
#include "Editor/Settings/SettingsStore.h"
#include "Core/Application.h"
#include "Engine/Build/BuildPipeline.h"
#include "Engine/Build/GameConfig.h"
#include "Engine/Build/LinuxRuntimeTemplate.h"
#include "Engine/Build/ShellUtil.h"
#include "Logger/Logger.h"
#include "Types/StringUtils.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{
namespace
{

// The stored platform name is "Steam"; the debug/MCP surface also accepts the
// display-name spellings.
bool IsSteamDeckPlatform(std::string_view platformName)
{
    const std::string platform = ToLowerAscii(platformName);
    return platform == "steam" || platform == "steam deck" || platform == "steamdeck";
}

std::filesystem::path FindSteamDeckBuildScript(const std::filesystem::path& workspaceRoot)
{
    std::error_code ec;
    std::vector<std::filesystem::path> candidates;
    if (!workspaceRoot.empty())
        candidates.push_back(workspaceRoot / "Tools" / "Scripts" / "build-steamdeck-docker.sh");

    std::filesystem::path cursor = PathUtils::GetExecutableDirectory();
    for (int i = 0; i < 8 && !cursor.empty(); ++i)
    {
        candidates.push_back(cursor / "Tools" / "Scripts" / "build-steamdeck-docker.sh");
        candidates.push_back(cursor.parent_path() / "Tools" / "Scripts" / "build-steamdeck-docker.sh");
        cursor = cursor.parent_path();
    }

    for (const auto& candidate : candidates)
    {
        if (std::filesystem::exists(candidate, ec))
            return candidate;
    }
    return {};
}

std::filesystem::path FindSteamDeckDeployScript(const std::filesystem::path& workspaceRoot)
{
    std::error_code ec;
    std::vector<std::filesystem::path> candidates;
    if (!workspaceRoot.empty())
        candidates.push_back(workspaceRoot / "Tools" / "Scripts" / "deploy-steamdeck.sh");

    std::filesystem::path cursor = PathUtils::GetExecutableDirectory();
    for (int i = 0; i < 8 && !cursor.empty(); ++i)
    {
        candidates.push_back(cursor / "Tools" / "Scripts" / "deploy-steamdeck.sh");
        candidates.push_back(cursor.parent_path() / "Tools" / "Scripts" / "deploy-steamdeck.sh");
        cursor = cursor.parent_path();
    }

    for (const auto& candidate : candidates)
    {
        if (std::filesystem::exists(candidate, ec))
            return candidate;
    }
    return {};
}

void ApplyExecutablePermissions(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::permissions(path,
                                 std::filesystem::perms::owner_exec |
                                     std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::add,
                                 ec);
}

bool CopyDirectoryRecursivePreservingModes(const std::filesystem::path& src,
                                           const std::filesystem::path& dst,
                                           const std::function<bool(const std::filesystem::path&)>& skipRootEntry = {},
                                           const std::function<bool()>& shouldCancel = nullptr)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(src, ec))
        return false;

    std::filesystem::create_directories(dst, ec);
    if (ec)
        return false;

    for (const auto& entry : std::filesystem::directory_iterator(src, ec))
    {
        if (shouldCancel && shouldCancel())
            return false;
        if (ec)
            return false;
        if (skipRootEntry && skipRootEntry(entry.path()))
            continue;

        const auto target = dst / entry.path().filename();
        if (entry.is_directory(ec))
        {
            if (!CopyDirectoryRecursivePreservingModes(entry.path(), target, {}, shouldCancel))
                return false;
        }
        else if (entry.is_regular_file(ec))
        {
            std::filesystem::create_directories(target.parent_path(), ec);
            if (ec)
                return false;
            std::filesystem::copy_file(entry.path(), target, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec)
                return false;

            ec.clear();
            std::filesystem::permissions(target, entry.status(ec).permissions(),
                                         std::filesystem::perm_options::replace, ec);
        }
    }
    return true;
}

// The Steam Deck export scripts, staged next to the editor by the editor build.
std::filesystem::path SteamDeckToolsDirectory()
{
    return GetEditorGlobalPaths().installToolsRoot / "SteamDeck";
}

// The fingerprint of the engine sources this editor was built from, which a
// cached Deck runtime must match (Tools/Scripts/deck_template.py). Empty when
// the editor build could not compute it.
std::string ReadEngineSourceIdentity()
{
    std::ifstream stamp(SteamDeckToolsDirectory() / "engine-source.fingerprint", std::ios::binary);
    std::string identity;
    std::getline(stamp, identity);
    return identity;
}

bool WriteSteamDeckLaunchScript(const std::filesystem::path& packageDir)
{
    std::error_code ec;
    const auto scriptPath = packageDir / "launch.sh";
    std::filesystem::copy_file(SteamDeckToolsDirectory() / "steamdeck-launch.sh", scriptPath,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
    {
        Logger::Log::Error("Build: Failed to stage Deck launcher: {}", ec.message());
        return false;
    }
    ApplyExecutablePermissions(scriptPath);
    return true;
}

bool WriteSteamDeckIconAndDesktop(const std::filesystem::path& packageDir,
                                  const std::filesystem::path& editorSdkDir,
                                  const std::string& displayName)
{
    const auto iconSrc = editorSdkDir / "AppIcon.png";
    const auto iconDst = packageDir / "icon.png";
    std::error_code ec;
    std::filesystem::copy_file(iconSrc, iconDst, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
    {
        Logger::Log::Warning("Build: Steam Deck icon not copied from {}: {}", iconSrc.string(), ec.message());
        return false;
    }

    const auto desktopPath = packageDir / "gameengine-player.desktop";
    std::ofstream desktop(desktopPath, std::ios::binary | std::ios::trunc);
    if (!desktop)
        return false;

    // Version= is the Desktop Entry Specification version the file follows, not the game's
    // or the engine's version.
    desktop << "[Desktop Entry]\n"
            << "Type=Application\n"
            << "Version=1.0\n"
            << "Name=" << displayName << "\n"
            << "Comment=Open Engine Player\n"
            << "Exec=\"__INSTALL_DIR__/" << displayName << "\"\n"
            << "Path=__INSTALL_DIR__\n"
            << "Icon=__INSTALL_DIR__/icon.png\n"
            << "Terminal=false\n"
            << "Categories=Game;\n"
            << "StartupWMClass=Player\n";

    const auto launcherPath = packageDir / displayName;
    std::ofstream launcher(launcherPath, std::ios::binary | std::ios::trunc);
    if (!launcher)
        return false;
    launcher << "#!/usr/bin/env bash\n"
             << "exec \"$(dirname \"$0\")/launch.sh\" \"$@\"\n";
    ApplyExecutablePermissions(launcherPath);
    return true;
}

bool RunBuildShellCommand(BuildPipeline& pipeline,
                          const std::string& command,
                          std::string& lastLine,
                          const std::function<void(const std::string& line)>& onLine = nullptr)
{
    ShellProcessResult result = pipeline.RunShellCommand(
        command,
        [&](const std::string& line) {
            if (!line.empty())
            {
                lastLine = line;
                Logger::Log::Info("Build: {}", line);
            }
            if (onLine)
                onLine(line);
        });

    return !result.cancelled && result.exitCode == 0;
}

std::filesystem::path SteamDeckDefaultBuildOutputDir()
{
#if defined(__APPLE__)
    if (const char* home = std::getenv("HOME"))
        return std::filesystem::path(home) / "Library" / "Application Support" / "GameEngine" / "Build" / "SteamDeck";
#elif defined(_WIN32)
    if (const char* appData = std::getenv("APPDATA"))
        return std::filesystem::path(appData) / "GameEngine" / "Build" / "SteamDeck";
#else
    if (const char* xdgDataHome = std::getenv("XDG_DATA_HOME"))
        return std::filesystem::path(xdgDataHome) / "GameEngine" / "Build" / "SteamDeck";
    if (const char* home = std::getenv("HOME"))
        return std::filesystem::path(home) / ".local" / "share" / "GameEngine" / "Build" / "SteamDeck";
#endif
    return {};
}

std::filesystem::path SteamDeckSharedTemplateCacheDir()
{
#if defined(__APPLE__)
    if (const char* home = std::getenv("HOME"))
        return std::filesystem::path(home) / "Library" / "Application Support" / "GameEngine" / "ExportTemplates" / "SteamDeck" / "steamdeck-player";
#elif defined(_WIN32)
    if (const char* appData = std::getenv("APPDATA"))
        return std::filesystem::path(appData) / "GameEngine" / "ExportTemplates" / "SteamDeck" / "steamdeck-player";
#else
    if (const char* xdgDataHome = std::getenv("XDG_DATA_HOME"))
        return std::filesystem::path(xdgDataHome) / "GameEngine" / "ExportTemplates" / "SteamDeck" / "steamdeck-player";
    if (const char* home = std::getenv("HOME"))
        return std::filesystem::path(home) / ".local" / "share" / "GameEngine" / "ExportTemplates" / "SteamDeck" / "steamdeck-player";
#endif
    return {};
}

std::filesystem::path SteamDeckLegacyProjectTemplateCacheDir(const std::filesystem::path& projectRoot)
{
    return projectRoot / ".Editor" / "ExportTemplates" / "SteamDeck" / "steamdeck-player";
}

bool IsValidSteamDeckExportTemplate(const std::filesystem::path& candidate)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(candidate / "Player", ec) &&
           std::filesystem::is_regular_file(candidate / "runtime-template.json", ec);
}

std::filesystem::path FindSteamDeckExportTemplate(const BuildSettings& settings,
                                                  const std::filesystem::path& hostDistDir,
                                                  BuildPipeline& pipeline)
{
    const std::string identity = ReadEngineSourceIdentity();
    if (identity.empty())
    {
        Logger::Log::Warning("Build: This editor build has no engine source identity; the Deck runtime will be rebuilt");
        return {};
    }

    std::vector<std::filesystem::path> candidates;
    if (const char* envTemplateDir = std::getenv("GE_STEAMDECK_TEMPLATE_DIR"))
    {
        if (*envTemplateDir)
            candidates.emplace_back(envTemplateDir);
    }
    const auto sharedTemplateDir = SteamDeckSharedTemplateCacheDir();
    if (!sharedTemplateDir.empty())
        candidates.push_back(sharedTemplateDir);
    if (!settings.projectRoot.empty())
    {
        candidates.push_back(SteamDeckLegacyProjectTemplateCacheDir(settings.projectRoot));
        candidates.push_back(settings.projectRoot / ".Editor" / "ExportTemplates" / "SteamDeck" / "steamdeck-player-template");
    }
    candidates.push_back(hostDistDir / "steamdeck-player");

    for (const auto& candidate : candidates)
    {
        if (IsValidSteamDeckExportTemplate(candidate))
        {
            std::string lastLine;
            const std::string verify = "python3 " + ShellQuote((SteamDeckToolsDirectory() / "deck_template.py").string()) +
                                       " verify --identity " + ShellQuote(identity) +
                                       " --package " + ShellQuote(candidate.string()) + " 2>&1";
            if (RunBuildShellCommand(pipeline, verify, lastLine))
                return candidate;
            Logger::Log::Warning("Build: Deck template rejected: {}", lastLine);
        }
    }
    return {};
}

bool InstallSteamDeckExportTemplateFromBuild(const std::filesystem::path& builtPackageDir)
{
    const auto cacheDir = SteamDeckSharedTemplateCacheDir();
    if (cacheDir.empty() || !IsValidSteamDeckExportTemplate(builtPackageDir))
        return false;

    std::error_code ec;
    std::filesystem::remove_all(cacheDir, ec);
    ec.clear();
    std::filesystem::create_directories(cacheDir.parent_path(), ec);
    if (ec)
        return false;

    return CopyDirectoryRecursivePreservingModes(
        builtPackageDir,
        cacheDir,
        &IsRuntimeTemplateGamePayload);
}

bool ExecuteSteamDeckTemplateExport(const BuildSettings& settings,
                                    const std::filesystem::path& hostDistDir,
                                    const std::filesystem::path& templateDir,
                                    BuildPipeline& pipeline,
                                    BuildPipeline::ProgressCallback progress)
{
    if (pipeline.IsCancelled())
        return false;

    // Keep runtime selection in the editor, but use the engine's one packaging
    // pipeline for assets, code, GUID identity, package mounts and game.config.
    BuildSettings exportSettings = settings;
    exportSettings.prebuiltPlayerDirectory = templateDir;
    exportSettings.outputDirectory = hostDistDir / ".steamdeck-export-staging" / "steamdeck-player";
    exportSettings.renderPipelinePath = ResolveBuildRenderPipelineForPlatform("Steam");
    exportSettings.playerConfig.renderPipeline = exportSettings.renderPipelinePath;
    exportSettings.playerConfig.windowWidth = 1280;
    exportSettings.playerConfig.windowHeight = 800;
    if (!pipeline.Execute(exportSettings, std::move(progress)))
        return false;

    const auto packageDir = exportSettings.outputDirectory;
    if (!WriteSteamDeckLaunchScript(packageDir))
        return false;
    WriteSteamDeckIconAndDesktop(packageDir, settings.editorSDKPath, settings.playerConfig.gameName);
    ApplyExecutablePermissions(packageDir / "Player");

    // Archive the candidate before replacing the existing package. A failure
    // in packaging leaves the previous deployable build intact.
    std::string lastLine;
    const auto stagingRoot = packageDir.parent_path();
    const std::string archive = "tar -czf " + ShellQuote((stagingRoot / "steamdeck-player.tar.gz").string()) +
                                " -C " + ShellQuote(stagingRoot.string()) + " steamdeck-player 2>&1";
    if (!RunBuildShellCommand(pipeline, archive, lastLine))
        return false;
    const std::string promote = "python3 " + ShellQuote((SteamDeckToolsDirectory() / "build_deck.py").string()) +
                                " --promote " + ShellQuote(stagingRoot.string()) +
                                " --output " + ShellQuote(hostDistDir.string()) + " 2>&1";
    return RunBuildShellCommand(pipeline, promote, lastLine);
}

bool RelativePathStartsWithParentTraversal(const std::filesystem::path& rel)
{
    auto it = rel.begin();
    return it != rel.end() && it->string() == "..";
}

std::string ToDockerSrcPath(const std::filesystem::path& hostPath,
                            const std::filesystem::path& repoRoot)
{
    std::error_code ec;
    auto absHostPath = std::filesystem::weakly_canonical(hostPath, ec);
    if (ec)
    {
        ec.clear();
        absHostPath = std::filesystem::absolute(hostPath, ec);
    }
    if (ec)
        return {};

    ec.clear();
    auto absRepoRoot = std::filesystem::weakly_canonical(repoRoot, ec);
    if (ec)
    {
        ec.clear();
        absRepoRoot = std::filesystem::absolute(repoRoot, ec);
    }
    if (ec)
        return {};

    ec.clear();
    const auto rel = std::filesystem::relative(absHostPath, absRepoRoot, ec);
    if (ec || rel.empty() || RelativePathStartsWithParentTraversal(rel))
        return {};

    return (std::filesystem::path("/src") / rel).generic_string();
}

// The "Steam Deck install" rows of the Steam Deck build settings page
// (build.platform.Steam.*), read when the build starts so the values the
// page shows are the values the install uses.
struct SteamDeckInstallSettings
{
    bool AutoInstallAfterBuild = false;
    std::string SshHost;
    std::string SshUser = "deck";
    std::filesystem::path SshKeyPath;
    std::filesystem::path RemoteDirectory;
    bool SteamShortcut = true;
};

SteamDeckInstallSettings LoadSteamDeckInstallSettings(const std::filesystem::path& projectRoot)
{
    SteamDeckInstallSettings install;
    auto prefs = projectRoot.empty() ? OpenEditorPreferences() : OpenProjectSettings(projectRoot);
    std::string err;
    prefs.Load(&err);
    prefs.TryGetBool("build.platform.Steam.autoInstallAfterBuild", install.AutoInstallAfterBuild);
    prefs.TryGetString("build.platform.Steam.installSshHost", install.SshHost);
    prefs.TryGetString("build.platform.Steam.installSshUser", install.SshUser);
    std::string sshKeyPath;
    if (prefs.TryGetString("build.platform.Steam.installSshKeyPath", sshKeyPath))
        install.SshKeyPath = sshKeyPath;
    std::string remoteDirectory;
    if (prefs.TryGetString("build.platform.Steam.installRemoteDirectory", remoteDirectory))
        install.RemoteDirectory = remoteDirectory;
    prefs.TryGetBool("build.platform.Steam.installSteamShortcut", install.SteamShortcut);
    return install;
}

bool InstallOnSteamDeckAfterBuild(const BuildSettings& settings, BuildPipeline& pipeline,
                                  const BuildExporterDescriptor::ProgressCallback& progress)
{
    const SteamDeckInstallSettings install = LoadSteamDeckInstallSettings(settings.projectRoot);
    if (!install.AutoInstallAfterBuild)
        return true;

    if (pipeline.IsCancelled())
        return false;

    const auto script = FindSteamDeckDeployScript(settings.projectRoot);
    if (script.empty())
    {
        Logger::Log::Error("Build: Steam Deck deploy script not found");
        progress(BuildProgress{BuildProgress::Stage::Failed, 0.0f,
                               "Steam Deck deploy script not found."});
        return false;
    }

    std::filesystem::path packageDir = settings.outputDirectory / "steamdeck-player";
    if (packageDir.is_relative() && !settings.projectRoot.empty())
        packageDir = settings.projectRoot / packageDir;

    if (!std::filesystem::is_directory(packageDir))
    {
        Logger::Log::Error("Build: Steam Deck package folder not found: {}", packageDir.string());
        progress(BuildProgress{BuildProgress::Stage::Failed, 0.0f,
                               "Steam Deck package folder not found."});
        return false;
    }

    if (install.SshHost.empty())
    {
        Logger::Log::Error("Build: Steam Deck host is empty. Set it in Build Settings > Steam Deck.");
        progress(BuildProgress{BuildProgress::Stage::Failed, 0.0f,
                               "Steam Deck host is not configured."});
        return false;
    }

    std::string command = "PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:$PATH " +
                          ShellQuote(script.string()) +
                          " --source " + ShellQuote(packageDir.string()) +
                          " --host " + ShellQuote(install.SshHost);
    if (!install.SshUser.empty())
        command += " --user " + ShellQuote(install.SshUser);
    if (!install.SshKeyPath.empty())
        command += " --ssh-key " + ShellQuote(install.SshKeyPath.string());
    if (!install.RemoteDirectory.empty())
        command += " --remote-dir " + ShellQuote(install.RemoteDirectory.string());
    if (!settings.playerConfig.gameName.empty())
        command += " --app-name " + ShellQuote(settings.playerConfig.gameName);
    if (!install.SteamShortcut)
        command += " --no-shortcut";
    command += " 2>&1";

    Logger::Log::Info("Build: Steam Deck auto install: {}", command);
    progress(BuildProgress{BuildProgress::Stage::Assembling, 0.95f,
                           "Installing build on Steam Deck..."});

    std::string lastLine;
    if (!RunBuildShellCommand(pipeline, command, lastLine))
    {
        if (pipeline.IsCancelled())
        {
            progress(BuildProgress{BuildProgress::Stage::Failed, 0.95f, "Build cancelled", {}, {}, true});
            return false;
        }

        std::string message = "Steam Deck install failed";
        if (!lastLine.empty())
            message += ": " + lastLine;
        Logger::Log::Error("Build: {}", message);
        progress(BuildProgress{BuildProgress::Stage::Failed, 0.0f, message});
        return false;
    }

    progress(BuildProgress{BuildProgress::Stage::Complete, 1.0f,
                           "Steam Deck install complete."});
    return true;
}

// macOS cannot run BuildPipeline's Linux compile phases, so the Deck package is
// assembled from a cached export template when one exists and built in Docker
// otherwise.
bool RunMacHostedLinuxBuild(const BuildSettings& settings, BuildPipeline& pipeline,
                            const BuildExporterDescriptor::ProgressCallback& progress)
{
    if (pipeline.IsCancelled())
        return false;

    const auto reportCancelled = [&progress]() {
        progress(BuildProgress{BuildProgress::Stage::Failed, 0.0f, "Build cancelled", {}, {}, true});
    };

    std::filesystem::path hostDistDir = settings.outputDirectory.empty()
                                            ? SteamDeckDefaultBuildOutputDir()
                                            : settings.outputDirectory;
    if (hostDistDir.is_relative() && !settings.projectRoot.empty())
        hostDistDir = settings.projectRoot / hostDistDir;
    if (hostDistDir.empty())
    {
        Logger::Log::Error("Build: No Steam Deck output directory");
        progress(BuildProgress{BuildProgress::Stage::Failed, 0.0f, "No Steam Deck output directory."});
        return false;
    }

    progress(BuildProgress{BuildProgress::Stage::PreparingProject, 0.05f,
                                  "Looking for Steam Deck export template..."});
    const auto templateDir = FindSteamDeckExportTemplate(settings, hostDistDir, pipeline);
    if (!templateDir.empty())
    {
        const bool ok = ExecuteSteamDeckTemplateExport(settings, hostDistDir, templateDir, pipeline,
            progress);
        BuildProgress result{};
        if (ok)
            result = BuildProgress{BuildProgress::Stage::Complete, 1.0f, "Steam Deck export complete"};
        else
            result = BuildProgress{BuildProgress::Stage::Failed, 0.0f, "Steam Deck packaging failed; see the build log"};
        progress(result);
        return ok;
    }

    if (pipeline.IsCancelled())
    {
        reportCancelled();
        return false;
    }

    Logger::Log::Info("Build: No Steam Deck export template found; falling back to Docker player build");

    // The Docker build compiles the engine, so it runs from an engine checkout.
    const auto script = FindSteamDeckBuildScript(settings.projectRoot);
    if (script.empty())
    {
        Logger::Log::Error("Build: Steam Deck Docker build script not found");
        progress(BuildProgress{BuildProgress::Stage::Failed, 0.0f,
                               "No matching Steam Deck runtime is cached, and rebuilding it needs the engine "
                               "checkout's Tools/Scripts/build-steamdeck-docker.sh."});
        return false;
    }
    const auto repoRoot = script.parent_path().parent_path().parent_path();
    std::error_code ec;

    // A runtime rebuild is not yet a completed game export. Keep it away from
    // the last working game until the common packaging pipeline has succeeded.
    const auto runtimeDistDir = hostDistDir / ".runtime-build";
    std::filesystem::path containerDistDir = "/output";
    ec.clear();
    const auto rel = std::filesystem::relative(runtimeDistDir, repoRoot, ec);
    if (!ec && !rel.empty() && !RelativePathStartsWithParentTraversal(rel))
        containerDistDir = rel;

    const std::string startupScene = !settings.playerConfig.startupScene.empty()
                                         ? settings.playerConfig.startupScene
                                         : (settings.scenes.empty() ? std::string() : settings.scenes[0]);
    std::string assetRoot = ToDockerSrcPath(settings.projectRoot / "Assets", repoRoot);
    if (assetRoot.empty() && !settings.projectRoot.empty())
        assetRoot = "/project/Assets";

    const std::string scriptArg;
    std::string command = "cd " + ShellQuote(repoRoot.string()) +
                          " && PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:$PATH";
    auto appendEnv = [&command](const char* name, const std::string& value) {
        command += " ";
        command += name;
        command += "=";
        command += ShellQuote(value);
    };
    appendEnv("GE_STEAMDECK_PROJECT_ROOT", settings.projectRoot.string());
    appendEnv("GE_STEAMDECK_HOST_DIST_DIR", runtimeDistDir.string());
    appendEnv("GE_STEAMDECK_CONTAINER_DIST_DIR", containerDistDir.generic_string());
    appendEnv("GE_STEAMDECK_ASSET_ROOT", assetRoot);
    appendEnv("GE_STEAMDECK_STARTUP_SCENE", startupScene);
    appendEnv("GE_STEAMDECK_RENDER_PIPELINE", ResolveBuildRenderPipelineForPlatform("Steam"));
    appendEnv("GE_STEAMDECK_GAME_NAME", settings.playerConfig.gameName);
    command += " " + ShellQuote(script.string()) + scriptArg + " 2>&1";

    Logger::Log::Info("Build: macOS hosted Steam Deck build: {}", command);
    progress(BuildProgress{BuildProgress::Stage::CompilingPlayer, 0.10f,
                                  "Starting Steam Deck build with Docker..."});

    std::string lastLine;
    const bool dockerOk = RunBuildShellCommand(
        pipeline,
        command,
        lastLine,
        [&progress](const std::string& line) {
            if (!line.empty())
                progress(BuildProgress{BuildProgress::Stage::CompilingPlayer, 0.50f, line});
        });

    if (!dockerOk)
    {
        if (pipeline.IsCancelled())
        {
            reportCancelled();
            return false;
        }

        std::ostringstream oss;
        oss << "Steam Deck Docker build failed";
        if (!lastLine.empty())
            oss << ": " << lastLine;
        Logger::Log::Error("Build: {}", oss.str());
        progress(BuildProgress{BuildProgress::Stage::Failed, 0.0f, oss.str()});
        return false;
    }

    if (!InstallSteamDeckExportTemplateFromBuild(runtimeDistDir / "steamdeck-player"))
    {
        progress(BuildProgress{BuildProgress::Stage::Failed, 0.0f, "Failed to cache the built Deck runtime"});
        return false;
    }
    const bool ok = ExecuteSteamDeckTemplateExport(settings, hostDistDir,
        SteamDeckSharedTemplateCacheDir(), pipeline,
        progress);
    BuildProgress result{};
    if (ok)
        result = BuildProgress{BuildProgress::Stage::Complete, 1.0f, "Steam Deck export complete"};
    else
        result = BuildProgress{BuildProgress::Stage::Failed, 0.0f, "Steam Deck packaging failed; see the build log"};
    progress(result);
    return ok;
}

bool UseMacHostedLinuxBuild()
{
#if defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

// The macOS export writes the deployable package into a folder inside the
// output directory; every other host builds the game into it directly.
std::filesystem::path SteamDeckOutputDirectory(const BuildSettings& settings)
{
    return UseMacHostedLinuxBuild() ? settings.outputDirectory / "steamdeck-player" : settings.outputDirectory;
}

bool RunSteamDeckBuild(const BuildSettings& settings, BuildPipeline& pipeline,
                       const BuildExporterDescriptor::ProgressCallback& progress)
{
    const bool built = UseMacHostedLinuxBuild() ? RunMacHostedLinuxBuild(settings, pipeline, progress)
                                                : pipeline.Execute(settings, progress);
    if (!built || pipeline.IsCancelled())
        return false;
    return InstallOnSteamDeckAfterBuild(settings, pipeline, progress);
}

} // namespace

void RegisterSteamDeckBuildExporter()
{
    BuildExporterDescriptor exporter;
    exporter.ExporterId = "steamDeck";
    exporter.DisplayName = "Steam Deck build";
    exporter.HandlesPlatform = &IsSteamDeckPlatform;
    exporter.Run = &RunSteamDeckBuild;
    exporter.OutputDirectory = &SteamDeckOutputDirectory;
    BuildExporterRegistry::Get().Register(std::move(exporter));
}

} // namespace GameEngine::Editor
