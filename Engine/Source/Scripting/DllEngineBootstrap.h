#pragma once

// DllEngineBootstrap.h - Decides whether a GE_* ABI entry point initializes the
// process's EngineCore.
//
// Engine is a shared library, so every module in the process (the host executable,
// GameEngine.Native, the managed runtime's callbacks) sees the one EngineCore
// singleton, and whoever calls EngineCore::Initialize first owns its lifecycle:
//
//  - A native host (Editor, Player, a test harness) initializes the engine itself.
//    From its first Initialize on, the ABI never starts the engine: not while that
//    Initialize runs on another thread, not after Shutdown, not after a failed run.
//  - Standalone: managed code loads GameEngine.Native with no native host (e.g.
//    `dotnet test`), so nobody has initialized the engine when the first ABI call
//    arrives, and that call initializes it here, configured from GE_NATIVE_DIR and
//    GE_SetHostScriptsConfig.
//
// Either way an engine that is initializing on the calling thread is left alone:
// ScriptManager starts CoreCLR inside EngineCore::Initialize and the managed side
// calls straight back into GE_* on the same thread.

#include "Core/Engine.h"
#include "Scripting/ScriptsConfig.h"
#include "Scripting/ScriptingABI.h"

#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>

namespace GameEngine::DllBootstrap
{

// ---- Host ScriptsConfig ----
// Written by GE_SetHostScriptsConfig; applied by the standalone bootstrap below.

inline std::mutex& GetHostConfigMutex()
{
    static std::mutex s_Mutex;
    return s_Mutex;
}

inline std::optional<ScriptsConfig>& GetHostScriptsConfig()
{
    static std::optional<ScriptsConfig> s_Config;
    return s_Config;
}

inline void SetHostScriptsConfig(const ScriptsConfig& config)
{
    std::lock_guard<std::mutex> lock(GetHostConfigMutex());
    GetHostScriptsConfig() = config;
}

// ---- Standalone bootstrap ----

// Serializes first callers: one that arrives while another thread's standalone
// Initialize is running blocks here until that Initialize has completed.
inline std::mutex& GetBootstrapMutex()
{
    static std::mutex s_Mutex;
    return s_Mutex;
}

inline GE_Result EnsureEngineInitialized()
{
    auto& eng = EngineCore::GetInstance();

    // Running, or initializing on this very thread (ScriptManager -> CoreCLR -> GE_*):
    // nothing to do. The second case must return before the lock below, which the
    // standalone Initialize on this thread may be holding.
    if (eng.IsInitialized() || eng.IsInitializingOnCurrentThread())
        return GE_Result_Ok;

    std::lock_guard<std::mutex> lock(GetBootstrapMutex());
    if (eng.IsInitialized())
        return GE_Result_Ok;

    // Some caller in this process drives the lifecycle: its Initialize is running on
    // another thread, or it has shut the engine down, or its Initialize failed. The
    // engine is that caller's to start, never the ABI's.
    if (eng.HasEverInitialized())
        return GE_Result_NotInitialized;

    ApplicationConfig cfg;
    if (const char* nativeDirEnv = std::getenv("GE_NATIVE_DIR"); nativeDirEnv && nativeDirEnv[0] != '\0')
    {
        std::filesystem::path nativeDir(nativeDirEnv);
        cfg.WorkspaceDirectory = nativeDir.string();
        cfg.AssetDirectory = (nativeDir / "Assets").string();
    }
    else
    {
        cfg.AssetDirectory = "Assets";
    }

    {
        std::lock_guard<std::mutex> configLock(GetHostConfigMutex());
        if (GetHostScriptsConfig().has_value())
            eng.SetScriptsConfig(*GetHostScriptsConfig());
    }

    return eng.Initialize(cfg) ? GE_Result_Ok : GE_Result_Fail;
}

} // namespace GameEngine::DllBootstrap
