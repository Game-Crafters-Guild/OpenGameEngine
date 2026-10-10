// The engine's lifecycle as a page drives it: ge_create, ge_tick, ge_update_assets, ge_resize
// and ge_shutdown (Apps/WebLibrary/ts/src/abi.ts).

#include "AbiLifecycle.h"

#include "AbiAssets.h"
#include "AbiErrors.h"
#include "AbiQuery.h"
#include "WebLibraryApplication.h"
#include "WebPackReader.h"

#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "Core/EngineLoggerBridge.h"
#include "Logger/Logger.h"
#include "Scripting/ScriptsConfig.h"

#include <emscripten/emscripten.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>

namespace GameEngine::WebLibrary
{

namespace
{

// The engine's own content (shaders, the render pipeline, the cooked material variants),
// fetched from beside the module's glue and unpacked into the module's file system root.
constexpr const char* kEnginePackName = "opengine-core.gepak";
constexpr const char* kContentRoot = "/";

enum class LifecycleState : uint8_t
{
    NotCreated,
    Running,
    // ge_create failed after the engine started, or ge_shutdown ran: the engine's process-wide
    // state (its singleton, the one window the canvas allows) does not come back up in this
    // module instance.
    Finished,
};

LifecycleState g_State = LifecycleState::NotCreated;
std::unique_ptr<WebLibraryApplication> g_Application;

// 0 when the page has a document and WebGPU; 1 without a document, 2 without WebGPU.
EM_JS(int, ProbeBrowser, (), {
    if (typeof document === 'undefined')
        return 1;
    if (typeof navigator === 'undefined' || !navigator.gpu)
        return 2;
    return 0;
});

// Points the module's canvas, and the selector the engine's WebGPU surface binds to, at the
// element `selector` names. 0 on success; 1 when it names nothing, 2 when it is not a canvas.
EM_JS_DEPS(WebLibraryCanvas, "$specialHTMLTargets");
EM_JS(int, BindCanvas, (const char* selector), {
    var element = document.querySelector(UTF8ToString(selector));
    if (!element)
        return 1;
    if (typeof element.getContext !== 'function')
        return 2;
    Module['canvas'] = element;
    specialHTMLTargets['#canvas'] = element;
    return 0;
});

// Fetches `name` from beside the module's glue into memory allocated with malloc. 0 on
// success, with the bytes at *outData and their count at *outSize; the HTTP status when the
// server answered otherwise; -1 when the request failed outright. Suspends under ASYNCIFY.
EM_ASYNC_JS(int, FetchBesideModule, (const char* name, uint8_t** outData, uint32_t* outSize), {
    var response;
    try {
        response = await fetch(locateFile(UTF8ToString(name)));
    } catch (error) {
        return -1;
    }
    if (!response.ok)
        return response.status || -1;
    var bytes = new Uint8Array(await response.arrayBuffer());
    var data = _malloc(bytes.length);
    HEAPU8.set(bytes, data);
    HEAPU32[outData >> 2] = data;
    HEAPU32[outSize >> 2] = bytes.length;
    return 0;
});

bool RefuseUnlessRunning(std::string_view call)
{
    if (RefuseAfterShutdown(call))
        return true;
    if (!g_Application)
    {
        SetLastError("{} was called before ge_create succeeded.", call);
        return true;
    }
    return false;
}

bool CheckBrowser(const char* canvasSelector)
{
    switch (ProbeBrowser())
    {
    case 1:
        SetLastError("ge_create needs a browser page: this environment has no document.");
        return false;
    case 2:
        SetLastError("ge_create needs WebGPU, and this browser has none. Use a current Chrome or Edge, or Safari "
                     "or Firefox with WebGPU enabled.");
        return false;
    default:
        break;
    }
    if (!canvasSelector || canvasSelector[0] == '\0')
    {
        SetLastError("ge_create needs the canvas's CSS selector; it was empty.");
        return false;
    }
    switch (BindCanvas(canvasSelector))
    {
    case 1:
        SetLastError("ge_create: the selector '{}' names no element on the page.", canvasSelector);
        return false;
    case 2:
        SetLastError("ge_create: the selector '{}' names an element that is not a canvas.", canvasSelector);
        return false;
    default:
        return true;
    }
}

bool FetchEnginePack()
{
    uint8_t* data = nullptr;
    uint32_t size = 0;
    const int status = FetchBesideModule(kEnginePackName, &data, &size);
    if (status != 0)
    {
        if (status < 0)
            SetLastError("ge_create could not fetch {} from beside the engine module: the request failed.",
                         kEnginePackName);
        else
            SetLastError("ge_create could not fetch {} from beside the engine module: HTTP {}. Serve it from the "
                         "folder that serves opengine-core.st.js and opengine-core.mt.js.",
                         kEnginePackName, status);
        return false;
    }
    const WebPlayer::PackUnpackResult unpacked =
        WebPlayer::UnpackWebPack(std::span<const uint8_t>(data, size), kContentRoot);
    std::free(data);
    if (!unpacked.Success)
    {
        SetLastError("ge_create: {} is damaged: {}", kEnginePackName, unpacked.Error);
        return false;
    }
    return true;
}

ApplicationConfig MakeApplicationConfig()
{
    ApplicationConfig config;
    config.Name = "Web page";
    // The canvas is the window; its CSS size, not this, sizes the drawing buffer.
    config.Fullscreen = false;
    config.AssetDirectory = std::string(kContentRoot) + "Assets";
    // The module's file system is memory: runtime caches stay inside it.
    config.WorkspaceDirectory = std::string(kContentRoot) + ".workspace";
    return config;
}

void StartLogging()
{
    Logger::Log::RedirectToSharedState(GetEngineLoggerState());
    if (Logger::Log::GetSinkCount() != 0)
        return;
    Logger::Log::Config config;
    config.GlobalMinLevel = Logger::LogLevel::Info;
    Logger::Log::Initialize(config);
}

int32_t Create(const char* canvasSelector)
{
    if (!CheckBrowser(canvasSelector) || !FetchEnginePack())
        return kFailed;

    StartLogging();
    ScriptsConfig scriptsConfig;
    scriptsConfig.disableClr = true;
    // A page has no files to edit: nothing reloads from its file system.
    scriptsConfig.enableHotReload = false;
    EngineCore::GetInstance().SetScriptsConfig(scriptsConfig);

    g_State = LifecycleState::Finished;
    auto application = std::make_unique<WebLibraryApplication>(MakeApplicationConfig());
    std::string error;
    if (!application->Start(error))
    {
        SetLastError("ge_create: {}", error);
        return kFailed;
    }
    g_Application = std::move(application);
    g_State = LifecycleState::Running;
    return kOk;
}

} // namespace

WebLibraryApplication* GetRunningApplication()
{
    return g_Application.get();
}

bool RefuseAfterShutdown(std::string_view call)
{
    if (g_State != LifecycleState::Finished)
        return false;
    SetLastError("{} was called after the engine shut down; load the engine module again to start a new one.",
                 call);
    return true;
}

} // namespace GameEngine::WebLibrary

using namespace GameEngine::WebLibrary;

extern "C"
{

/// Creates the engine on the canvas `canvasSelector` names. Suspends, as ge_shutdown does.
EMSCRIPTEN_KEEPALIVE int32_t ge_create(const char* canvasSelector, uint32_t flags)
{
    const AbiCallScope scope("ge_create");
    if (scope.Refused() || RefuseAfterShutdown("ge_create"))
        return kFailed;
    if (g_State == LifecycleState::Running)
    {
        SetLastError("ge_create was called twice; one engine module runs one engine.");
        return kFailed;
    }
    if (flags != 0)
    {
        SetLastError("ge_create: no flag is defined yet; pass 0 (got {}).", flags);
        return kFailed;
    }
    return Create(canvasSelector);
}

/// Runs one frame, then advances the asset loads.
EMSCRIPTEN_KEEPALIVE int32_t ge_tick()
{
    const AbiCallScope scope("ge_tick");
    if (scope.Refused())
        return kFailed;
    if (RefuseWhileQueryRuns("ge_tick"))
        return kFailed;
    if (RefuseUnlessRunning("ge_tick"))
        return kFailed;
    const bool running = g_Application->PollEventsAndTick();
    AdvanceAssetLoads();
    if (!running)
    {
        SetLastError("ge_tick: the engine stopped: the WebGPU device was lost. Reload the page.");
        return kFailed;
    }
    return kOk;
}

/// Runs the asset manager's update alone, then advances the asset loads.
EMSCRIPTEN_KEEPALIVE int32_t ge_update_assets()
{
    const AbiCallScope scope("ge_update_assets");
    if (scope.Refused() || RefuseAfterShutdown("ge_update_assets"))
        return kFailed;
    if (RefuseWhileQueryRuns("ge_update_assets"))
        return kFailed;
    if (!GameEngine::EngineCore::GetInstance().IsInitialized())
    {
        SetLastError("ge_update_assets was called before ge_create succeeded.");
        return kFailed;
    }
    GameEngine::EngineCore::GetInstance().GetAssetManager().Update();
    AdvanceAssetLoads();
    return kOk;
}

/// Sizes the drawing buffer to the canvas's CSS size times the device pixel ratio.
EMSCRIPTEN_KEEPALIVE int32_t ge_resize(int32_t width, int32_t height, double devicePixelRatio)
{
    const AbiCallScope scope("ge_resize");
    if (scope.Refused())
        return kFailed;
    if (RefuseUnlessRunning("ge_resize"))
        return kFailed;
    if (width <= 0 || height <= 0 || !(devicePixelRatio > 0.0))
    {
        SetLastError("ge_resize needs a positive size and pixel ratio (got {} x {} at {}).", width, height,
                     devicePixelRatio);
        return kFailed;
    }
    g_Application->Resize(width, height);
    return kOk;
}

/// Stops the engine and releases the device; every later call fails. A running query ends with
/// it: a page may dispose of the engine from inside a query's callback.
EMSCRIPTEN_KEEPALIVE int32_t ge_shutdown()
{
    const AbiCallScope scope("ge_shutdown");
    if (scope.Refused())
        return kFailed;
    if (RefuseUnlessRunning("ge_shutdown"))
        return kFailed;
    DropAssetLoads();
    ReleaseQueries();
    g_Application->Shutdown();
    g_Application.reset();
    g_State = LifecycleState::Finished;
    return kOk;
}

} // extern "C"
