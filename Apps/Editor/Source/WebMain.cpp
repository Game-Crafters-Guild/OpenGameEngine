// Web Editor entry (web platform plan, Phase 7): the same EditorApplication as
// the desktop editor, driven by the browser instead of a blocking main loop.
//
// Not a port of main.cpp. That file is a desktop host — SEH crash dumps, a
// per-process log file, the GameEngine.Native ABI handshake, relaunch-args
// capture for device-loss restart — and every one of those has no counterpart
// in a page. What is shared is the part that matters: the same
// ApplicationConfig, the same EditorApplication, the same Tick().
//
// The browser owns the frame cadence: main() initializes and registers a rAF
// callback that runs Application::Tick once per frame, then returns with the
// runtime kept alive.

#include "EditorApplication.h"

#include "Core/Application.h"
#include "Core/Engine.h"
#include "Engine/Build/GameConfig.h"
#include "FileSystem/FileSystem.h"
#include "Logger/Logger.h"
#include "Platform/WebEnvironment.h"
#include "Platform/WebPersistentStorage.h"
#include "Scripting/ScriptsConfig.h"

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/stack.h>
#include <malloc.h>

#include <cstdio>
#include <sys/stat.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{

// MEMFS root: the dist unpacks mirroring the desktop next-to-exe layout, so
// the wasm "executable directory" is the filesystem root.
constexpr const char kWorkspaceRoot[] = "/";

// Defined by the Platform module, which owns the mount; aliased for brevity.
constexpr const char* kProjectsMount = GameEngine::Platform::Web::kProjectsMount;

// Preloaded into MEMFS at link time: the project the editor plants on first run
// so a fresh browser has something to open. Editor chrome ships with the binary
// for the same reason (an editor without it is not an editor), but a project is
// user content, so this is a seed and not a mount — it is copied out once and
// the copy is what gets edited.
constexpr const char kSeedProjectSource[] = "/SeedProject";

/// Read a query parameter from the page's URL. The browser's counterpart to a
/// command-line switch: `?project=/project/WebSmoke` is `--project <path>`.
std::string QueryParameter(const char* name)
{
    char* value = static_cast<char*>(EM_ASM_PTR({
        const params = new URLSearchParams(globalThis.location.search);
        const found = params.get(UTF8ToString($0));
        return found === null ? 0 : stringToNewUTF8(found);
    }, name));
    if (value == nullptr)
    {
        return {};
    }
    std::string out(value);
    std::free(value);
    return out;
}

/// Copy the preloaded seed project into persistent storage.
///
/// Absent seed is normal, not an error: this build shipped without one.
/// A destination that already exists is the user's project and is left
/// alone unless `?replant=1` / `GE_REPLANT=1` explicitly requests a wipe.
/// A seed revision is not permission to discard an edited project. Other
/// `/project/*` trees that were not in `/SeedProject` are never touched.
void PlantSeedProject()
{
    std::error_code probe;
    if (!std::filesystem::is_directory(kSeedProjectSource, probe))
    {
        std::printf("WEBEDITOR: no seed project preloaded at %s\n", kSeedProjectSource);
        return;
    }

    const char* replantEnv = std::getenv("GE_REPLANT");
    const bool forceReplant =
        QueryParameter("replant") == "1" || (replantEnv != nullptr && std::string(replantEnv) == "1");

    std::error_code walk;
    for (const auto& seed : std::filesystem::directory_iterator(kSeedProjectSource, walk))
    {
        if (!seed.is_directory())
        {
            continue;
        }
        const std::filesystem::path destination =
            std::filesystem::path(kProjectsMount) / seed.path().filename();
        std::error_code step;
        const bool destinationExists = std::filesystem::exists(destination, step);

        if (destinationExists)
        {
            if (!forceReplant)
            {
                std::printf("WEBEDITOR: project '%s' already present; seed not replanted\n",
                            destination.string().c_str());
                continue;
            }
            std::error_code clear;
            std::filesystem::remove_all(destination, clear);
            if (clear)
            {
                std::printf("WEBEDITOR: could not clear '%s' for replant: %s\n",
                            destination.string().c_str(), clear.message().c_str());
                continue;
            }
        }

        if (!GameEngine::FileSystem::CopyTree(seed.path(), destination))
        {
            std::printf("WEBEDITOR: could not plant seed project '%s'\n",
                        destination.string().c_str());
            continue;
        }
        if (destinationExists)
        {
            std::printf("WEBEDITOR: replanted seed project at '%s' (explicit request)\n",
                        destination.string().c_str());
        }
        else
        {
            std::printf("WEBEDITOR: planted seed project at '%s'\n", destination.string().c_str());
        }
    }
    if (walk)
    {
        std::printf("WEBEDITOR: could not read the seed project directory: %s\n",
                    walk.message().c_str());
    }
}

/// Resolve the project to open, or empty to land on the picker.
std::filesystem::path ResolveRequestedProject()
{
    const std::string requested = QueryParameter("project");
    if (requested.empty())
    {
        return {};
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(requested, ec))
    {
        std::printf("WEBEDITOR: ?project=%s is not a directory on this origin's storage; opening "
                    "the project picker instead. Projects live under %s.\n",
                    requested.c_str(), kProjectsMount);
        return {};
    }
    std::printf("WEBEDITOR: opening project %s\n", requested.c_str());
    return std::filesystem::path(requested);
}

GameEngine::EditorApplication* g_App = nullptr;
int g_Frame = 0;
double g_LastHeartbeatMs = 0.0;
constexpr int kHeartbeatFrames = 300;

// A Tick() that never returns left the browser through a JS throw, and a JS
// throw pops no C++ frames: the shadow stack those frames hold is gone for
// good. Emscripten does exactly this once any pthread has crashed — every
// subsequent yield on the main thread calls emscripten_exit_with_live_runtime,
// whose `throw "unwind"` the runtime then swallows as a normal exit — so the
// loop keeps ticking, draws nothing, and drains about a kilobyte of stack a
// frame until the region is spent. Say so at the point it starts rather than
// let it surface as a stack overflow fifteen hundred frames later.
bool g_TickInProgress = false;
int g_AbandonedFrames = 0;
constexpr int kAbandonedFrameReport = 8;

size_t WasmHeapBytes()
{
    // EM_ASM_INT is i32 and cannot hold a 4 GiB heap. The SAB length is a JS
    // number, so take it as a double.
    return static_cast<size_t>(EM_ASM_DOUBLE({
        return (typeof wasmMemory !== 'undefined' && wasmMemory.buffer)
            ? wasmMemory.buffer.byteLength
            : 0;
    }));
}

void Frame(void*)
{
    ++g_Frame;
    // The first frames are reported individually: a browser build that wedges
    // does so in frame 1 or 2, and a heartbeat every 300 frames cannot tell
    // "stalled before drawing anything" from "never started".
    if (g_Frame <= 3)
    {
        std::printf("WEBEDITOR: frame %d begin heap=%zu\n", g_Frame, WasmHeapBytes());
    }
    if (g_Frame % kHeartbeatFrames == 0)
    {
        const double nowMs = emscripten_get_now();
        const double fps =
            g_LastHeartbeatMs > 0.0 ? kHeartbeatFrames * 1000.0 / (nowMs - g_LastHeartbeatMs) : 0.0;
        g_LastHeartbeatMs = nowMs;
        // Free shadow stack, because on this target it is a consumable. The
        // browser's own call stack is ~1 MB and cannot be raised from the page,
        // and the C++ stack is a fixed linear-memory region: a frame loop that
        // does not return all of it lands below the region and writes over
        // whatever is there. Reported every heartbeat so a run's transcript
        // shows the trend, not just the crash. Heap size is the grown wasm
        // SAB; it does not shrink, and the 4 GiB cap freezes Chrome's pthread
        // workers once it is hit.
        std::printf("WEBEDITOR: heartbeat frame=%d fps=%.1f stackFree=%zu heap=%zu used=%zu\n", g_Frame, fps,
                    static_cast<size_t>(emscripten_stack_get_free()), WasmHeapBytes(),
                    static_cast<size_t>(mallinfo().uordblks));
    }
    if (g_TickInProgress)
    {
        ++g_AbandonedFrames;
        if (g_AbandonedFrames == kAbandonedFrameReport)
        {
            std::printf("WEBEDITOR: %d consecutive frames left Tick() without returning — the "
                        "frame loop is being unwound to the event loop and the shadow stack it "
                        "held is not coming back (free=%zu). A worker thread has crashed; look "
                        "for 'sent an error' earlier in this log.\n",
                        g_AbandonedFrames, static_cast<size_t>(emscripten_stack_get_free()));
        }
    }
    else
    {
        g_AbandonedFrames = 0;
    }

    g_TickInProgress = true;
    const bool keepRunning = g_App->PollEventsAndTick();
    g_TickInProgress = false;
    if (!keepRunning)
    {
        Logger::Log::Info("WebEditor: main loop ended");
        emscripten_cancel_main_loop();
    }
}

// file_packager mounts every --preload-file directory and file read-only
// (mode 555). The editor treats /Assets and /.Cache as writable roots — new
// Polyhaven downloads land under Assets/, texture cooks and cooked-variant
// lookups write under .Cache/ — and on desktop those trees ARE writable staged
// copies. Lift the preload's read-only bit once at boot so the same code paths
// work here; MEMFS is a per-session copy, so nothing durable is being opened up.
void MakePreloadedTreeWritable(const char* root)
{
    std::error_code ec;
    ::chmod(root, 0777);
    for (std::filesystem::recursive_directory_iterator it(root, ec), end;
         it != end && !ec; it.increment(ec))
    {
        ::chmod(it->path().c_str(), it->is_directory(ec) ? 0777 : 0666);
    }
}

} // namespace

int main()
{
    // Diagnostic switches first: the query string is this host's environment,
    // and engine subsystems read theirs as they initialize.
    GameEngine::Platform::Web::ImportEnvironmentFromUrl();

    // The release browser host must initialize logging just like native main.
    // Without this, Log's pre-initialization stdout fallback bypasses every
    // sink, including the editor's Log panel and startup ring buffer.
    Logger::Log::Config logConfig;
    logConfig.GlobalMinLevel = Logger::LogLevel::Debug;
    logConfig.ConsoleUseStderr = false;
#if LOGGER_ENABLE_SOURCE_LOCATION
    logConfig.EnableSourceLocation = true;
#endif
    Logger::Log::Initialize(logConfig);

    // Before a project can be resolved, the projects tree has to be durable.
    //
    // This is the only OPFS mount, and everything that has to survive a reload
    // lives inside it: PathUtils::GetUserData/CacheDirectory() resolve to
    // /project/.user-data and /project/.cache, hidden subtrees of this mount,
    // because a second OPFS mount would alias this one instead of sitting
    // beside it. Those roots are created on first use and only once the mount
    // is up -- creating them any earlier plants a MEMFS directory at the mount
    // point and poisons the mount itself.
    //
    // Keep these low-level mount diagnostics on stdout as well.
    const bool projectsMounted = GameEngine::Platform::Web::MountPersistentStorage(kProjectsMount);
    std::printf("WEBEDITOR: projects mount %s at %s\n", projectsMounted ? "ok" : "REFUSED",
                kProjectsMount);

    // The preloaded bundle trees stay read-only without this: asset downloads
    // (Assets/Polyhaven) and every cook-cache write under /.Cache fail with
    // EACCES otherwise.
    MakePreloadedTreeWritable("/Assets");
    MakePreloadedTreeWritable("/.Cache");
    PlantSeedProject();
    std::printf("WEBEDITOR: storage ready\n");

    GameEngine::ApplicationConfig config;
    config.Name = "Open Engine Editor";
    config.EnableEditor = true;
    config.PersistScenesEagerly = true; // browser storage is the only place a scene survives
    // A project opened in the first frames deadlocks the wasm main thread in
    // the scene view's boot batch (the frame-3 shadow/fog boundary). Boots land
    // on the picker; a later open wins that race far more often.
    config.AutoOpenLastProjectOnBoot = false;
    // Persistent storage mounts on frame 8 of the boot sequence; a startup
    // scene opened before that reads an empty project tree.
    config.StartupSceneOpenFrame = 8;

    // No ?project= means the fallback root, which is what makes the editor show
    // the project picker (EditorApplication gates it on exactly that).
    const std::filesystem::path requestedProject = ResolveRequestedProject();
    config.WorkspaceDirectory =
        requestedProject.empty() ? std::filesystem::path(kWorkspaceRoot) : requestedProject;
    config.WorkspaceDirectoryIsFallback = requestedProject.empty();

    // The editor otherwise opens Untitled. `?scene=<project-relative path>`
    // names the startup scene outright (a gate can open a scene authored for
    // one check); otherwise seed GE_EDITOR_STARTUP_SCENE from the project's
    // game.config so `?project=/project/WebSmoke` actually loads WebSmoke.scene
    // (and whatever effects that scene authors).
    if (const std::string scene = QueryParameter("scene"); !scene.empty())
        setenv("GE_EDITOR_STARTUP_SCENE", scene.c_str(), 1);
    if (!requestedProject.empty() && std::getenv("GE_EDITOR_STARTUP_SCENE") == nullptr)
    {
        const auto gameConfig = GameEngine::LoadGameConfig(requestedProject / "game.config");
        if (!gameConfig.startupScene.empty())
            setenv("GE_EDITOR_STARTUP_SCENE", gameConfig.startupScene.c_str(), 0);
    }

    // Headless smoke: `?smoke_polyhaven=<slug>:<type>` kicks one online-library
    // download at startup so a gate run can verify the whole transfer path
    // (JobSystem worker -> synchronous fetch -> cache write) from the console
    // transcript alone. Same contract as GE_SMOKE_POLYHAVEN on native.
    if (const std::string smokeDownload = QueryParameter("smoke_polyhaven"); !smokeDownload.empty())
        setenv("GE_SMOKE_POLYHAVEN", smokeDownload.c_str(), 0);

    // No CoreCLR on this target (web plan Phase 5), so nothing may try to host
    // it or watch script files for a compile that cannot run.
    GameEngine::ScriptsConfig scriptsConfig;
    scriptsConfig.disableClr = true;
    scriptsConfig.enableHotReload = false;
    scriptsConfig.enableAsyncHotReload = false;
    scriptsConfig.enableAutoProjectGeneration = false;
    scriptsConfig.deferInitialLoad = false;
    GameEngine::EngineCore::GetInstance().SetScriptsConfig(scriptsConfig);

    static GameEngine::EditorApplication app(config);
    g_App = &app;

    if (!app.Initialize())
    {
        Logger::Log::Error("WebEditor: EditorApplication::Initialize failed");
        std::printf("WEBEDITOR: initialize failed\n");
        return 1;
    }

    std::printf("WEBEDITOR: initialized, entering rAF loop\n");
    Logger::Log::Info("Web editor initialized; entering the browser frame loop");
    emscripten_set_main_loop_arg(Frame, nullptr, 0, 0);
    // requestAnimationFrame stops for hidden pages — and Chrome marks a fully
    // OCCLUDED window hidden, not just background tabs. Everything frame-driven
    // (the boot sequence's persistent-storage mount, project open, hot-reload)
    // then parks forever with the main thread idle: the long-standing
    // "intermittent boot wedge" was exactly this, intermittent only in how often
    // the window happened to be covered. Tick on a timer while hidden; hand the
    // loop back to rAF when visible so presentation stays vsync-aligned.
    emscripten_set_visibilitychange_callback(
        nullptr, false,
        [](int, const EmscriptenVisibilityChangeEvent* ev, void*) -> EM_BOOL {
            if (ev->hidden)
                emscripten_set_main_loop_timing(EM_TIMING_SETTIMEOUT, 33);
            else
                emscripten_set_main_loop_timing(EM_TIMING_RAF, 0);
            return EM_TRUE;
        });
    // The page can START hidden (an automation tab, a restored background tab):
    // pick the right mode for the initial state too, or boot itself parks.
    EmscriptenVisibilityChangeEvent vis{};
    if (emscripten_get_visibility_status(&vis) == EMSCRIPTEN_RESULT_SUCCESS && vis.hidden)
        emscripten_set_main_loop_timing(EM_TIMING_SETTIMEOUT, 33);
    return 0;
}
