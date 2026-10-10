#pragma once

#include "AssetCore/Types.h"
#include "AssetCore/GUID.h"
#include "AssetCore/AssetTypes.h"
#include "AssetCore/AssetTypeId.h"
#include <filesystem>
#include <utility>

namespace GameEngine {

class AssetManager;

/**
 * @brief What a Reload() attempt did to the asset.
 *
 * Deferred is not a failure: the file was readable but had no bytes yet,
 * which is the state a truncate-then-write save passes through. The live
 * payload is kept and nothing went wrong, so callers that report failures
 * must not report this one.
 */
enum class ReloadOutcome {
    Reloaded, ///< The asset now holds the file's bytes.
    Deferred, ///< The read came back empty; a save is in progress.
    Failed    ///< The file could not be read, or its bytes were rejected.
};

/**
 * @brief Base class for all assets
 *
 * This is the core asset interface that all asset types must implement.
 * It provides basic functionality for asset identification, loading, and state management.
 */
class Asset {
public:
    /**
     * @brief Constructor
     * @param guid Unique identifier for the asset
     * @param type Type of the asset
     * @param path File path to the asset
     * @param typeId Programmable type id string (optional; defaults to AssetTypeToString(type))
     */
    Asset(const GUID& guid, AssetType type, const std::filesystem::path& path, String typeId = {});

    /**
     * @brief Virtual destructor
     */
    virtual ~Asset() = default;

    /**
     * @brief Get asset GUID
     * @return The unique identifier for this asset
     */
    const GUID& GetGUID() const { return m_Guid; }

    /**
     * @brief Get asset type
     * @return The type of this asset
     */
    AssetType GetType() const { return m_Type; }

    /**
     * @brief Get programmable type id (hashed)
     */
    AssetTypeId GetTypeId() const { return m_TypeId; }

    /**
     * @brief Get programmable type id string (authoritative for persistence/debugging)
     */
    const String& GetTypeIdString() const { return m_TypeIdString; }

    /**
     * @brief Get asset file path
     * @return The file path to this asset
     */
    const std::filesystem::path& GetPath() const { return m_Path; }

    /**
     * @brief Get asset loading state
     * @return The current loading state of this asset
     */
    AssetState GetState() const { return m_State; }

    /**
     * @brief Get asset name (filename without extension)
     * @return The name of the asset derived from its file path
     */
    String GetName() const;

    /**
     * @brief Get file extension
     * @return The file extension of the asset
     */
    String GetExtension() const;

    /**
     * @brief Get last modified time
     * @return The last modification time of the asset file
     */
    std::filesystem::file_time_type GetLastModified() const;

    /**
     * @brief Check if asset file exists
     * @return true if the asset file exists on disk
     */
    bool Exists() const;

    /**
     * @brief Load asset from file
     * @return true if loading was successful, false otherwise
     */
    virtual bool Load() = 0;

    /**
     * @brief Load asset from memory data (for async loading)
     * @param data The raw data to load from
     * @return true if loading was successful, false otherwise
     */
    virtual bool LoadFromData(const Vector<uint8>& data) = 0;

    /**
     * @brief Unload asset from memory
     */
    virtual void Unload() = 0;

    /**
     * @brief Reload the asset from its file.
     *
     * The file is read before the loaded payload is touched, so a missing,
     * locked, unreadable or still-empty file leaves the asset exactly as it
     * was. An empty read is never adopted: a save that truncates before it
     * writes passes through zero length, so on a reload zero bytes mean a save
     * in progress, whether or not this type would accept an empty file on an
     * initial load. Anything else goes through ReloadFromData().
     *
     * Bytes that were read count as seen whether or not they loaded, so
     * NeedsReload() stops offering a file the asset has already rejected until
     * that file changes again. The writing half of a truncate-then-write is
     * such a change, so the finished file still reloads.
     *
     * @return Reloaded once the asset holds the file's bytes, Deferred when
     *         the read came back empty, Failed when the file could not be
     *         read or its bytes were rejected.
     */
    ReloadOutcome Reload();

    /**
     * @brief True when this asset type can hot-reload through the async
     * pipeline: a fresh instance is decoded on worker threads and its payload
     * is adopted in place on the main thread via AdoptReload(). Types
     * returning true must implement AdoptReloadedPayload().
     */
    virtual bool SupportsAsyncReload() const { return false; }

    /**
     * @brief Adopt the payload of a freshly decoded instance of this asset
     * (main thread, cheap member swaps only). Equivalent to Reload() with the
     * read+decode already done: on success this object holds the new payload
     * (its pointer identity is unchanged — consumers holding Asset* stay
     * valid) and `staged` holds the previous payload, released when the
     * caller drops it. Returns false when `staged` is not adoptable (wrong
     * concrete type); the caller falls back to an inline Reload().
     */
    bool AdoptReload(Asset& staged);

    /**
     * @brief Post-load hook invoked after Load() / LoadFromData() succeeds.
     *
     * Runs on every load path (initial load, reload, sync, async) so derived
     * classes can put data-dependent build steps here without worrying about
     * which path produced the data. Default is a no-op. Overrides MUST NOT
     * throw — exceptions during hot-reload would unwind with the asset in a
     * Loaded-but-PostLoad-incomplete state.
     *
     * AssetManager::CheckForReloads invokes Reload() (and therefore PostLoad)
     * from the main thread after taking a shared_ptr snapshot of the loaded
     * asset. Overrides should still prefer local state only; re-entering
     * AssetManager APIs during reload can schedule additional work and event
     * dispatch while the current reload is still completing.
     *
     * Loaders that bypass AssetManager (direct calls to Load/LoadFromData on
     * a freshly-constructed Asset) are responsible for calling PostLoad
     * themselves on success; AssetManager and Asset::Reload() do this for
     * their callers.
     */
    virtual void PostLoad() {}

    /**
     * @brief Check if asset needs reloading (file changed)
     * @return true if the asset file has been modified since last load
     */
    bool NeedsReload() const;

    /**
     * @brief Get memory usage of this asset in bytes
     * @return Estimated memory usage in bytes
     */
    virtual size_t GetMemoryUsage() const { return 0; }

    /**
     * @brief Check if asset is loaded
     * @return true if the asset is in the Loaded state
     */
    bool IsLoaded() const { return m_State == AssetState::Loaded; }

    /**
     * @brief Check if asset is loading
     * @return true if the asset is in the Loading state
     */
    bool IsLoading() const { return m_State == AssetState::Loading; }

    /**
     * @brief Check if asset failed to load
     * @return true if the asset is in the Failed state
     */
    bool HasFailed() const { return m_State == AssetState::Failed; }

protected:
    /**
     * @brief Replace the loaded payload with the current file bytes, for
     * Reload(). The default unloads and loads from `data`.
     *
     * Override this in types whose consumers hold handles into the payload:
     * such a type parses `data` into a candidate first and commits into the
     * live objects only once the whole candidate parsed, so that returning
     * false leaves every consumer on the last good payload. The default
     * implementation cannot offer that — Unload() has already run by the time
     * LoadFromData() can fail.
     *
     * @return true when the asset now holds the payload of `data`.
     */
    virtual bool ReloadFromData(const Vector<uint8>& data);

    /**
     * @brief Type-specific payload adoption for AdoptReload: swap the decoded
     * data members with `staged` (same concrete type, freshly loaded).
     * Non-payload user state (e.g. sampler settings) stays untouched, exactly
     * as it survives a Reload(). Runs on the main thread and must stay cheap —
     * member swaps, no decoding, no I/O.
     */
    virtual bool AdoptReloadedPayload(Asset& staged) { (void)staged; return false; }

    /**
     * @brief Set the loading state of the asset
     * @param state The new state to set
     */
    void SetState(AssetState state) { m_State = state; }

    /**
     * @brief Update the last modified time cache
     */
    void UpdateLastModifiedTime() const;

private:
    /**
     * @brief The asset's file moved. Owner thread only, and only through
     * AssetManager::RenameAssetPath, which keeps the registry binding and this path in step.
     */
    friend class AssetManager;
    void SetPath(std::filesystem::path path) { m_Path = std::move(path); }

    GUID m_Guid;                                                    ///< Unique identifier
    AssetType m_Type;                                              ///< Asset type
    AssetTypeId m_TypeId = 0;                                      ///< Programmable type id hash
    String m_TypeIdString;                                         ///< Programmable type id string
    std::filesystem::path m_Path;                                  ///< File path
    AssetState m_State;                                            ///< Loading state
    mutable std::filesystem::file_time_type m_LastChecked;        ///< Last time we checked for modifications
    mutable std::filesystem::file_time_type m_LastModified;       ///< Last modification time of the file

    DISALLOW_COPY_AND_ASSIGN(Asset);
};

/**
 * @brief Asset metadata structure
 *
 * Contains all the information needed to identify and create an asset.
 */
struct AssetMetadata {
    GUID Guid;                          ///< Unique identifier
    AssetType Type;                     ///< Asset type
    String TypeId;                      ///< Programmable type id string (may be custom)
    std::filesystem::path Path;         ///< File path
    String Name;                        ///< Asset name
    String Extension;                   ///< File extension
    /// @brief Last modification time. ADVISORY: at warm-start, populated
    /// from the snapshot's recorded mtime — may be stale relative to
    /// disk if the file was edited offline without bumping its parent
    /// dir's mtime. Consumers needing the authoritative value must stat
    /// the file themselves.
    std::filesystem::file_time_type LastModified;
    /// @brief File size in bytes. ADVISORY: same staleness contract as
    /// LastModified. Anything that depends on the size matching disk
    /// (integrity checks, streaming chunk sizing) must re-stat.
    size_t FileSize;

    // Dependency information
    Vector<GUID> Dependencies;          ///< Assets this asset depends on
    Vector<String> DependencyPaths;     ///< Dependency paths (for debugging/logging)
    bool DependenciesExtracted = false; ///< Whether dependencies have been extracted

    /**
     * @brief Default constructor
     */
    AssetMetadata() : Type(AssetType::Unknown), FileSize(0) {}

    /**
     * @brief Constructor with basic information
     */
    AssetMetadata(const GUID& guid, AssetType type, const std::filesystem::path& path)
        : Guid(guid), Type(type), TypeId(AssetTypeToString(type)), Path(path), FileSize(0) {
        Name = path.stem().string();
        Extension = path.extension().string();

        try {
            if (std::filesystem::exists(path)) {
                LastModified = std::filesystem::last_write_time(path);
                FileSize = std::filesystem::file_size(path);
            }
        } catch (...) {
            // Ignore filesystem errors
        }
    }

    /**
     * @brief Check if metadata is valid
     */
    bool IsValid() const {
        return !Guid.IsNull() && Type != AssetType::Unknown && !Path.empty();
    }
};

} // namespace GameEngine
