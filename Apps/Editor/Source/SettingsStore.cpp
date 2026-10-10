#include "Editor/Settings/SettingsStore.h"

#if defined(__APPLE__)
#include <cstdio> // renamex_np, RENAME_SWAP
#endif

#include "Editor/EditorPaths.h"
#include "FileSystem/FileSystem.h"
#include "FileSystem/ScopedFileLock.h"

#include <fstream>

namespace GameEngine::Editor
{
namespace fs = std::filesystem;

static fs::path LockFilePath(const fs::path& settingsFile)
{
    fs::path p = settingsFile;
    p += ".lock";
    return p;
}

enum class JsonFileReadResult
{
    Ok,
    Missing,
    IoError,
    ParseError,
};

static JsonFileReadResult ReadJsonObjectFile(const fs::path& file, nlohmann::json& outJson, std::string* outError)
{
    outJson = nlohmann::json::object();

    std::error_code ec;
    if (!fs::exists(file, ec))
        return JsonFileReadResult::Missing;

    std::ifstream in(file, std::ios::binary | std::ios::in);
    if (!in.is_open())
    {
        if (outError)
            *outError = "Failed to open settings file: " + file.string();
        return JsonFileReadResult::IoError;
    }

    std::string contents;
    in.seekg(0, std::ios::end);
    const std::streampos len = in.tellg();
    in.seekg(0, std::ios::beg);
    if (len > 0)
    {
        contents.resize(static_cast<size_t>(len));
        in.read(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!in.good())
        {
            if (outError)
                *outError = "Failed while reading settings file: " + file.string();
            return JsonFileReadResult::IoError;
        }
    }

    if (contents.empty())
        return JsonFileReadResult::Ok;

    try
    {
        outJson = nlohmann::json::parse(contents, nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
        if (!outJson.is_object())
        {
            outJson = nlohmann::json::object();
            if (outError)
                *outError = "Settings file root is not a JSON object: " + file.string();
            return JsonFileReadResult::ParseError;
        }
    }
    catch (const std::exception& e)
    {
        outJson = nlohmann::json::object();
        if (outError)
            *outError = std::string("Failed to parse JSON: ") + e.what();
        return JsonFileReadResult::ParseError;
    }
    return JsonFileReadResult::Ok;
}

static bool EnsureParentDir(const fs::path& filePath, std::string* outError)
{
    std::error_code ec;
    fs::path parent = filePath.parent_path();
    if (parent.empty())
        return true;

    fs::create_directories(parent, ec);
    if (ec)
    {
        if (outError)
            *outError = "Failed to create directory: " + parent.string() + " (" + ec.message() + ")";
        return false;
    }
    return true;
}

// True when the file already holds exactly these bytes.
static bool FileAlreadyHasContents(const fs::path& filePath, const std::string& contents)
{
    std::error_code ec;
    if (!fs::exists(filePath, ec) || ec)
        return false;

    ec.clear();
    const auto size = fs::file_size(filePath, ec);
    if (ec || size != contents.size())
        return false;

    std::ifstream in(filePath, std::ios::binary | std::ios::in);
    if (!in.is_open())
        return false;

    std::string existing;
    existing.resize(contents.size());
    if (!existing.empty())
        in.read(existing.data(), static_cast<std::streamsize>(existing.size()));
    if (in.bad())
        return false;

    return existing == contents;
}

static bool WriteTextFileAtomic(const fs::path& dstFile, const std::string& contents, std::string* outError)
{
    if (!EnsureParentDir(dstFile, outError))
        return false;

    std::error_code ec;
    fs::path parent = dstFile.parent_path();
    const std::string filename = dstFile.filename().string();

    // Keep temp file in the same directory to maximize rename atomicity.
    fs::path tmp = (parent / (filename + ".tmp")).lexically_normal();

    // If a stale temp exists, best-effort remove.
    fs::remove(tmp, ec);
    ec.clear();

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!out.is_open())
        {
            if (outError)
                *outError = "Failed to open temp file for write: " + tmp.string();
            return false;
        }
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        out.flush();
        if (!out.good())
        {
            if (outError)
                *outError = "Failed while writing temp file: " + tmp.string();
            return false;
        }
    }

    // Publish the complete replacement. Native publication preserves the
    // previous destination if replacement fails; it never truncates it first.
#if defined(__APPLE__)
    // Atomic exchange: the destination is never absent, not even for the
    // instant a byte-replacing fallback needs (observed as a visible "blink"
    // of Preferences.json under a file watcher).
    if (fs::exists(dstFile, ec) &&
        ::renamex_np(tmp.c_str(), dstFile.c_str(), RENAME_SWAP) == 0)
    {
        std::error_code cleanupEc;
        fs::remove(tmp, cleanupEc); // now holds the old contents
        return true;
    }
    ec.clear();
#endif
    if (!FileSystem::PublishFile(tmp, dstFile))
    {
        if (outError)
            *outError = "Failed to replace settings file: " + dstFile.string();
        return false;
    }
    return true;
}

SettingsStore::SettingsStore(fs::path filePath)
    : m_FilePath(std::move(filePath))
{
    m_Json = nlohmann::json::object();
    SetSchemaVersion(1);
    m_DiskSnapshot = m_Json;
}

int SettingsStore::GetSchemaVersion() const
{
    try
    {
        if (m_Json.contains("schemaVersion") && m_Json["schemaVersion"].is_number_integer())
            return m_Json["schemaVersion"].get<int>();
    }
    catch (...)
    {
    }
    return 1;
}

void SettingsStore::SetSchemaVersion(int v)
{
    if (!m_Json.is_object())
        m_Json = nlohmann::json::object();
    m_Json["schemaVersion"] = v;
}

bool SettingsStore::Load(std::string* outError)
{
    if (m_FilePath.empty())
    {
        if (outError)
            *outError = "SettingsStore file path is empty";
        return false;
    }

    FileSystem::ScopedFileLock lock(LockFilePath(m_FilePath));

    nlohmann::json disk;
    const JsonFileReadResult result = ReadJsonObjectFile(m_FilePath, disk, outError);

    m_Json = disk;
    if (!m_Json.contains("schemaVersion"))
        SetSchemaVersion(1);
    // Snapshot what actually came from disk (empty on missing/failed read), so
    // Save() only ever merges back keys this store changed on top of it.
    m_DiskSnapshot = m_Json;

    return result == JsonFileReadResult::Ok || result == JsonFileReadResult::Missing;
}

bool SettingsStore::Save(std::string* outError)
{
    if (m_FilePath.empty())
    {
        if (outError)
            *outError = "SettingsStore file path is empty";
        return false;
    }

    nlohmann::json ours = m_Json;
    if (!ours.is_object())
        ours = nlohmann::json::object();
    if (!ours.contains("schemaVersion"))
        ours["schemaVersion"] = 1;

    if (!EnsureParentDir(m_FilePath, outError))
        return false;

    // Hold the lock across read-merge-write so concurrent editors serialize.
    FileSystem::ScopedFileLock lock(LockFilePath(m_FilePath));

    nlohmann::json disk;
    std::string readError;
    const JsonFileReadResult readResult = ReadJsonObjectFile(m_FilePath, disk, &readError);
    if (readResult == JsonFileReadResult::IoError)
    {
        // The file exists but cannot be read back; overwriting would destroy
        // every key we did not load. Refuse and leave it untouched.
        if (outError)
            *outError = "Refusing to save settings over unreadable file (" + readError + ")";
        return false;
    }

    nlohmann::json merged;
    if (readResult == JsonFileReadResult::Ok)
    {
        // Merge: start from current disk state, apply only the top-level keys
        // this store changed since Load. Keys written by other processes in
        // the meantime survive.
        merged = disk;
        const bool snapshotValid = m_DiskSnapshot.is_object() && m_Json.is_object();
        const nlohmann::json base = snapshotValid ? m_DiskSnapshot : ours;
        for (const auto& [key, value] : ours.items())
        {
            if (!base.contains(key) || base.at(key) != value)
                merged[key] = value;
        }
        for (const auto& item : base.items())
        {
            if (!ours.contains(item.key()))
                merged.erase(item.key());
        }
        if (!merged.contains("schemaVersion"))
            merged["schemaVersion"] = 1;
    }
    else
    {
        // Missing (first save / file deleted) or corrupt (recovery): write our
        // full state. A corrupt original is set aside first — recovery must
        // not be the operation that destroys the evidence (and the keys) of
        // whatever corrupted it.
        if (readResult == JsonFileReadResult::ParseError)
        {
            std::error_code ec;
            fs::path aside = m_FilePath;
            aside += ".corrupt";
            fs::rename(m_FilePath, aside, ec);
        }
        merged = ours;
    }

    std::string s;
    try
    {
        s = merged.dump(/*indent=*/2);
        s.push_back('\n');
    }
    catch (const std::exception& e)
    {
        if (outError)
            *outError = std::string("Failed to serialize JSON: ") + e.what();
        return false;
    }

    // A save that would not change the file must not touch it. Settings pages
    // seed each row by handing the stored value straight back to the row's
    // change callback, so building a page runs a save per row; rewriting the
    // file there bumps its mtime, wakes the file watcher and dirties version
    // control for an edit nobody made.
    if (!FileAlreadyHasContents(m_FilePath, s) && !WriteTextFileAtomic(m_FilePath, s, outError))
        return false;

    // Sync the store to what is now on disk; keys merged in from other
    // processes must not read as "removed by us" on a subsequent Save.
    m_Json = merged;
    m_DiskSnapshot = merged;
    return true;
}

bool SettingsStore::Contains(std::string_view key) const
{
    if (!m_Json.is_object())
        return false;
    return m_Json.contains(std::string(key));
}

bool SettingsStore::Remove(std::string_view key)
{
    if (!m_Json.is_object())
        return false;
    return m_Json.erase(std::string(key)) > 0;
}

bool SettingsStore::TryGetBool(std::string_view key, bool& out) const
{
    try
    {
        const auto it = m_Json.find(std::string(key));
        if (it == m_Json.end() || !it->is_boolean())
            return false;
        out = it->get<bool>();
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool SettingsStore::TryGetInt64(std::string_view key, int64_t& out) const
{
    try
    {
        const auto it = m_Json.find(std::string(key));
        if (it == m_Json.end() || !(it->is_number_integer() || it->is_number_unsigned()))
            return false;
        out = it->get<int64_t>();
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool SettingsStore::TryGetDouble(std::string_view key, double& out) const
{
    try
    {
        const auto it = m_Json.find(std::string(key));
        if (it == m_Json.end() || !(it->is_number_float() || it->is_number_integer() || it->is_number_unsigned()))
            return false;
        out = it->get<double>();
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool SettingsStore::TryGetString(std::string_view key, std::string& out) const
{
    try
    {
        const auto it = m_Json.find(std::string(key));
        if (it == m_Json.end() || !it->is_string())
            return false;
        out = it->get<std::string>();
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void SettingsStore::SetBool(std::string_view key, bool v)
{
    if (!m_Json.is_object())
        m_Json = nlohmann::json::object();
    m_Json[std::string(key)] = v;
}

void SettingsStore::SetInt64(std::string_view key, int64_t v)
{
    if (!m_Json.is_object())
        m_Json = nlohmann::json::object();
    m_Json[std::string(key)] = v;
}

void SettingsStore::SetDouble(std::string_view key, double v)
{
    if (!m_Json.is_object())
        m_Json = nlohmann::json::object();
    m_Json[std::string(key)] = v;
}

void SettingsStore::SetString(std::string_view key, std::string_view v)
{
    if (!m_Json.is_object())
        m_Json = nlohmann::json::object();
    m_Json[std::string(key)] = std::string(v);
}

void SettingsStore::SetJson(std::string_view key, const nlohmann::json& v)
{
    if (!m_Json.is_object())
        m_Json = nlohmann::json::object();
    m_Json[std::string(key)] = v;
}

SettingsStore OpenEditorPreferences()
{
    const EditorGlobalPaths g = GetEditorGlobalPaths();
    return SettingsStore(g.preferencesFile);
}

SettingsStore OpenProjectSettings(const fs::path& workspaceRoot)
{
    const EditorGlobalPaths g = GetEditorGlobalPaths();
    const EditorProjectPaths p = GetEditorProjectPaths(workspaceRoot, g);
    return SettingsStore(p.projectSettingsFile);
}

SettingsStore OpenUserProjectSettings(const fs::path& workspaceRoot)
{
    const EditorGlobalPaths g = GetEditorGlobalPaths();
    const EditorProjectPaths p = GetEditorProjectPaths(workspaceRoot, g);
    return SettingsStore(p.userProjectSettingsFile);
}

} // namespace GameEngine::Editor
