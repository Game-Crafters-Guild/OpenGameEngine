// UserModuleEntry.cpp — default implementations of the four user-module ABI
// exports. The generated user-project build compiles this into every native user
// DLL, so users never write the exports themselves.
//
// In C10, components self-register via GE_REGISTER_COMPONENT's static initializer
// at DLL load (proven single-TU-safe by the cross-DLL smoke test), so Register_v1
// is a no-op. C12 replaces static-init with an explicit ge_reg linker-section walk
// here, plus a symmetric Unregister_v1.

#include "NativeScripting/NativeScriptingABI.h"

// The allocation counter's hook, so this module's allocations count into the engine's
// counter where the engine carries its instrumentation (the SDK defines
// GE_DEBUG_INSTRUMENTATION to the engine's value).
#if GE_DEBUG_INSTRUMENTATION
#include "Memory/AllocationHook.inl"
#endif

extern "C" {

GE_USERAPI uint32_t GE_UserModule_AbiVersion_v1(void)
{
    return GE_USERMODULE_ABI_VERSION;
}

GE_USERAPI void GE_UserModule_ToolchainFingerprint_v1(GE_ToolchainFingerprint* out)
{
    // Captures THIS DLL's compiler / CRT / STL macros (this TU is compiled by the
    // user-project build, not the engine). The host compares field-by-field before
    // Register_v1 and refuses the module on any difference.
    GE_FillToolchainFingerprint(out);
}

GE_USERAPI int GE_UserModule_Register_v1(const GE_UserModuleHostApi* host)
{
    (void)host; // C10: components already registered via static init at DLL load.
    return 0;
}

GE_USERAPI void GE_UserModule_Unregister_v1(const GE_UserModuleHostApi* host)
{
    (void)host; // C12: walk the ge_reg section in reverse + drop engine-side refs.
}

} // extern "C"
