#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>

// Build this shim as the scripting module and include public ABI once
#define GE_SCRIPTING_BUILD 1
#include "Scripting/ScriptingABI.h"

static GE_Result GE_CDECL Shim_Log(GE_LogLevel /*level*/, const char* /*msg*/, uint32_t /*len*/) { return GE_Result_Ok; }
static GE_Result GE_CDECL Shim_RegisterManagedCallback(GE_CallbackId /*id*/, void* /*fnPtr*/) { return GE_Result_Ok; }
static GE_Result GE_CDECL Shim_GetManagedCallback(GE_CallbackId /*id*/, void** outFnPtr) { if(outFnPtr) *outFnPtr = nullptr; return GE_Result_Ok; }
static GE_Result GE_CDECL Shim_SubscribeDiagnostics(const GE_DiagnosticsSink* /*sink*/) { return GE_Result_Ok; }
static GE_Result GE_CDECL Shim_UnsubscribeDiagnostics(const GE_DiagnosticsSink* /*sink*/) { return GE_Result_Ok; }
static GE_Result GE_CDECL Shim_NotifyCompilationEvent(GE_CompilationStage /*stage*/, float /*progress01*/, const GE_Diagnostic* /*diags*/, uint32_t /*diagCount*/) { return GE_Result_Ok; }
static GE_Result GE_CDECL Shim_NotifyReloadEvent(GE_ReloadStage /*stage*/, const char* /*reasonUtf8*/) { return GE_Result_Ok; }
static GE_Result GE_CDECL Shim_GetAssetCount(int32_t* outCount) { if(outCount) *outCount = 0; return GE_Result_Ok; }
static GE_Result GE_CDECL Shim_ScriptsDomainSwap(GE_DomainHandle /*newDomain*/) { return GE_Result_Ok; }
static GE_Result GE_CDECL Shim_ScriptsDomainUnload(GE_DomainHandle /*domain*/) { return GE_Result_Ok; }

static GE_Result GE_CDECL Shim_ECS_GetWorldHandle(GE_Handle* outWorld) { if(outWorld) *outWorld = (GE_Handle)1; return GE_Result_Ok; }
static GE_Result GE_CDECL Shim_ECS_GetEntityCount(GE_Handle /*world*/, int32_t* outCount) { if(outCount) *outCount = 0; return GE_Result_Ok; }

static const GE_Interface_v1 g_InterfaceV1 = {
    /*sizeBytes*/ (uint32_t)sizeof(GE_Interface_v1),
    /*abiVersion*/ GE_ABI_VERSION_CURRENT,
    /*Log*/ Shim_Log,
    /*RegisterManagedCallback*/ Shim_RegisterManagedCallback,
    /*NotifyCompilationEvent*/ Shim_NotifyCompilationEvent,
    /*NotifyReloadEvent*/ Shim_NotifyReloadEvent,
    /*GetManagedCallback*/ Shim_GetManagedCallback,
    /*SubscribeDiagnostics*/ Shim_SubscribeDiagnostics,
    /*UnsubscribeDiagnostics*/ Shim_UnsubscribeDiagnostics,
    /*GetAssetCount*/ Shim_GetAssetCount,
    /*ScriptsDomainSwap*/ Shim_ScriptsDomainSwap,
    /*ScriptsDomainUnload*/ Shim_ScriptsDomainUnload,
    /*ECS_GetWorldHandle*/ Shim_ECS_GetWorldHandle,
    /*ECS_GetEntityCount*/ Shim_ECS_GetEntityCount
};

extern "C" {

GE_API uint32_t GE_CDECL GE_ScriptingGetAbiVersion(void)
{
    return GE_ABI_VERSION_CURRENT;
}

GE_API GE_Result GE_CDECL GE_GetInterface(uint32_t abiVersion, const void** outTable, uint32_t* outSizeBytes)
{
    if (outTable) *outTable = nullptr;
    if (outSizeBytes) *outSizeBytes = 0u;

    if (GE_ABI_VERSION_MAJOR(abiVersion) != GE_ABI_VERSION_MAJOR(GE_ABI_VERSION_CURRENT))
        return GE_Result_Fail; // incompatible major

    if (outTable) *outTable = &g_InterfaceV1;
    if (outSizeBytes) *outSizeBytes = (uint32_t)sizeof(GE_Interface_v1);
    return GE_Result_Ok;
}


    // HotReload preloaded context exports required by Engine hot-reload tasks (no-op stubs for tests)
    GE_API GE_Result GE_CDECL GE_PreloadAssemblyContext(const char* /*assemblyPath*/, uint32_t /*len*/) { return GE_Result_Ok; }
    GE_API GE_Result GE_CDECL GE_SwapPreloadedContext() { return GE_Result_Ok; }
    GE_API GE_Result GE_CDECL GE_CleanupOldContext(const char* /*assemblyPath*/, uint32_t /*len*/) { return GE_Result_Ok; }

// Keep legacy exports (no-op) for now
GE_API GE_Result GE_CDECL GE_RegisterManagedCallback(GE_CallbackId /*id*/, void* /*fnPtr*/) { return GE_Result_Ok; }
GE_API GE_Result GE_CDECL GE_Log(GE_LogLevel /*level*/, const char* /*msg*/, uint32_t /*len*/) { return GE_Result_Ok; }

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID)
{
    return TRUE;
}

} // extern "C"
#endif

