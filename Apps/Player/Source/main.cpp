#include "PlayerApplication.h"
#include "PlayerHdrOptions.h"
#include "PlayerLog.h"
#include "Core/EngineLoggerBridge.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "Core/StandardPaths.h"
#include "Engine/Build/GameConfig.h"
#include "Engine/Build/PackagedGameLayout.h"
#include "Logger/Logger.h"
#include "Platform/DiscreteGpuPreference.h"
#include "Platform/Thread.h"
#include "Scripting/NativeAotScriptsLibrary.h"
#include "Scripting/ScriptsConfig.h"
#include "Scripting/ScriptingABI.h"
#include "Scripting/ECSABI.h"
#include "ECS/ECS.h"
#include "ECS/ComponentRegistry.h"
#if GE_PLAYER_MOVIE_RECORDER
#include "Rendering/Passes/TemporalDither.h"
#include "Video/VideoWriter.h"
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#ifdef _WIN32
#  include <windows.h>
#endif

namespace {

void TerminateHandler() noexcept
{
    Logger::Log::Error("std::terminate called");
    try
    {
        auto eptr = std::current_exception();
        if (eptr)
            std::rethrow_exception(eptr);
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("Unhandled exception at terminate: {}", e.what());
    }
    catch (...)
    {
        Logger::Log::Error("Unhandled non-std exception at terminate");
    }
    std::abort();
}

#ifdef _WIN32
LONG WINAPI UnhandledSEHFilter(EXCEPTION_POINTERS* info)
{
    unsigned code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
    Logger::Log::Error("Unhandled SEH exception: 0x{:08X}", code);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

static std::sig_atomic_t gSignalCount = 0;

void SignalHandler(int sig)
{
    if (gSignalCount < 3)
    {
        ++gSignalCount;
        Logger::Log::Error("Caught signal: {}", sig);
    }
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

// The directory the build stages game.config and Assets/ in: the executable's directory, or
// Contents/Resources inside a macOS app bundle.
std::filesystem::path StagedContentRoot()
{
    return GameEngine::PathUtils::InstallContentRootFor(GameEngine::PathUtils::GetExecutableDirectory());
}

// Parse --config <path> from command line. Returns path to game.config.
std::filesystem::path ParseConfigPath(int argc, char** argv)
{
    for (int i = 1; i < argc - 1; ++i)
    {
        if (std::string(argv[i]) == "--config")
            return std::filesystem::path(argv[i + 1]);
    }
    // Default: the game.config the build staged.
    return StagedContentRoot() / "game.config";
}

// Parse --asset-root <path> from command line; nullopt when it is not given.
std::optional<std::filesystem::path> ParseAssetRoot(int argc, char** argv)
{
    for (int i = 1; i < argc - 1; ++i)
    {
        if (std::string(argv[i]) == "--asset-root")
            return std::filesystem::path(argv[i + 1]);
    }
    return std::nullopt;
}

void ApplyWindowModeOverrides(int argc, char** argv, GameEngine::GameConfig& cfg)
{
    bool cliOverride = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg(argv[i]);
        if (arg == "--fullscreen")
        {
            cfg.windowMode = GameEngine::WindowMode::BorderlessFullscreen;
            cliOverride = true;
        }
        else if (arg == "--exclusive-fullscreen")
        {
            cfg.windowMode = GameEngine::WindowMode::ExclusiveFullscreen;
            cliOverride = true;
        }
        else if (arg == "--windowed")
        {
            cfg.windowMode = GameEngine::WindowMode::Windowed;
            cliOverride = true;
        }
        else if (arg == "--window-mode")
        {
            if (i + 1 >= argc)
            {
                Logger::Log::Error("Player: --window-mode requires windowed, borderless, or exclusive");
                continue;
            }
            GameEngine::WindowMode parsed = GameEngine::WindowMode::Windowed;
            if (!GameEngine::TryParseWindowMode(argv[++i], parsed))
            {
                Logger::Log::Error("Player: Unknown --window-mode value '{}'", argv[i]);
                continue;
            }
            cfg.windowMode = parsed;
            cliOverride = true;
        }
    }

    if (cliOverride)
        return;

    const char* envMode = std::getenv("GE_WINDOW_MODE");
    if (!envMode || envMode[0] == '\0')
        return;

    GameEngine::WindowMode parsed = GameEngine::WindowMode::Windowed;
    if (GameEngine::TryParseWindowMode(envMode, parsed))
        cfg.windowMode = parsed;
    else
        Logger::Log::Warning("Player: Unknown GE_WINDOW_MODE '{}', using game.config value", envMode);
}

#if GE_PLAYER_MOVIE_RECORDER
bool TryParseDouble(const char* text, double& out)
{
    if (!text)
        return false;
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || (end && *end != '\0'))
        return false;
    out = value;
    return true;
}

bool TryParseUint64(const char* text, uint64_t& out)
{
    if (!text || text[0] == '-')
        return false;
    char* end = nullptr;
    const auto value = std::strtoull(text, &end, 10);
    if (end == text || (end && *end != '\0'))
        return false;
    out = static_cast<uint64_t>(value);
    return true;
}

uint64_t ParseMovieFrameLimit(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::string(argv[i]) != "--quit-after")
            continue;

        uint64_t frameLimit = 0;
        if (i + 1 < argc && TryParseUint64(argv[i + 1], frameLimit))
            return frameLimit;

        Logger::Log::Error("Player: --quit-after requires a non-negative frame count");
        return 0;
    }
    return 0;
}

std::optional<GameEngine::Video::VideoWriterOptions> ParseMovieOptions(int argc, char** argv)
{
    GameEngine::Video::VideoWriterOptions options{};
    options.recordAudio = true;
    // Software-allowed by default: Media Foundation "hardware" H264 routes to
    // whatever GPU registers an encoder MFT and rejects CPU-staged frames on
    // machines where that MFT wants D3D surfaces, so strict-by-default means a
    // fresh install's first recording fails. --require-hardware-encoder opts
    // back into the strict behavior.
    options.requireHardwareAcceleration = false;
    bool hasMoviePath = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg(argv[i]);
        if (arg == "--write-movie")
        {
            if (i + 1 >= argc)
            {
                Logger::Log::Error("Player: --write-movie requires an output path");
                continue;
            }
            options.path = argv[++i];
            hasMoviePath = true;
        }
        else if (arg == "--movie-fps" || arg == "--fixed-fps")
        {
            double fps = 0.0;
            if (i + 1 < argc && TryParseDouble(argv[++i], fps) && fps > 0.0)
                options.fps = fps;
            else
                Logger::Log::Error("Player: {} requires a positive numeric FPS", arg);
        }
        else if (arg == "--movie-codec")
        {
            if (i + 1 >= argc)
            {
                Logger::Log::Error("Player: --movie-codec requires a codec name");
                continue;
            }
            const std::string codecName(argv[++i]);
            const auto codec = GameEngine::Video::ParseVideoCodecName(codecName, GameEngine::Video::VideoCodec::Auto);
            std::string normalizedCodecName = codecName;
            std::transform(normalizedCodecName.begin(), normalizedCodecName.end(), normalizedCodecName.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (codec == GameEngine::Video::VideoCodec::Auto && normalizedCodecName != "auto")
                Logger::Log::Warning("Player: Unknown movie codec '{}', using auto", codecName);
            options.codec = codec;
        }
        else if (arg == "--movie-bitrate")
        {
            uint64_t bitrateKbps = 0;
            if (i + 1 < argc && TryParseUint64(argv[++i], bitrateKbps) && bitrateKbps > 0)
                options.bitrateKbps = static_cast<uint32_t>(std::min<uint64_t>(bitrateKbps, 1000000ull));
            else
                Logger::Log::Error("Player: --movie-bitrate requires a positive bitrate in Kbps");
        }
        else if (arg == "--movie-width")
        {
            uint64_t width = 0;
            if (i + 1 < argc && TryParseUint64(argv[++i], width) && width > 0)
                options.width = static_cast<uint32_t>(std::min<uint64_t>(width, 16384ull));
            else
                Logger::Log::Error("Player: --movie-width requires a positive pixel width");
        }
        else if (arg == "--movie-height")
        {
            uint64_t height = 0;
            if (i + 1 < argc && TryParseUint64(argv[++i], height) && height > 0)
                options.height = static_cast<uint32_t>(std::min<uint64_t>(height, 16384ull));
            else
                Logger::Log::Error("Player: --movie-height requires a positive pixel height");
        }
        else if (arg == "--movie-audio")
        {
            options.recordAudio = true;
        }
        else if (arg == "--no-movie-audio")
        {
            options.recordAudio = false;
        }
        else if (arg == "--movie-fade-in")
        {
            options.fadeInEnabled = true;
        }
        else if (arg == "--no-movie-fade-in")
        {
            options.fadeInEnabled = false;
        }
        else if (arg == "--movie-fade-out")
        {
            options.fadeOutEnabled = true;
        }
        else if (arg == "--no-movie-fade-out")
        {
            options.fadeOutEnabled = false;
        }
        else if (arg == "--movie-fade-color")
        {
            if (i + 1 >= argc)
            {
                Logger::Log::Error("Player: --movie-fade-color requires black or white");
                continue;
            }
            std::string color(argv[++i]);
            std::transform(color.begin(), color.end(), color.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            options.fadeColor = color == "white" ? GameEngine::Video::FadeColor::White : GameEngine::Video::FadeColor::Black;
        }
        else if (arg == "--movie-fade-duration")
        {
            double seconds = 0.0;
            if (i + 1 < argc && TryParseDouble(argv[++i], seconds) && seconds > 0.0)
                options.fadeDurationSeconds = std::clamp(seconds, 0.01, 60.0);
            else
                Logger::Log::Error("Player: --movie-fade-duration requires a positive duration in seconds");
        }
        else if (arg == "--require-hardware-encoder")
        {
            options.requireHardwareAcceleration = true;
        }
        else if (arg == "--movie-temporal-dither")
        {
            // The Player has no editor settings, so the editor's experimental
            // toggle reaches a standalone recording the same way every other
            // recording option does: on the command line it already builds
            // (MovieRecorderController). Process-wide state rather than a
            // VideoWriterOptions field — the phase is consumed by the encode
            // PASS, which never sees these options.
            GameEngine::Rendering::Passes::SetTemporalMovieDitherEnabled(true);
        }
    }

    if (!hasMoviePath)
        return std::nullopt;
    return options;
}
#endif // GE_PLAYER_MOVIE_RECORDER

} // namespace

static int PlayerMainImpl(int argc, char** argv)
{
    GameEngine::Platform::SetCurrentThreadName("Main Thread");

    // Unify this executable's Logger with Engine.dll's BEFORE anything logs.
    // Logger is a static module: the exe and the engine shared library each
    // carry a copy of its state, and without this redirect the two fork —
    // sinks configured below land in one copy while Engine.dll's internal
    // logs drain into the other, unconfigured one (packaged Release games
    // used to emit NO engine-side logs). Same fix the Editor applies at its
    // own startup.
    Logger::Log::RedirectToSharedState(GameEngine::GetEngineLoggerState());

    // Install crash handlers.
    std::set_terminate(TerminateHandler);
#ifdef _WIN32
    SetUnhandledExceptionFilter(UnhandledSEHFilter);
#endif
    std::signal(SIGABRT, SignalHandler);
#ifdef SIGSEGV
    std::signal(SIGSEGV, SignalHandler);
#endif

    // Initialize logger. The Player is a WIN32-subsystem app in every config, so
    // a console sink alone writes nowhere a player (or a shipped game's crash
    // report) can see. The log file opens once the configuration names the game;
    // until then its records are held, so a configuration that fails to load
    // still leaves its reason in the file.
    if (Logger::Log::GetSinkCount() == 0)
    {
        Logger::Log::Config logCfg;
        logCfg.GlobalMinLevel = Logger::LogLevel::Info;
        Logger::Log::Initialize(logCfg);
    }
#if LOGGER_ENABLE_FILE_LOGGING
    Logger::DeferredSink& heldLogRecords = GameEngine::StartPlayerLog();
#endif

    // Capture the LAUNCH working directory before it is rewritten below:
    // relative --config/--asset-root arguments mean "relative to where the
    // user ran the command", not to the exe dir the process pins itself to.
    std::error_code launchCwdEc;
    const std::filesystem::path launchCwd = std::filesystem::current_path(launchCwdEc);

    // Ensure working directory matches executable for asset resolution.
    GameEngine::PathUtils::EnsureWorkingDirectoryMatchesExecutable();

    // Load game config. A user-passed --config that does not exist is a hard
    // error — silently falling back to defaults ("My Game", no scene) renders
    // a black window with no explanation.
    std::filesystem::path configPath = ParseConfigPath(argc, argv);
    const bool configFromArgs = configPath != StagedContentRoot() / "game.config";
    if (configPath.is_relative() && !launchCwdEc)
        configPath = launchCwd / configPath;
    GameEngine::GameConfig gameConfig = GameEngine::LoadGameConfig(configPath);
#if LOGGER_ENABLE_FILE_LOGGING
    GameEngine::OpenPlayerLogFile(heldLogRecords, GameEngine::StandardPaths::UserLogsRoot(gameConfig.gameName));
#endif
    if (configFromArgs && !std::filesystem::exists(configPath))
    {
        Logger::Log::Error("Player: --config '{}' not found", configPath.string());
        Logger::Log::Flush();
        return 1;
    }
    ApplyWindowModeOverrides(argc, argv, gameConfig);

    const GameEngine::PlayerHdrProjectSettings hdrProject{
        gameConfig.hdrEnabled, gameConfig.hdrMode, gameConfig.hdrSwapchainBitDepth};
    const GameEngine::PlayerHdrOptions hdrOptions = GameEngine::ResolvePlayerHdrOptions(
        argc, argv, GameEngine::ReadPlayerHdrEnvironment(), hdrProject);
    GameEngine::LogPlayerHdrOptions(hdrOptions);
    gameConfig.hdrEnabled = hdrOptions.Enabled;
    gameConfig.hdrMode = hdrOptions.Mode;
    gameConfig.hdrSwapchainBitDepth = hdrOptions.BitDepth;
    const bool allowRuntimeHdrToggle = hdrOptions.AllowRuntimeToggle;

    // The standalone Player is a runtime, not a dev tool: it should never spawn
    // the resident CompileServerHost (a hot-reload/incremental-compile helper that
    // needs the dev .NET SDK). On machines without it, that spawn stalls startup
    // ~60s before timing out. Default it off here; an explicit GE_DISABLE_COMPILE_SERVER
    // in the environment still wins (don't overwrite a pre-set value).
#ifdef _WIN32
    if (!std::getenv("GE_DISABLE_COMPILE_SERVER"))
        _putenv_s("GE_DISABLE_COMPILE_SERVER", "1");
#else
    setenv("GE_DISABLE_COMPILE_SERVER", "1", 0);
#endif

    // Build ApplicationConfig from GameConfig.
    GameEngine::ApplicationConfig appConfig;
    appConfig.Name = gameConfig.gameName;
    appConfig.WindowWidth = gameConfig.windowWidth;
    appConfig.WindowHeight = gameConfig.windowHeight;
    appConfig.Fullscreen = (gameConfig.windowMode != GameEngine::WindowMode::Windowed);
    // Resolve the asset root to an absolute path. A relative value from the
    // command line resolves against the LAUNCH cwd (where the user ran the
    // command); without one, the root is the Assets/ the build staged.
    std::filesystem::path assetRoot = GameEngine::PathUtils::GetInstallAssetsRoot();
    if (std::optional<std::filesystem::path> cliAssetRoot = ParseAssetRoot(argc, argv))
    {
        assetRoot = *cliAssetRoot;
        if (assetRoot.is_relative() && !launchCwdEc)
            assetRoot = launchCwd / assetRoot;
    }
    std::error_code absEc;
    if (auto absRoot = std::filesystem::absolute(assetRoot, absEc); !absEc)
        assetRoot = absRoot;
    appConfig.AssetDirectory = assetRoot.lexically_normal().string();

    // Workspace root holds writable runtime data (shader cache, asset-db cache).
    // Default it to a per-user data directory instead of the executable dir — on
    // macOS the exe lives inside a read-only/codesigned .app bundle, so writing a
    // .Cache next to it pollutes (and invalidates) the bundle. Honors an explicit
    // game-config/CLI override if one is ever added.
    if (appConfig.WorkspaceDirectory.empty())
    {
        appConfig.WorkspaceDirectory =
            GameEngine::StandardPaths::UserDataRoot(gameConfig.gameName).string();
    }

#if GE_PLAYER_MOVIE_RECORDER
    auto movieOptions = ParseMovieOptions(argc, argv);
    const uint64_t movieFrameLimit = ParseMovieFrameLimit(argc, argv);
    if (movieOptions)
    {
        constexpr GameEngine::uint32 kMaxMovieWindowWidth = 1920;
        constexpr GameEngine::uint32 kMaxMovieWindowHeight = 1080;
        const GameEngine::uint32 requestedWindowWidth = std::max<GameEngine::uint32>(1, appConfig.WindowWidth);
        const GameEngine::uint32 requestedWindowHeight = std::max<GameEngine::uint32>(1, appConfig.WindowHeight);
        if (requestedWindowWidth > kMaxMovieWindowWidth || requestedWindowHeight > kMaxMovieWindowHeight)
        {
            const double scale = std::min(
                static_cast<double>(kMaxMovieWindowWidth) / static_cast<double>(requestedWindowWidth),
                static_cast<double>(kMaxMovieWindowHeight) / static_cast<double>(requestedWindowHeight));
            appConfig.WindowWidth = std::max<GameEngine::uint32>(
                1,
                static_cast<GameEngine::uint32>(std::lround(static_cast<double>(requestedWindowWidth) * scale)));
            appConfig.WindowHeight = std::max<GameEngine::uint32>(
                1,
                static_cast<GameEngine::uint32>(std::lround(static_cast<double>(requestedWindowHeight) * scale)));
            Logger::Log::Info(
                "Player: Clamped movie capture window from {}x{} to {}x{}; output remains {}x{}",
                requestedWindowWidth,
                requestedWindowHeight,
                appConfig.WindowWidth,
                appConfig.WindowHeight,
                movieOptions->width,
                movieOptions->height);
        }
    }
#else
    for (int i = 1; i < argc; ++i)
    {
        if (std::string(argv[i]) == "--write-movie")
        {
            Logger::Log::Error(
                "Player: movie recording is not compiled into this build "
                "(reconfigure with -DPLAYER_MOVIE_RECORDER=ON)");
            return 1;
        }
    }
#endif

    // Detect NativeAOT scripts library next to the executable. When present,
    // CoreCLR initialization is skipped entirely and ManagedSystemBridge loads
    // the AOT library via LoadLibrary/dlopen instead.
    auto exeDir = GameEngine::PathUtils::GetExecutableDirectory();
    bool useNativeAOT = std::filesystem::exists(exeDir / GameEngine::kNativeAotScriptsLibraryName);

    // Packaged-game detection is a POSITIVE signal from the shipped layout:
    // game.config plus the staged asset-identity manifest (Assets/.assetmanifest),
    // both written into the staged content root by the build pipeline. Established
    // HERE, before any engine/script initialization, so packaged behavior never
    // has to be inferred mid-flight. In packaged mode managed scripts ship
    // PREBUILT (game.config scriptAssemblyPath under Managed/, package
    // assemblies in Managed/Packages/ beside it); the Player only ever LOADS
    // them — csproj generation, dotnet, and the compile server are hard-disabled
    // in ScriptManager. A configured-but-missing assembly is a loud error
    // (incomplete package / stale config), then the CLR stays off rather than
    // falling back to the dev compile pipeline.
    const GameEngine::PackagedGameLayout packagedLayout =
        GameEngine::DetectPackagedGameLayout(StagedContentRoot(), gameConfig.scriptAssemblyPath);
    if (packagedLayout.IsPackaged)
    {
        Logger::Log::Info(
            "Player: packaged game layout detected (game.config + Assets/.assetmanifest at '{}') "
            "— packaged script mode, runtime compilation disabled",
            packagedLayout.ContentRoot.string());
    }
    if (!useNativeAOT && !packagedLayout.PrebuiltScriptAssembly.empty() &&
        !packagedLayout.PrebuiltScriptAssemblyExists)
    {
        Logger::Log::Error(
            "Player: game.config scriptAssemblyPath '{}' not found at '{}' — managed "
            "scripts will not load (repackage the game or fix game.config)",
            gameConfig.scriptAssemblyPath, packagedLayout.PrebuiltScriptAssembly.string());
    }

    GameEngine::ScriptsConfig scriptsConfig =
        GameEngine::BuildPlayerScriptsConfig(packagedLayout, useNativeAOT);
    GameEngine::EngineCore::GetInstance().SetScriptsConfig(scriptsConfig);

#if GE_ENABLE_SCRIPTING
    // Unify the DLL-local logger with this module's state (same fix as the
    // exe<->Engine.dll split): without it, GameEngine.Native.dll logs vanish.
    GE_SetHostLoggerState(GameEngine::GetEngineLoggerState());

    // Share the host's ComponentRegistry with the DLL. (ComponentTypeId is
    // consteval post-Phase-1b — no allocator state to share.)
    GE_SetHostEcsState(
        GameEngine::ECS::ComponentRegistry::GetComponentsPtr(),
        GameEngine::ECS::ComponentRegistry::GetNameToTypeIdPtr(),
        GameEngine::ECS::ComponentRegistry::GetHandlersPtr());
#endif

    if (useNativeAOT)
        Logger::Log::Info("[Player] NativeAOT scripts detected — CoreCLR disabled");

    // Enable .NET diagnostics for debugger attachment in debug builds.
#if defined(_DEBUG) || defined(DEBUG)
    if (!scriptsConfig.disableClr)
    {
#ifdef _WIN32
        _putenv_s("DOTNET_EnableDiagnostics", "1");
#else
        setenv("DOTNET_EnableDiagnostics", "1", 0);
#endif
    }
#endif

    // --screenshot <path>: one-shot FinalColor readback -> PNG -> exit (headless verify).
    std::string screenshotPath;
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::string(argv[i]) == "--screenshot")
        {
            screenshotPath = argv[i + 1];
            break;
        }
    }

    // Create and run the player application. Managed GameSystems tick through
    // ManagedSystemBridge in BOTH script modes (NativeAOT and CoreCLR packaged);
    // the Player has no PlayModeDriver to do it.
    GameEngine::PlayerApplication app(
        appConfig, gameConfig,
        GameEngine::PlayerNeedsManagedSystemBridge(scriptsConfig, useNativeAOT),
#if GE_PLAYER_MOVIE_RECORDER
        movieOptions, movieFrameLimit,
#endif
        allowRuntimeHdrToggle, screenshotPath);
    try
    {
        if (!app.Initialize())
        {
            Logger::Log::Error("Player: Failed to initialize");
            return -1;
        }
        const int exitCode = app.Run();
        app.Shutdown();
        return exitCode;
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("Player: Unhandled exception: {}", e.what());
        return -2;
    }
    catch (...)
    {
        Logger::Log::Error("Player: Unhandled non-std exception");
        return -3;
    }
}

#ifdef _WIN32
int WINAPI WinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int)
{
    return PlayerMainImpl(__argc, __argv);
}
#endif

int main(int argc, char** argv)
{
    return PlayerMainImpl(argc, argv);
}
