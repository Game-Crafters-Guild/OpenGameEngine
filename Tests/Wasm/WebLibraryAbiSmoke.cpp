// The WebLibrary module's exports under node (Tests/Wasm/WebLibraryAbiSmoke.mjs drives them):
// the same ABI objects the page's module links, with this headless boot in place of
// ge_create, whose window and WebGPU device need a browser page. The boot brings up the
// engine and an empty primary world exactly as ge_create does once its device exists.

#include "AbiErrors.h"

#include "Assets/AssetManager.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/Entity.h"
#include "Logger/Logger.h"
#include "Scripting/ScriptsConfig.h"

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace
{

std::unique_ptr<GameEngine::Application> g_Application;

// malloc calls on the main thread, where the ABI runs: the driver reads it around ABI calls.
std::atomic<unsigned> g_MainThreadMallocs{0};

// Waits on the promise the driver puts in Module.smokeGate: a suspension the driver ends.
EM_ASYNC_JS(void, WaitForSmokeGate, (), { await Module['smokeGate']; });

} // namespace

extern "C"
{

void* __real_malloc(std::size_t size);

/// Every malloc in this module, operator new's included (the link wraps malloc: Tests/Wasm/CMakeLists.txt).
void* __wrap_malloc(std::size_t size)
{
    if (emscripten_is_main_runtime_thread())
        g_MainThreadMallocs.fetch_add(1, std::memory_order_relaxed);
    return __real_malloc(size);
}

/// Boots the engine headless with an empty primary world; 0 on success.
EMSCRIPTEN_KEEPALIVE int smoke_boot()
{
    using namespace GameEngine;
    if (g_Application)
        return 0;
    Logger::Log::Config logConfig;
    logConfig.GlobalMinLevel = Logger::LogLevel::Warning;
    Logger::Log::Initialize(logConfig);

    ScriptsConfig scriptsConfig;
    scriptsConfig.disableClr = true;
    scriptsConfig.enableHotReload = false;
    EngineCore::GetInstance().SetScriptsConfig(scriptsConfig);

    ApplicationConfig config;
    config.Name = "WebLibraryAbiSmoke";
    config.AssetDirectory = "/Assets";
    config.WorkspaceDirectory = "/.workspace";
    g_Application = std::make_unique<Application>(config);
    if (!g_Application->Initialize())
        return 1;
    EngineCore::GetInstance().GetAssetManager().WaitForStartupScan(kAssetSourceAliasProject);
    return EngineCore::GetInstance().EnsurePrimaryWorld() ? 0 : 2;
}

/// The write-grant version of the entity's `typeId` column (World::GetEntityColumnVersion): it
/// rises when a write to that column is granted, which is what a Changed<T> system filters on.
EMSCRIPTEN_KEEPALIVE double smoke_column_version(uint32_t entityId, uint64_t typeId)
{
    using namespace GameEngine;
    const ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld();
    return world ? static_cast<double>(world->GetEntityColumnVersion(ECS::EntityHandle(entityId), typeId)) : -1.0;
}

/// The malloc calls made on the main thread since the module started.
EMSCRIPTEN_KEEPALIVE unsigned smoke_main_thread_mallocs()
{
    return g_MainThreadMallocs.load(std::memory_order_relaxed);
}

/// Suspends, as ge_shutdown does while the GPU finishes, inside an export's call scope, until the
/// driver resolves Module.smokeGate; 0 when it ran, -1 when the scope refused it.
EMSCRIPTEN_KEEPALIVE int smoke_suspend()
{
    const GameEngine::WebLibrary::AbiCallScope scope("smoke_suspend");
    if (scope.Refused())
        return -1;
    WaitForSmokeGate();
    return 0;
}

} // extern "C"
