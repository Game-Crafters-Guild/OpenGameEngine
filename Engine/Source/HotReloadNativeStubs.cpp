// HotReloadNativeStubs.cpp — Engine.dll's dispatcher for the five hot-reload
// entry points (GE_PreloadAssemblyContext, GE_SwapPreloadedContext,
// GE_CleanupOldContext, GE_ClearCompilerCache, GE_GetCompilerStats).
//
// Why a dispatcher and not a direct link to GameEngine.Native:
//   Engine.dll's Jobs/HotReloadTasks.cpp and Jobs/IncrementalCompilationTask.cpp
//   call these entry points. The real CoreCLR-backed implementations live in
//   GameEngine.Native.dll (Source/Scripting/ScriptingABI.cpp). If Engine.dll
//   PRIVATE-linked GameEngine.Native to pull those impls in, the two SHARED
//   libraries form a link cycle that the linker can't resolve.
//
// Architecture:
//   - Engine.dll always exports the five GE_* entry points (so editor /
//     player code paths can link Engine alone without dragging in
//     GameEngine.Native's import lib).
//   - Each entry point reads a function pointer from g_hotReloadCallbacks
//     and forwards. Default is null → return GE_Result_Ok so unscripted
//     builds (CLR disabled, headless tests) keep working.
//   - GameEngine.Native.dll's GE_HotReloadRegister.cpp installs its real
//     impls via GE_RegisterHotReloadCallbacks() at module load (global
//     ctor — runs after Engine.dll's static init since GameEngine.Native
//     depends on Engine.dll).

#include "Scripting/ScriptingABI.h"

#include <atomic>

namespace {

struct CallbackSlots {
    std::atomic<GE_PreloadAssemblyContext_Fn> Preload{nullptr};
    std::atomic<GE_SwapPreloadedContext_Fn>   Swap{nullptr};
    std::atomic<GE_CleanupOldContext_Fn>      Cleanup{nullptr};
    std::atomic<GE_ClearCompilerCache_Fn>     ClearCache{nullptr};
    std::atomic<GE_GetCompilerStats_Fn>       GetStats{nullptr};
};

CallbackSlots& Slots() {
    // Function-local static — zero-initialized on first call, thread-safe under
    // C++11 magic statics. Avoids cross-DLL static-init order issues.
    static CallbackSlots s;
    return s;
}

} // namespace

extern "C" {

void GE_CDECL GE_RegisterHotReloadCallbacks(const GE_HotReloadCallbacks* cb)
{
    auto& s = Slots();
    if (cb == nullptr) {
        s.Preload   .store(nullptr, std::memory_order_release);
        s.Swap      .store(nullptr, std::memory_order_release);
        s.Cleanup   .store(nullptr, std::memory_order_release);
        s.ClearCache.store(nullptr, std::memory_order_release);
        s.GetStats  .store(nullptr, std::memory_order_release);
        return;
    }
    s.Preload   .store(cb->PreloadAssemblyContext, std::memory_order_release);
    s.Swap      .store(cb->SwapPreloadedContext,   std::memory_order_release);
    s.Cleanup   .store(cb->CleanupOldContext,      std::memory_order_release);
    s.ClearCache.store(cb->ClearCompilerCache,     std::memory_order_release);
    s.GetStats  .store(cb->GetCompilerStats,       std::memory_order_release);
}

GE_Result GE_CDECL GE_PreloadAssemblyContext(const char* assemblyPathUtf8, uint32_t pathLen)
{
    auto fn = Slots().Preload.load(std::memory_order_acquire);
    return fn ? fn(assemblyPathUtf8, pathLen) : GE_Result_Ok;
}

GE_Result GE_CDECL GE_SwapPreloadedContext(void)
{
    auto fn = Slots().Swap.load(std::memory_order_acquire);
    return fn ? fn() : GE_Result_Ok;
}

GE_Result GE_CDECL GE_CleanupOldContext(const char* assemblyPathUtf8, uint32_t pathLen)
{
    auto fn = Slots().Cleanup.load(std::memory_order_acquire);
    return fn ? fn(assemblyPathUtf8, pathLen) : GE_Result_Ok;
}

GE_Result GE_CDECL GE_ClearCompilerCache(void)
{
    auto fn = Slots().ClearCache.load(std::memory_order_acquire);
    return fn ? fn() : GE_Result_Ok;
}

GE_Result GE_CDECL GE_GetCompilerStats(void)
{
    auto fn = Slots().GetStats.load(std::memory_order_acquire);
    return fn ? fn() : GE_Result_Ok;
}

} // extern "C"
