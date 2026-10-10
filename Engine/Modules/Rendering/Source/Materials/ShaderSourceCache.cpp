#include "ShaderSourceCache.h"

#include "ShaderIncludeClosure.h"

#include <utility>

namespace GameEngine { namespace Rendering {

ShaderSourceCache& ShaderSourceCache::Get()
{
    static ShaderSourceCache instance;
    return instance;
}

ShaderSourceCache::EntryPtr ShaderSourceCache::Read(const std::filesystem::path& path,
                                                    const std::string& normalizedKey)
{
    // Read first, always, and outside the lock. The bytes are what proves the
    // cached entry still describes this file, and on a cold cache a parallel
    // compile must not serialize every worker behind one file read.
    std::string bytes;
    if (!ReadShaderFileText(path, bytes, nullptr))
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Entries.erase(normalizedKey);
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        const auto it = m_Entries.find(normalizedKey);
        if (it != m_Entries.end() && it->second->Bytes == bytes)
            return it->second;
    }

    // Either unseen or changed. A comparison that fails is an edit -- including
    // one that preserved the file's mtime and length -- and the fresh hash is
    // what moves the compile key.
    auto entry = std::make_shared<Entry>();
    entry->Hash = HashShaderBytes(bytes.data(), bytes.size());
    entry->IncludeOperands = ScanShaderIncludeDirectives(bytes);
    entry->Bytes = std::move(bytes);

    std::lock_guard<std::mutex> lock(m_Mutex);
    // Assign rather than emplace: an existing entry here is the stale one this
    // read just disproved. Two workers racing the same file read the same
    // content and derive the same entry, so either may win.
    EntryPtr& slot = m_Entries[normalizedKey];
    slot = std::move(entry);
    return slot;
}

}} // namespace GameEngine::Rendering
