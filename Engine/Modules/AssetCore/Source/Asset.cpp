#include "AssetCore/Asset.h"
#include "AssetCore/SharedFileRead.h"
#include <chrono>

namespace GameEngine {

Asset::Asset(const GUID& guid, AssetType type, const std::filesystem::path& path, String typeId)
    : m_Guid(guid)
    , m_Type(type)
    , m_TypeIdString(std::move(typeId))
    , m_Path(path)
    , m_State(AssetState::Unloaded)
{
    if (m_TypeIdString.empty())
    {
        m_TypeIdString = AssetTypeToString(type);
    }
    m_TypeId = HashAssetTypeId(m_TypeIdString);

    UpdateLastModifiedTime();
    m_LastChecked = std::chrono::file_clock::now();
}

String Asset::GetName() const {
    return m_Path.stem().string();
}

String Asset::GetExtension() const {
    return m_Path.extension().string();
}

std::filesystem::file_time_type Asset::GetLastModified() const {
    if (Exists()) {
        try {
            return std::filesystem::last_write_time(m_Path);
        } catch (...) {
            // Return cached value if filesystem operation fails
            return m_LastModified;
        }
    }
    return m_LastModified;
}

bool Asset::Exists() const {
    try {
        return std::filesystem::exists(m_Path);
    } catch (...) {
        return false;
    }
}

ReloadOutcome Asset::Reload() {
    // Sample the timestamp before the read, not after: a write that lands while
    // we are reading leaves a newer one on disk, and stamping that would retire
    // bytes we never saw. Stamping an older timestamp only costs another pass.
    const std::filesystem::file_time_type observed = GetLastModified();

    // Read first. Unloading before the read is what lets a deleted file, a
    // half-written save or a sharing violation strip a perfectly good loaded
    // asset; the bytes have to be in hand before the live payload is touched.
    Vector<uint8> data;
    if (!ReadFileBytesShared(m_Path, data)) {
        return ReloadOutcome::Failed;
    }

    // Bytes that have been read have been judged, whether or not they loaded.
    // Recording them either way is what stops NeedsReload() from offering the
    // same rejected file on every poll for the rest of the session — an asset
    // that keeps its payload through a failed reload stays Loaded, so nothing
    // else would ever take it out of the polling set.
    m_LastModified = observed;
    m_LastChecked = std::chrono::file_clock::now();

    // No bytes is not a payload. A save that truncates before it writes puts
    // the file through zero length first, and that is a change a watcher
    // reports, so an empty read is a save in progress far more often than a
    // file somebody emptied — the same reading AssetIOService takes on the
    // async path. The live payload beats nothing either way.
    //
    // No retry sleep here: this runs inline on the main thread. The writing
    // half of the save leaves a timestamp newer than the one just recorded and
    // raises its own change, so the finished file is offered again on its own;
    // a file that really is empty leaves that timestamp alone and is not
    // re-offered.
    if (data.empty()) {
        return ReloadOutcome::Deferred;
    }

    if (!ReloadFromData(data)) {
        return ReloadOutcome::Failed;
    }

    PostLoad();
    return ReloadOutcome::Reloaded;
}

bool Asset::ReloadFromData(const Vector<uint8>& data) {
    Unload();
    return LoadFromData(data);
}

bool Asset::AdoptReload(Asset& staged) {
    if (!AdoptReloadedPayload(staged)) {
        return false;
    }

    // Same bookkeeping tail as Reload(): the asset now carries the payload of
    // the current on-disk bytes.
    SetState(AssetState::Loaded);
    UpdateLastModifiedTime();
    m_LastChecked = std::chrono::file_clock::now();
    return true;
}

bool Asset::NeedsReload() const {
    if (!Exists() || m_State != AssetState::Loaded) {
        return false;
    }

    // Check if file has been modified since last check
    auto currentModified = GetLastModified();
    return currentModified > m_LastModified;
}

void Asset::UpdateLastModifiedTime() const {
    if (Exists()) {
        try {
            m_LastModified = std::filesystem::last_write_time(m_Path);
        } catch (...) {
            // Ignore filesystem errors
        }
    }
}

} // namespace GameEngine
