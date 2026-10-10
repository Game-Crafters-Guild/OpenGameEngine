#pragma once

// GE_API — symbol export/import macro for the Engine shared library.
//
// Phase 1 of the C++ scripting plan promotes Engine from STATIC to SHARED so
// that consumers (Editor, Player, GameEngine.Native, future user DLLs) link
// against a single canonical Engine.dll/dylib/so instead of each re-linking a
// static copy. This header defines the macro every annotated public symbol
// uses to declare which side of the DLL boundary it lives on.
//
// Semantics:
//   - Compiling Engine.dll itself:           GE_API = __declspec(dllexport) (Win)
//                                            GE_API = [[gnu::visibility("default")]] (POSIX)
//   - Compiling a consumer (Editor, etc.):   GE_API = __declspec(dllimport) (Win)
//                                            GE_API = (empty)               (POSIX)
//
// Annotation policy during the staged rollout:
//   * Until visibility lockdown lands in the final Phase 1 commit, Engine is
//     SHARED but uses WINDOWS_EXPORT_ALL_SYMBOLS=ON on Windows and the default
//     (permissive) visibility on POSIX. GE_API annotations are accepted but
//     don't yet gate exports — they're harmless and forward-compatible.
//   * Each annotation batch progressively decorates a module's public headers.
//     Builds remain green on every platform throughout, because the export
//     defaults are still permissive.
//   * The final commit flips CXX_VISIBILITY_PRESET=hidden and
//     WINDOWS_EXPORT_ALL_SYMBOLS=OFF in one auditable step. Only annotated
//     symbols are exported from that point on.
//
// Visibility and export are different things: hiding a symbol is a compiler
// setting, exporting one is what GE_API annotates. Both stay permissive until
// the final commit, so an un-annotated symbol still links until then.

#if defined(_WIN32)
  #if defined(GAMEENGINE_BUILD_DLL)
    #define GE_API __declspec(dllexport)
  #else
    #define GE_API __declspec(dllimport)
  #endif
#elif defined(__GNUC__) || defined(__clang__)
  #define GE_API __attribute__((visibility("default")))
#else
  #define GE_API
#endif
