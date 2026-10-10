#include "Inspectors/Particles/ParticleStackEditor.h"

#include "Assets/AssetManager.h"
#include "AssetCore/SharedFileRead.h"
#include "Particles/Assets/ParticleStackAsset.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <utility>

namespace GameEngine::ParticleInspectors
{
namespace
{
// The stack asset an undo entry edited, resolved when the entry runs: the object the asset manager
// holds for its GUID now, or, without a manager, the edited object while it still lives. The object
// the edit was made on may have been released or replaced since.
struct StackFile
{
    AssetManager* Assets = nullptr;
    GUID Guid;
    std::filesystem::path Path;
    Particles::ParticleStackAsset* Edited = nullptr;
    std::weak_ptr<const void> EditedLives;
};

// The loaded stack the entry reaches, or null when there is none; `owner` keeps a managed one alive.
Particles::ParticleStackAsset* LoadedStack(const StackFile& file, std::shared_ptr<Particles::ParticleStackAsset>& owner)
{
    if (file.Assets)
        owner = std::dynamic_pointer_cast<Particles::ParticleStackAsset>(file.Assets->GetAsset(file.Guid));
    if (owner)
        return owner.get();
    return file.EditedLives.expired() ? nullptr : file.Edited;
}

std::filesystem::path CurrentPath(const StackFile& file)
{
    std::shared_ptr<Particles::ParticleStackAsset> owner;
    const auto* loaded = LoadedStack(file, owner);
    return loaded ? loaded->GetPath() : file.Path;
}

bool CaptureStackFile(const StackFile& file, Editor::UndoRedoService::SnapshotTarget::Snapshot& out)
{
    Vector<uint8> bytes;
    if (!ReadFileBytesShared(CurrentPath(file), bytes))
        return false;
    out.assign(bytes.begin(), bytes.end());
    return true;
}

// Writes the snapshot back and reloads the stack if it is loaded; an unloaded stack reads the file
// when it next loads.
bool ApplyStackFile(const StackFile& file, const Editor::UndoRedoService::SnapshotTarget::Snapshot& snapshot)
{
    std::shared_ptr<Particles::ParticleStackAsset> owner;
    auto* loaded = LoadedStack(file, owner);
    std::ofstream out(loaded ? loaded->GetPath() : file.Path, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
        return false;
    out.write(reinterpret_cast<const char*>(snapshot.data()), static_cast<std::streamsize>(snapshot.size()));
    out.close();
    return !loaded || loaded->Load();
}

StackFile EditedStackFile(Particles::ParticleStackAsset& asset, AssetManager* assets)
{
    return {assets, asset.GetGUID(), asset.GetPath(), &asset, asset.Liveness()};
}

// The whole file, captured before an edit and written back on undo; the stack reloads from it.
Editor::UndoRedoService::SnapshotTarget MakeFileSnapshotTarget(const StackFile& file, const std::string& label)
{
    Editor::UndoRedoService::SnapshotTarget target;
    target.debugLabel = label;
    target.Capture = [file](Editor::UndoRedoService::SnapshotTarget::Snapshot& out) { return CaptureStackFile(file, out); };
    target.Apply = [file](const Editor::UndoRedoService::SnapshotTarget::Snapshot& snapshot)
    { return ApplyStackFile(file, snapshot); };
    return target;
}
} // namespace

ParticleStackEditor::ParticleStackEditor(Particles::ParticleStackAsset& asset, AssetManager* assets,
                                         Editor::UndoRedoService* undo, std::function<void()> requestRefresh)
    : m_Asset(asset), m_Assets(assets), m_Undo(undo), m_RequestRefresh(std::move(requestRefresh))
{
    Reload();
}

void ParticleStackEditor::Reload()
{
    m_Diagnostics = m_Asset.Diagnostics();
    m_Valid = m_Asset.Document() != nullptr;
    if (m_Valid)
        m_Document = *m_Asset.Document();
}

bool ParticleStackEditor::Apply(const Particles::StackDocument& document)
{
    std::vector<Particles::StackDiagnostic> diagnostics;
    if (!Particles::ValidateStack(document, diagnostics) || !m_Asset.SetDocument(document))
    {
        m_Diagnostics = diagnostics.empty() ? m_Asset.Diagnostics() : std::move(diagnostics);
        return false;
    }
    m_Diagnostics.clear();
    m_Document = document;
    return true;
}

void ParticleStackEditor::RequestRefresh() const
{
    if (m_RequestRefresh)
        m_RequestRefresh();
}

void ParticleStackEditor::Commit(const std::string& label, const Edit& edit, bool rebuild)
{
    if (m_Gesture)
    {
        auto document = m_Document;
        edit(document);
        const bool applied = Apply(document);
        FinishPreview();
        if (rebuild || !applied)
            RequestRefresh();
        return;
    }
    // Outside a gesture the asset may have moved on (an undo, a reload): start from it.
    Reload();
    if (!m_Valid)
        return;
    auto document = m_Document;
    edit(document);
    std::vector<Particles::StackDiagnostic> diagnostics;
    if (!Particles::ValidateStack(document, diagnostics))
    {
        m_Diagnostics = std::move(diagnostics);
        RequestRefresh();
        return;
    }
    Editor::UndoRedoService::InteractiveEdit change;
    if (m_Undo)
        change = m_Undo->BeginInteractiveEdit(
            label, MakeFileSnapshotTarget(EditedStackFile(m_Asset, m_Assets), label));
    if (Apply(document) && m_Asset.Save() && change)
        change.Commit();
    if (rebuild || !m_Diagnostics.empty())
        RequestRefresh();
}

void ParticleStackEditor::Preview(const std::string& label, const Edit& edit)
{
    if (!m_Gesture)
    {
        Reload();
        if (!m_Valid)
            return;
        if (m_Undo)
            m_Gesture = m_Undo->BeginInteractiveEdit(
                label, MakeFileSnapshotTarget(EditedStackFile(m_Asset, m_Assets), label));
    }
    auto document = m_Document;
    edit(document);
    Apply(document);
}

void ParticleStackEditor::FinishPreview()
{
    // Without an undo service there is no gesture; the last preview is still unsaved.
    if (!m_Asset.Save() || !m_Gesture)
        return;
    m_Gesture.Commit();
    m_Gesture = {};
}

void ParticleStackEditor::CancelPreview()
{
    if (m_Gesture)
    {
        // The snapshot writes the file as it was and the asset reloads it.
        m_Gesture.Cancel();
        m_Gesture = {};
    }
    else
        m_Asset.Load();
    Reload();
}

} // namespace GameEngine::ParticleInspectors
