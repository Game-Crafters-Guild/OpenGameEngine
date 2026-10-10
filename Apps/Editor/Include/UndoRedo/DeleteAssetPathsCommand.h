#pragma once

#include "UndoRedo/IEditorCommand.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "FileSystem/FileSystem.h"
#include "Logger/Logger.h"
#include "Platform/Shell.h"

#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

namespace GameEngine::Editor
{

// Undoable delete for one or more asset paths (files or folders).
// Do/Redo copies each path into a staging directory for Undo, then moves the
// original into the OS trash / Recycle Bin so nothing is permanently lost even
// after this command leaves the undo stack. Undo restores from staging and
// re-registers assets. Staging is removed when the command is destroyed.
class DeleteAssetPathsCommand final : public IEditorCommand
{
public:
    DeleteAssetPathsCommand(std::vector<std::filesystem::path> paths,
                            AssetManager* assets,
                            std::filesystem::path stagingRoot,
                            std::function<void()> onChanged)
        : m_Paths(std::move(paths))
        , m_Assets(assets)
        , m_StagingRoot(std::move(stagingRoot))
        , m_OnChanged(std::move(onChanged))
    {
        if (m_Paths.size() == 1)
        {
            const auto& p = m_Paths.front();
            std::error_code ec;
            if (std::filesystem::is_directory(p, ec))
                m_Name = "Delete Folder";
            else
                m_Name = "Delete Asset";
        }
        else
        {
            m_Name = "Delete " + std::to_string(m_Paths.size()) + " Assets";
        }
    }

    ~DeleteAssetPathsCommand() override
    {
        if (m_StagingRoot.empty())
            return;
        std::error_code ec;
        std::filesystem::remove_all(m_StagingRoot, ec);
    }

    const char* GetName() const override { return m_Name.c_str(); }
    const char* GetTypeName() const override { return "DeleteAssetPathsCommand"; }

    void Do() override { ApplyDelete(); }
    void Undo() override { ApplyRestore(); }
    void Redo() override { ApplyDelete(); }

private:
    struct Entry
    {
        std::filesystem::path Original;
        std::filesystem::path Staged;
    };

    static bool CopyPath(const std::filesystem::path& from, const std::filesystem::path& to)
    {
        std::error_code ec;
        std::filesystem::create_directories(to.parent_path(), ec);
        // Streamed, not std::filesystem::copy: OPFS (wasm) rejects copy_file
        // with EPERM because it has no POSIX permission bits to preserve, and a
        // failed staging copy here silently costs the delete its undo.
        if (FileSystem::CopyTree(from, to))
            return true;
        Logger::Log::Warning("DeleteAssetPathsCommand: failed to stage copy of '{}'",
                             from.string());
        return false;
    }

    static bool MovePath(const std::filesystem::path& from, const std::filesystem::path& to)
    {
        std::error_code ec;
        std::filesystem::create_directories(to.parent_path(), ec);
        ec.clear();
        std::filesystem::rename(from, to, ec);
        if (!ec)
            return true;

        if (!CopyPath(from, to))
            return false;
        ec.clear();
        std::filesystem::remove_all(from, ec);
        if (ec)
        {
            Logger::Log::Warning("DeleteAssetPathsCommand: restored copy at '{}' but failed to clear staging '{}': {}",
                                 to.string(), from.string(), ec.message());
            return false;
        }
        return true;
    }

    static bool RemoveOriginal(const std::filesystem::path& original)
    {
        if (Platform::MoveToTrash(original))
            return true;

        // Trash unavailable — remove after the undo copy is already staged.
        std::error_code ec;
        std::filesystem::remove_all(original, ec);
        if (ec)
        {
            Logger::Log::Warning("DeleteAssetPathsCommand: trash failed and remove_all failed for '{}': {}",
                                 original.string(), ec.message());
            return false;
        }
        Logger::Log::Warning("DeleteAssetPathsCommand: OS trash unavailable; permanently removed '{}'",
                             original.string());
        return true;
    }

    void UnregisterTree(const std::filesystem::path& root) const
    {
        if (!m_Assets)
            return;
        auto& registry = m_Assets->GetRegistry();
        std::error_code ec;
        if (std::filesystem::is_regular_file(root, ec))
        {
            (void)registry.TryUnregisterAssetByPath(root);
            return;
        }
        if (!std::filesystem::is_directory(root, ec))
            return;
        for (std::filesystem::recursive_directory_iterator it(root, ec), end;
             !ec && it != end; it.increment(ec))
        {
            if (it->is_regular_file(ec))
                (void)registry.TryUnregisterAssetByPath(it->path());
        }
        (void)registry.TryUnregisterAssetByPath(root);
    }

    void RegisterTree(const std::filesystem::path& root) const
    {
        if (!m_Assets)
            return;
        auto& registry = m_Assets->GetRegistry();
        std::error_code ec;
        if (std::filesystem::is_regular_file(root, ec))
        {
            try { (void)registry.RegisterAsset(root); } catch (...) {}
            return;
        }
        if (!std::filesystem::is_directory(root, ec))
            return;
        for (std::filesystem::recursive_directory_iterator it(root, ec), end;
             !ec && it != end; it.increment(ec))
        {
            if (it->is_regular_file(ec))
            {
                try { (void)registry.RegisterAsset(it->path()); } catch (...) {}
            }
        }
    }

    void ApplyDelete()
    {
        if (m_Paths.empty())
            return;

        std::error_code ec;
        std::filesystem::create_directories(m_StagingRoot, ec);
        if (ec)
        {
            Logger::Log::Warning("DeleteAssetPathsCommand: failed to create staging '{}': {}",
                                 m_StagingRoot.string(), ec.message());
            return;
        }

        m_Entries.clear();
        m_Entries.reserve(m_Paths.size());
        for (size_t i = 0; i < m_Paths.size(); ++i)
        {
            const auto& original = m_Paths[i];
            if (original.empty())
                continue;
            ec.clear();
            if (!std::filesystem::exists(original, ec))
                continue;

            const std::filesystem::path staged =
                m_StagingRoot / (std::to_string(i) + "_" + original.filename().string());
            // Stage a copy first so Undo still works after the original goes to trash.
            if (!CopyPath(original, staged))
                continue;
            UnregisterTree(original);
            if (!RemoveOriginal(original))
            {
                // Leave the staged copy so a later Undo attempt can still recover it
                // if the original somehow remains; drop the staging entry otherwise.
                std::error_code removeEc;
                if (std::filesystem::exists(original, removeEc))
                    std::filesystem::remove_all(staged, removeEc);
                continue;
            }
            m_Entries.push_back(Entry{original, staged});
        }

        if (m_OnChanged)
            m_OnChanged();
    }

    void ApplyRestore()
    {
        for (auto it = m_Entries.rbegin(); it != m_Entries.rend(); ++it)
        {
            if (!MovePath(it->Staged, it->Original))
            {
                // The staging copy could not be moved back; the entry is
                // dropped below, so the only remaining copy is the one in the
                // OS trash. Say so loudly instead of failing silently.
                Logger::Log::Error(
                    "Undo could not restore '{}' from staging; recover it from the OS trash",
                    it->Original.string());
                continue;
            }
            RegisterTree(it->Original);
        }
        m_Entries.clear();
        if (m_OnChanged)
            m_OnChanged();
    }

    std::vector<std::filesystem::path> m_Paths;
    std::vector<Entry> m_Entries;
    AssetManager* m_Assets = nullptr;
    std::filesystem::path m_StagingRoot;
    std::function<void()> m_OnChanged;
    std::string m_Name;
};

} // namespace GameEngine::Editor
