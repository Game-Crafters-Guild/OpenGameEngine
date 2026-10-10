#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine { namespace Rendering {

// What the closure walk has already worked out about a shader file's content,
// so a header pulled in by fifty shaders is hashed and scanned once instead of
// fifty times.
//
// `IncludeClosure::covered` dedupes within ONE shader; nothing dedupes across
// them. The cost lands on cache HITS specifically, because the key has to be
// built before the compile cache can be probed -- so a hit pays full price for
// every include on disk, and a burst of first-time draws pays it once per
// shader.
//
// THE CACHE IS VALIDATED, NOT ANNOUNCED. Every lookup still reads the file and
// compares the bytes; only the hashing and the directive scan are reused, and
// only when the bytes match exactly. Nothing here trusts a timestamp, a size, or
// an edit notification: a rewrite that preserves the mtime and the length is the
// case the content-addressed key exists to catch, and it is also the case an
// announcement channel misses in any process that has no watcher -- a cook, a
// test, a player. The read is cheap next to an FNV pass and a directive scan
// over the same bytes, which is what makes paying it every time affordable.
//
// Where a directive resolves is never remembered: every walk searches the disk
// again. A file appearing or disappearing earlier in a search order changes
// which file a directive reaches, and no read of the file it used to reach can
// reveal that, so the key stays a function of what is on disk now.
class ShaderSourceCache
{
public:
    struct Entry
    {
        // The bytes this entry was derived from. Kept so a later lookup can
        // prove the file still holds them.
        std::string Bytes;
        uint64_t Hash = 0;
        // Operands of this file's own #include directives, so a cached file
        // costs no re-scan of its text.
        std::vector<std::string> IncludeOperands;
    };

    // Shared, never copied: one engine header is reached from dozens of
    // shaders, and each visit reads its operand list. A handle also keeps its
    // snapshot alive after a concurrent read has replaced the map slot, so a
    // walk in flight finishes on one consistent view.
    using EntryPtr = std::shared_ptr<const Entry>;

    static ShaderSourceCache& Get();

    // Read `path` and return its hash and include operands. Null exactly when a
    // direct read fails, so a caller can use this in place of reading the file;
    // a remembered entry for a file that can no longer be read is dropped.
    EntryPtr Read(const std::filesystem::path& path, const std::string& normalizedKey);

private:
    std::mutex m_Mutex;
    std::unordered_map<std::string, EntryPtr> m_Entries;
};

}} // namespace GameEngine::Rendering
