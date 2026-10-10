#include "Platform/WebPersistentStorage.h"

#include "Logger/Logger.h"

#include <emscripten/wasmfs.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>

namespace GameEngine {
namespace Platform {
namespace Web {

namespace {

// One OPFS backend per page: the backend owns a dedicated worker, and asking
// WasmFS for a second one would spawn a second worker over the same storage.
//
// Created on a pthread, never on the browser's main thread. WasmFS blocks
// inside the constructor until that worker has started, and a main thread
// waiting for a worker it is itself responsible for starting is waiting on
// itself; upstream asserts on exactly this (system/lib/wasmfs/thread_utils.h),
// so on the main thread every assertion-enabled build aborts here before the
// application sees a frame. Spawning from a pthread satisfies the precondition
// instead of relying on ASYNCIFY to unwind around it.
backend_t OpfsBackend()
{
    static backend_t backend = []
    {
        backend_t created = nullptr;
        std::thread spawner([&created] { created = wasmfs_create_opfs_backend(); });
        spawner.join();
        return created;
    }();
    return backend;
}

} // namespace

bool MountPersistentStorage(const std::filesystem::path& mountPoint)
{
    static std::unordered_map<std::string, bool> s_mounted;

    const std::string path = mountPoint.string();
    if (const auto it = s_mounted.find(path); it != s_mounted.end())
    {
        return it->second;
    }

    bool ok = false;
    if (backend_t opfs = OpfsBackend(); opfs == nullptr)
    {
        LOG_ERROR("Platform: the browser refused an OPFS backend, so '{}' cannot be persistent. "
                  "OPFS needs a secure context (https or localhost) and a browser that "
                  "implements it; Chromium does, Safari's implementation is incomplete.",
                  path);
    }
    else if (const int err = wasmfs_create_directory(path.c_str(), 0777, opfs); err != 0)
    {
        LOG_ERROR("Platform: mounting persistent storage at '{}' failed ({}). The parent "
                  "directory must exist and the mount point must not already be a file.",
                  path, std::strerror(errno));
    }
    else
    {
        ok = true;
        LOG_INFO("Platform: persistent storage (OPFS) mounted at '{}'.", path);
    }

    s_mounted.emplace(path, ok);
    return ok;
}

} // namespace Web
} // namespace Platform
} // namespace GameEngine
