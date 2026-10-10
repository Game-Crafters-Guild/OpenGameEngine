// Minimal HotReload native stubs for UITests so we don't depend on the
// full GameEngine.Native binary when linking against Scripting.
//
// NOTE: We intentionally omit GE_API here because in this test build the
// declarations from ScriptingABI.h are marked dllimport; defining them with
// GE_API would cause C2491 ("definition of dllimport function not allowed").
// This may produce C4273 warnings about inconsistent DLL linkage, which is
// acceptable for this isolated test binary.

#include "Scripting/ScriptingABI.h"

extern "C"
{
    GE_Result GE_CDECL GE_PreloadAssemblyContext(const char* /*assemblyPathUtf8*/, uint32_t /*len*/)
    {
        return GE_Result_Ok;
    }

    GE_Result GE_CDECL GE_SwapPreloadedContext()
    {
        return GE_Result_Ok;
    }

    GE_Result GE_CDECL GE_CleanupOldContext(const char* /*assemblyPathUtf8*/, uint32_t /*len*/)
    {
        return GE_Result_Ok;
    }

    // Incremental compilation cache API stubs used by Engine's IncrementalCompilationTask.
    // These are no-ops for UITests so we don't pull in the full GameEngine.Native shim.
    GE_Result GE_CDECL GE_ClearCompilerCache(void)
    {
        return GE_Result_Ok;
    }

    GE_Result GE_CDECL GE_GetCompilerStats(void)
    {
        return GE_Result_Ok;
    }
}
