#pragma once

#include "AssetCore/Types.h"

#include <string>

namespace GameEngine {

// Subasset identities are derived, never stored: GUID::Derive(containerGuid, key),
// with a per-scheme key ("embedded:<i>" for embedded animation clips,
// "material/<i>" for bridge materials). A container rename changes the container
// GUID and with it every derived subasset GUID, while persisted references
// (scenes) keep the old ones. Rename healing therefore cascades: the container's
// record journals its derive keys under kSubassetDeriveKeysKvKey, and redirect
// emission re-derives Derive(old, key) -> Derive(new, key) per key without any
// knowledge of the scheme that minted it.
//
// The key list lives in the git-shared journal, never only in the per-machine
// cache: the cascade must survive cache deletion (IAssetDbCache is contractually
// safe to delete) and must work on a machine that never imported the container
// before the rename.
inline constexpr const char* kSubassetDeriveKeysKvKey = "subassets.deriveKeys";

// Key formatters — single source of truth for each scheme's key layout. The
// outputs are a persistence contract (see GUID::Derive): scenes bake the derived
// GUIDs and the journal bakes the keys themselves. Treat both as frozen.
inline String EmbeddedClipDeriveKey(uint32 animationIndex)
{
    return "embedded:" + std::to_string(animationIndex);
}

inline String ModelMaterialDeriveKey(uint32 materialIndex)
{
    return "material/" + std::to_string(materialIndex);
}

// kv-row codec: newline-joined, order-preserving. Keys must be non-empty and
// must not contain '\n' (both schemes above are index-formatted); the journal
// writer (AssetRegistry::RegisterSubassetDeriveKeys) enforces this.
inline String JoinSubassetDeriveKeys(const Vector<String>& keys)
{
    String out;
    for (const String& key : keys)
    {
        if (!out.empty())
            out += '\n';
        out += key;
    }
    return out;
}

inline Vector<String> SplitSubassetDeriveKeys(const String& value)
{
    Vector<String> keys;
    size_t begin = 0;
    while (begin <= value.size())
    {
        const size_t end = value.find('\n', begin);
        const size_t stop = (end == String::npos) ? value.size() : end;
        if (stop > begin)
            keys.emplace_back(value.substr(begin, stop - begin));
        if (end == String::npos)
            break;
        begin = end + 1;
    }
    return keys;
}

} // namespace GameEngine
