// Web Player entry (web platform plan, Phase 6): the same PlayerApplication as
// the desktop Player, driven by the browser instead of a blocking main loop.
// Nothing is baked in at link time — the export dist this page was served
// with is fetched into MEMFS before the engine boots, so one built binary is a
// reusable template for any exported project. The WebGPU device binds to the
// shell page's #canvas through the engine's window path. The browser owns the
// frame cadence: main() initializes and registers a rAF callback that runs
// Application::Tick once per frame — main returns with the runtime kept alive,
// and the tab owns shutdown.

#include "PlayerApplication.h"
#include "WebDistLoader.h"

#include "Core/EngineLoggerBridge.h"
#include "Core/Engine.h"
#include "Engine/Build/GameConfig.h"
#include "Logger/Logger.h"
#include "Platform/WebEnvironment.h"
#include "Scripting/ScriptsConfig.h"

#include <emscripten/emscripten.h>

#include <cstdio>
#include <filesystem>
#include <string>

namespace
{

// The dist unpacks into MEMFS mirroring the desktop Player's next-to-exe
// layout: the wasm "executable directory" is the FS root.
constexpr const char kContentRoot[] = "/";

GameEngine::PlayerApplication* g_App = nullptr;
int g_Frame = 0;
double g_LastHeartbeatMs = 0.0;
constexpr int kHeartbeatFrames = 300;

void Frame(void*)
{
    ++g_Frame;
    if (g_Frame % kHeartbeatFrames == 0)
    {
        const double nowMs = emscripten_get_now();
        const double fps =
            g_LastHeartbeatMs > 0.0 ? kHeartbeatFrames * 1000.0 / (nowMs - g_LastHeartbeatMs) : 0.0;
        g_LastHeartbeatMs = nowMs;
        std::printf("WEBPLAYER: heartbeat frame=%d fps=%.1f\n", g_Frame, fps);
    }
    if (!g_App->PollEventsAndTick())
    {
        // Exit was requested (device loss, script quit). The page stays up;
        // stop ticking and leave the final log state visible on the console.
        Logger::Log::Info("WebPlayer: main loop ended (exit code {})", 0);
        emscripten_cancel_main_loop();
    }
}

} // namespace

int main()
{
    // Diagnostic switches first: the query string is this host's environment,
    // and engine subsystems read theirs as they initialize.
    GameEngine::Platform::Web::ImportEnvironmentFromUrl();

    Logger::Log::RedirectToSharedState(GameEngine::GetEngineLoggerState());
    if (Logger::Log::GetSinkCount() == 0)
    {
        Logger::Log::Config logCfg;
        logCfg.GlobalMinLevel = Logger::LogLevel::Info;
        Logger::Log::Initialize(logCfg);
    }

    std::printf("WEBPLAYER: boot (threads=%d)\n",
#if defined(GE_WASM_SINGLE_THREAD)
                0
#else
                1
#endif
    );

    const std::filesystem::path contentRoot{kContentRoot};
    GameEngine::WebPlayer::WebDistInfo dist;
    std::string distError;
    if (!GameEngine::WebPlayer::LoadWebDist(contentRoot, dist, distError))
    {
        Logger::Log::Error("WebPlayer: no loadable export dist — {}", distError);
        return 1;
    }
    std::printf("WEBPLAYER: dist '%s' loaded (entry scene '%s')\n", dist.ProjectName.c_str(),
                dist.EntryScene.c_str());

    GameEngine::GameConfig gameConfig = GameEngine::LoadGameConfig(contentRoot / "game.config");

    GameEngine::ApplicationConfig appConfig;
    appConfig.Name = gameConfig.gameName;
    appConfig.WindowWidth = gameConfig.windowWidth;
    appConfig.WindowHeight = gameConfig.windowHeight;
    appConfig.Fullscreen = false; // the canvas is the window; CSS sizes it
    appConfig.AssetDirectory = (contentRoot / "Assets").string();
    // MEMFS is writable; keep runtime caches inside the content mount.
    appConfig.WorkspaceDirectory = (contentRoot / ".workspace").string();

    // No C# on this target yet (Phase 5 owns the wasm scripting story).
    GameEngine::ScriptsConfig scriptsConfig;
    scriptsConfig.disableClr = true;
    GameEngine::EngineCore::GetInstance().SetScriptsConfig(scriptsConfig);

    static GameEngine::PlayerApplication app(appConfig, gameConfig,
                                             /*tickManagedSystems*/ false);
    g_App = &app;
    if (!app.Initialize())
    {
        Logger::Log::Error("WebPlayer: Failed to initialize");
        return 1;
    }
    std::printf("WEBPLAYER: initialized (%ux%u, scene '%s')\n", appConfig.WindowWidth,
                appConfig.WindowHeight, gameConfig.startupScene.c_str());

    // fps=0: follow the display's rAF rate. simulate_infinite_loop=0: return
    // from main and keep the runtime alive (no EXIT_RUNTIME on this target).
    emscripten_set_main_loop_arg(&Frame, nullptr, 0, 0);
    return 0;
}
