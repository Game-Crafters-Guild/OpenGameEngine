// The renamed exports deliberately model older/incomplete modules. The other
// variants use the real ABI with a selectable rejection at each later gate.
#if defined(GE_PROBE_MISSING_ABI)
#define GE_UserModule_AbiVersion_v1 GE_Probe_UnusedAbiVersion
#endif
#if defined(GE_PROBE_MISSING_FINGERPRINT)
#define GE_UserModule_ToolchainFingerprint_v1 GE_Probe_UnusedFingerprint
#endif
#include "NativeScripting/NativeScriptingABI.h"

namespace
{
int g_Mode = 0;
int g_RegisterCalls = 0;
int* g_DetachCount = nullptr;
struct DetachProbe
{
    ~DetachProbe() { if (g_DetachCount) ++*g_DetachCount; }
} g_DetachProbe;
}

extern "C"
{
GE_USERAPI void GE_RejectionProbe_Configure(int mode, int* detachCount)
{
    g_Mode = mode;
    g_DetachCount = detachCount;
}

GE_USERAPI int GE_RejectionProbe_RegisterCalls() { return g_RegisterCalls; }

GE_USERAPI uint32_t GE_UserModule_AbiVersion_v1()
{
    if (g_Mode == 4) return 1u; // adapter vtable before OnPostSimulation
    return GE_USERMODULE_ABI_VERSION + (g_Mode == 1 ? 1u : 0u);
}

GE_USERAPI void GE_UserModule_ToolchainFingerprint_v1(GE_ToolchainFingerprint* out)
{
    GE_FillToolchainFingerprint(out);
    if (g_Mode == 2)
        ++out->CompilerVersionMinor;
}

GE_USERAPI int GE_UserModule_Register_v1(const GE_UserModuleHostApi*)
{
    ++g_RegisterCalls;
    return g_Mode == 3 ? 17 : 0;
}

GE_USERAPI void GE_UserModule_Unregister_v1(const GE_UserModuleHostApi*) {}
}
