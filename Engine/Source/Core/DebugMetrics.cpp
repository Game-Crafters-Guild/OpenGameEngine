#include "Core/DebugMetrics.h"

namespace GameEngine::Debug
{

// Defined here rather than header-inline so the function-local static has
// exactly ONE storage location: inside Engine.dll's data segment, exported via
// the generated exports.def. Every module that links Engine's import lib
// (Editor.exe, its panels, the debug server) resolves this same instance, so
// samples pushed from Engine.dll's Application::Tick are visible to the
// editor-side Monitors panel that reads them. When Get() was header-inline each
// module compiled its own function-local static, splitting producers (Tick)
// from readers (panels) across separate singletons — the root cause of the
// empty Monitors "Time/*" charts.
//
// KNOWN LIMIT: GameEngine.Native.dll statically links its own copy of Engine
// (GE_SCRIPTING_STATIC), so code executing inside that DLL still sees a separate
// DebugMetrics instance. This is the pre-existing dual-static family; if a
// script-side producer ever needs to publish into the shared instance, the
// GE_SetHost* redirection pattern would apply. Intentionally not addressed here.
DebugMetrics& DebugMetrics::Get()
{
    static DebugMetrics s;
    return s;
}

} // namespace GameEngine::Debug
