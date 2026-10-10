#include "FileSystem/TreeRemoval.h"

#include "JobSystem/JobChannel.h"
#include "Logger/Logger.h"
#include "Platform/WebPersistentStorage.h"

#include <emscripten/em_js.h>

#include <string>

// Remove one OPFS entry, given its path relative to the OPFS root, with the
// browser's own recursive removeEntry. Fire-and-forget: the promise resolves
// entirely inside the browser, with no per-file WASMFS proxy round-trip.
// clang-format off
EM_JS(void, GeWebRemoveOpfsEntry, (const char* relUtf8), {
    const rel = UTF8ToString(relUtf8);
    (async () => {
        try {
            let dir = await navigator.storage.getDirectory();
            const parts = rel.split('/').filter(function (s) { return s.length; });
            for (let i = 0; i < parts.length - 1; ++i)
                dir = await dir.getDirectoryHandle(parts[i]);
            await dir.removeEntry(parts[parts.length - 1], { recursive: true });
            console.log('[FileSystem] removed', rel);
        } catch (e) {
            console.warn('[FileSystem] removeEntry failed for', rel, e);
        }
    })();
});
// clang-format on

namespace GameEngine::FileSystem
{

TreeRemoval::TreeRemoval(::JobSystem::WorkStealingThreadPool& /*jobSystem*/) {}

TreeRemoval::~TreeRemoval() = default;

// The persistent mount root is the OPFS root, so a path's OPFS-relative form
// is the path minus the mount. A synchronous std::filesystem::remove_all here
// would freeze the frame loop behind WASMFS's proxy.
void TreeRemoval::Remove(const std::filesystem::path& root)
{
    const std::filesystem::path mount =
        std::filesystem::path(Platform::Web::kProjectsMount).lexically_normal();
    const std::filesystem::path rel = root.lexically_normal().lexically_relative(mount);
    if (rel.empty() || rel.begin()->string() == "..")
    {
        LOG_WARNING("FileSystem: '{}' is outside persistent storage '{}' and was not removed",
                    root.string(), mount.string());
        return;
    }
    const std::string relStr = rel.generic_string();
    GeWebRemoveOpfsEntry(relStr.c_str());
}

} // namespace GameEngine::FileSystem
