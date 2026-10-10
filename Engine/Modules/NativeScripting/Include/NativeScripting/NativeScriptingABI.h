#ifndef GE_NATIVE_SCRIPTING_ABI_H
#define GE_NATIVE_SCRIPTING_ABI_H

// NativeScriptingABI.h — the C ABI contract between the engine host and a native
// user-script DLL. Phase 2 commit C9 (skeleton): the load/unload handshake only.
//
// Design (the "hybrid" model): a user DLL links the Engine import lib and calls the
// engine's real C++ API directly for the bulk of its work (world.View<T>(),
// GE_REGISTER_COMPONENT, etc.) — exactly like an Unreal module. This header is NOT
// a wrapper over that API. It is the small, mangling-stable, C-only seatbelt that
// makes safe hot-reload possible:
//
//   * Version + toolchain handshake. The host MUST be able to verify a freshly
//     built DLL is ABI-compatible BEFORE running any of its C++ — because if the
//     STL/CRT ABI differs, even calling a C++ function to ask "are you compatible?"
//     can corrupt memory. AbiVersion / ToolchainFingerprint are plain integers, so
//     they are safe to call across a mismatched toolchain. Mismatch -> refuse +
//     unload, with nothing corrupted.
//   * Register / Unregister bracket the user DLL's lifetime so the engine can
//     attach on load and sever every cached reference into the DLL before unload.
//
// Everything here is C-compatible (no STL, no C++ types) so the contract is stable
// across compilers and toolchains. The bulk gameplay API lives in the normal
// engine headers the user links against, not here.

#include <stdint.h>

// The STL identity macros the fingerprint below captures (_MSVC_STL_UPDATE,
// _LIBCPP_VERSION, _GLIBCXX_RELEASE) are defined by the standard library headers,
// not the compiler. <version> is the cheapest header guaranteed to define them, so
// a TU that includes nothing else from the STL (e.g. the user DLL's module entry)
// still fingerprints the real standard library instead of 0.
#ifdef __cplusplus
#include <version>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Bumped whenever this ABI changes shape. A DLL built against a different value is
// refused at load with a precise diagnostic.
// Version 2 adds optional post-simulation dispatch and the exclusive ECS
// system execution contract to the hybrid C++ interface.
#define GE_USERMODULE_ABI_VERSION 2u

// Export attribute for the four required user-DLL entry points. The host resolves
// them with GetProcAddress / dlsym, so there is no import side.
#if defined(_WIN32)
#  define GE_USERAPI __declspec(dllexport)
#else
#  define GE_USERAPI __attribute__((visibility("default")))
#endif

// ---------------------------------------------------------------------------
// Toolchain fingerprint — catches "same engine version, incompatible build".
//
// ABI version catches a stale DLL against a newer engine. It does NOT catch the
// worse class: same engine, different MSVC update, a different _ITERATOR_DEBUG_LEVEL
// (Debug vs Release STL changes container layouts!), or differing /Zc flags. Those
// load and then crash on the first STL operation, far from the cause. The host
// compares each field on load and refuses with the specific field that differs.
typedef struct GE_ToolchainFingerprint
{
    uint32_t CompilerVendor;       // GE_COMPILER_* enum (MSVC / Clang / GCC)
    uint32_t CompilerVersionMajor; // _MSC_VER / __clang_major__ / __GNUC__
    uint32_t CompilerVersionMinor; // _MSC_FULL_VER / __clang_minor__ / __GNUC_MINOR__
    uint32_t CrtId;                // GE_CRT_FLAG_* bits: debug vs release CRT, static vs DLL
    uint32_t IteratorDebugLevel;   // _ITERATOR_DEBUG_LEVEL (MSVC) / _GLIBCXX_DEBUG (libstdc++)
    uint32_t ZcFlags;              // GE_ZC_FLAG_* conformance bits (/Zc:wchar_t, EH, RTTI, /J)
    uint32_t StdLibAbiTag;         // _MSVC_STL_UPDATE / _LIBCPP_VERSION / _GLIBCXX_RELEASE
    uint32_t _Padding;
} GE_ToolchainFingerprint;

#define GE_COMPILER_UNKNOWN 0u
#define GE_COMPILER_MSVC    1u
#define GE_COMPILER_CLANG   2u
#define GE_COMPILER_GCC     3u

#define GE_CRT_FLAG_DEBUG   0x1u // debug CRT (/MDd, /MTd): _DEBUG
#define GE_CRT_FLAG_DLL     0x2u // dynamic CRT (/MD, /MDd): _DLL

#define GE_ZC_FLAG_NATIVE_WCHAR_T 0x1u // /Zc:wchar_t: _NATIVE_WCHAR_T_DEFINED
#define GE_ZC_FLAG_EXCEPTIONS     0x2u // /EH: _CPPUNWIND / __cpp_exceptions
#define GE_ZC_FLAG_RTTI           0x4u // /GR: _CPPRTTI / __GXX_RTTI
#define GE_ZC_FLAG_CHAR_UNSIGNED  0x8u // /J: _CHAR_UNSIGNED

// Populate `out` from the CALLING translation unit's compiler / CRT / STL macros.
// Compiled into both sides of the boundary — the user DLL's UserModuleEntry.cpp and
// the engine host — so each captures the toolchain it was actually built with, and
// a field-by-field compare is exact. static inline (not an export): the whole point
// is that the values are baked per-binary at compile time.
static inline void GE_FillToolchainFingerprint(GE_ToolchainFingerprint* out)
{
    if (!out)
        return;
    out->CompilerVendor = GE_COMPILER_UNKNOWN;
    out->CompilerVersionMajor = 0u;
    out->CompilerVersionMinor = 0u;
    out->CrtId = 0u;
    out->IteratorDebugLevel = 0u;
    out->ZcFlags = 0u;
    out->StdLibAbiTag = 0u;
    out->_Padding = 0u;

#if defined(__clang__)
    out->CompilerVendor = GE_COMPILER_CLANG;
    out->CompilerVersionMajor = (uint32_t)__clang_major__;
    out->CompilerVersionMinor = (uint32_t)__clang_minor__;
#elif defined(_MSC_VER)
    out->CompilerVendor = GE_COMPILER_MSVC;
    out->CompilerVersionMajor = (uint32_t)_MSC_VER;
    out->CompilerVersionMinor = (uint32_t)_MSC_FULL_VER;
#elif defined(__GNUC__)
    out->CompilerVendor = GE_COMPILER_GCC;
    out->CompilerVersionMajor = (uint32_t)__GNUC__;
    out->CompilerVersionMinor = (uint32_t)__GNUC_MINOR__;
#endif

#if defined(_DEBUG)
    out->CrtId |= GE_CRT_FLAG_DEBUG;
#endif
#if defined(_DLL)
    out->CrtId |= GE_CRT_FLAG_DLL;
#endif

    // _ITERATOR_DEBUG_LEVEL lives in MSVC's <yvals.h> (not <yvals_core.h>, so
    // <version> does not define it). When no STL container header set it and no /D
    // override made it a command-line define, apply MSVC's own default: 2 for the
    // debug CRT, 0 otherwise — the same value any STL include in this TU would get.
#if defined(_ITERATOR_DEBUG_LEVEL)
    out->IteratorDebugLevel = (uint32_t)_ITERATOR_DEBUG_LEVEL;
#elif defined(_MSC_VER) && defined(_DEBUG)
    out->IteratorDebugLevel = 2u;
#elif defined(_GLIBCXX_DEBUG)
    out->IteratorDebugLevel = 1u;
#endif

#if defined(_NATIVE_WCHAR_T_DEFINED)
    out->ZcFlags |= GE_ZC_FLAG_NATIVE_WCHAR_T;
#endif
#if defined(_CPPUNWIND) || defined(__cpp_exceptions)
    out->ZcFlags |= GE_ZC_FLAG_EXCEPTIONS;
#endif
#if defined(_CPPRTTI) || defined(__GXX_RTTI)
    out->ZcFlags |= GE_ZC_FLAG_RTTI;
#endif
#if defined(_CHAR_UNSIGNED)
    out->ZcFlags |= GE_ZC_FLAG_CHAR_UNSIGNED;
#endif

#if defined(_MSVC_STL_UPDATE)
    out->StdLibAbiTag = (uint32_t)_MSVC_STL_UPDATE;
#elif defined(_LIBCPP_VERSION)
    out->StdLibAbiTag = (uint32_t)_LIBCPP_VERSION;
#elif defined(_GLIBCXX_RELEASE)
    out->StdLibAbiTag = (uint32_t)_GLIBCXX_RELEASE;
#endif
}

// ---------------------------------------------------------------------------
// Service discovery — the host hands the user DLL a registry it can query for the
// few host services that must cross the boundary as a C-stable interface (component
// / system registration). The bulk API is reached by direct linking, NOT through
// here, so this stays small. Returning a versioned interface by id keeps the ABI
// stable: later commits add services without changing GE_UserModuleHostApi's layout.
typedef uint64_t GE_ServiceId;

typedef struct GE_ServiceRegistry
{
    uint32_t StructSize;  // sizeof(GE_ServiceRegistry) the host built — growth guard
    uint32_t AbiVersion;  // GE_USERMODULE_ABI_VERSION the host speaks

    // Return a pointer to a versioned host-service interface (its own struct of
    // function pointers), or null if the service is unknown or older than minVersion.
    const void* (*GetService)(GE_ServiceId id, uint32_t minVersion);
} GE_ServiceRegistry;

// Passed to Register_v1 / Unregister_v1. Carries the version the host speaks and
// the service registry. The user DLL shares the engine's singletons through the
// Engine import lib it links, so no engine context pointer is needed here.
typedef struct GE_UserModuleHostApi
{
    uint32_t                  StructSize; // sizeof(GE_UserModuleHostApi) — growth guard
    uint32_t                  AbiVersion; // GE_USERMODULE_ABI_VERSION the host speaks
    const GE_ServiceRegistry* Services;
} GE_UserModuleHostApi;

// ---------------------------------------------------------------------------
// The four required exports a native user-script DLL must provide.
//
//   AbiVersion / ToolchainFingerprint : pre-flight checks, called BEFORE any of the
//     DLL's C++ runs. Plain integers only — safe across a mismatched toolchain.
//   Register   : called once after the checks pass; the DLL wires its components/
//                systems into the engine (in C9 this is just the contract).
//   Unregister : called immediately before FreeLibrary/dlclose so the DLL can drop
//                anything the engine-side sweep cannot reach.
//
// Implementations live in the user DLL; the SDK header (later) provides default
// definitions that walk the linker-collected registration section.
GE_USERAPI uint32_t GE_UserModule_AbiVersion_v1(void);
GE_USERAPI void     GE_UserModule_ToolchainFingerprint_v1(GE_ToolchainFingerprint* out);
GE_USERAPI int      GE_UserModule_Register_v1(const GE_UserModuleHostApi* host);
GE_USERAPI void     GE_UserModule_Unregister_v1(const GE_UserModuleHostApi* host);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // GE_NATIVE_SCRIPTING_ABI_H
