#pragma once

// AbiMainThread.h - The scripting ABI's rule for exports that may run only on the engine's main thread.

#include "Core/Engine.h"
#include "Scripting/ScriptManager.h"

namespace GameEngine::ScriptingAbi
{

/**
 * @brief True on the thread that runs the engine's frames, the only thread that mutates the state such an
 * export reads (the UI tree, a model's payload during a hot reload).
 *
 * A host that never marked a main thread answers true. That is not a weakening: it is the standalone-host
 * case (gtest, dotnet test), where no frame loop exists, nothing else mutates that state, and the caller is
 * the only thread. Refusing there would fail every suite that drives an export without an engine, and would
 * protect nothing. The ScriptManager is resolved per call because EngineCore::Initialize and Shutdown
 * replace it.
 */
inline bool OnEngineMainThread()
{
    const ScriptManager& scripts = EngineCore::GetInstance().GetScriptManager();
    return !scripts.HasMarkedMainThread() || scripts.IsMainThread();
}

} // namespace GameEngine::ScriptingAbi
