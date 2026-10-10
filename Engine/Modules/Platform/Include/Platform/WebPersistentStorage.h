#pragma once

#include <filesystem>

namespace GameEngine {
namespace Platform {

// Origin-scoped persistent storage for the browser build. There is no desktop
// counterpart: a native process already has a writable filesystem, and every
// other mount in a page (MEMFS, the preloaded chrome) dies with the tab.
//
// Backed by the Origin Private File System through WasmFS, so the mount is a
// real POSIX subtree — std::filesystem, fopen and stat work against it, and
// file contents stay in the browser's storage rather than in linear memory.
//
// Link requirement: the OPFS backend lives in WasmFS, so an executable that
// calls this must link with `-s WASMFS`. Without it the call does not reach a
// stub, it fails to link on `wasmfs_create_opfs_backend`.
namespace Web {

/// Projects, one directory each. Also where an imported folder is copied to,
/// since a page cannot open a project in place on the user's disk.
inline constexpr const char* kProjectsMount = "/project";

/// Persistent user-data and cache roots, as HIDDEN subtrees of the projects
/// mount. They cannot be their own mounts: WasmFS's OPFS backend hands every
/// mounted directory the same OPFS root (OPFSDirectory id 1), so a second
/// mount would alias the first, not sit beside it. Dot-named so nothing that
/// browses the projects tree mistakes them for a project; the picker itself
/// lists a registered catalog, not this directory.
inline constexpr const char* kUserDataDir = "/project/.user-data";
inline constexpr const char* kUserCacheDir = "/project/.cache";

/**
 * @brief Mount the origin's persistent storage at @p mountPoint.
 *
 * Idempotent: the backend is created once per page and later calls for the
 * same mount point succeed without touching it.
 *
 * Blocks until the OPFS worker is up. Safe to call from the browser's main
 * thread: the worker is spawned from a pthread, because WasmFS's wait for it
 * is unsatisfiable on the thread that has to start it.
 *
 * @return true when @p mountPoint is a usable persistent directory.
 */
bool MountPersistentStorage(const std::filesystem::path& mountPoint);

} // namespace Web
} // namespace Platform
} // namespace GameEngine
