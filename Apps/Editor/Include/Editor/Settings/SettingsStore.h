#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace GameEngine::Editor
{
// Simple JSON-backed settings store with atomic writes.
// Intended for Editor preferences and project settings (Editor-side).
//
// Concurrency contract: several editor processes share the global preferences
// file. Save() re-reads the file under an inter-process lock and writes back
// only the top-level keys this store changed since Load() (set, or removed),
// so a store that loaded empty/stale — failed Load, file momentarily absent,
// long-lived snapshot — cannot wipe keys written by another process. Merging
// is per top-level key: concurrent edits to different keys both survive;
// concurrent edits to the same key are last-writer-wins.
class SettingsStore
{
  public:
    explicit SettingsStore(std::filesystem::path filePath);

    const std::filesystem::path& GetFilePath() const { return m_FilePath; }

    // Loads the file if present; otherwise initializes an empty store.
    // Ensures schemaVersion is present. On failure the store resets to empty
    // and a later Save() will still preserve whatever is on disk.
    bool Load(std::string* outError = nullptr);

    // Merges this store's changes into the current on-disk state and writes
    // atomically (temp file + rename) under an inter-process lock. Identical
    // serialized bytes are not rewritten. Fails — without touching the file —
    // when an existing file cannot be read back (a corrupt/unparseable file is
    // overwritten as recovery). On success the store reflects the merged
    // on-disk state, including keys other processes wrote since Load().
    bool Save(std::string* outError = nullptr);

    // Direct JSON access (advanced use).
    nlohmann::json& Json() { return m_Json; }
    const nlohmann::json& Json() const { return m_Json; }

    // Common helpers.
    bool Contains(std::string_view key) const;
    bool Remove(std::string_view key);

    bool TryGetBool(std::string_view key, bool& out) const;
    bool TryGetInt64(std::string_view key, int64_t& out) const;
    bool TryGetDouble(std::string_view key, double& out) const;
    bool TryGetString(std::string_view key, std::string& out) const;

    void SetBool(std::string_view key, bool v);
    void SetInt64(std::string_view key, int64_t v);
    void SetDouble(std::string_view key, double v);
    void SetString(std::string_view key, std::string_view v);
    void SetJson(std::string_view key, const nlohmann::json& v);

    // Schema version helpers.
    int GetSchemaVersion() const;
    void SetSchemaVersion(int v);

  private:
    std::filesystem::path m_FilePath;
    nlohmann::json m_Json;
    // On-disk state as of the last successful Load(); Save() diffs m_Json
    // against this to decide which keys the merge may set or remove.
    nlohmann::json m_DiskSnapshot;
};

// High-level conventions (paths only; callers decide whether/when to load/save).
SettingsStore OpenEditorPreferences();
SettingsStore OpenProjectSettings(const std::filesystem::path& workspaceRoot);
SettingsStore OpenUserProjectSettings(const std::filesystem::path& workspaceRoot);

} // namespace GameEngine::Editor
