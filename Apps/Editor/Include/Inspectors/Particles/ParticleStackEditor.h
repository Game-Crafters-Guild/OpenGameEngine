#pragma once

#include "Particles/ParticleStackDocument.h"
#include "UndoRedo/UndoRedoService.h"

#include <functional>
#include <string>
#include <vector>

namespace GameEngine
{
class AssetManager;
}

namespace GameEngine::Particles
{
class ParticleStackAsset;
}

namespace GameEngine::ParticleInspectors
{

/// The stack asset an inspector edits, with its undo. Every edit is a function of the document: a
/// commit applies it, saves the file and records one undo entry; a preview (a drag in progress)
/// applies it in memory, so every emitter running the stack shows it at once, and records one
/// entry when it finishes. The inspector only reads the document; all writes go through here.
///
/// Holds the asset by reference: the inspector that owns the editor is rebuilt whenever the
/// selection or the asset changes, before the asset manager could release it. Its undo entries
/// outlive it and the asset, so they hold the asset's GUID and file and reload whichever object
/// `assets` has loaded for that GUID when they are undone.
class ParticleStackEditor
{
  public:
    using Edit = std::function<void(Particles::StackDocument&)>;

    ParticleStackEditor(Particles::ParticleStackAsset& asset, AssetManager* assets, Editor::UndoRedoService* undo,
                        std::function<void()> requestRefresh);
    ParticleStackEditor(const ParticleStackEditor&) = delete;
    ParticleStackEditor& operator=(const ParticleStackEditor&) = delete;

    /// The document as last committed or previewed, or null when the asset holds no valid stack.
    const Particles::StackDocument* Document() const { return m_Valid ? &m_Document : nullptr; }
    /// Why the asset holds no valid stack, or why the last edit was refused.
    const std::vector<Particles::StackDiagnostic>& Diagnostics() const { return m_Diagnostics; }

    /// Applies `edit` with one undo entry named `label`; rebuilds the inspector when the edit
    /// changes which fields are shown or when it is refused.
    void Commit(const std::string& label, const Edit& edit, bool rebuild);
    /// Applies `edit` live as part of one gesture; the first preview opens the undo entry.
    void Preview(const std::string& label, const Edit& edit);
    /// Saves the gesture's result and records its undo entry.
    void FinishPreview();
    /// Reverts every preview of the gesture and records nothing.
    void CancelPreview();

  private:
    void Reload();
    // Applies `document` to the asset, or keeps the asset and reports why it cannot.
    bool Apply(const Particles::StackDocument& document);
    void RequestRefresh() const;

    Particles::ParticleStackAsset& m_Asset;
    AssetManager* m_Assets = nullptr;
    Editor::UndoRedoService* m_Undo = nullptr;
    std::function<void()> m_RequestRefresh;
    Particles::StackDocument m_Document;
    std::vector<Particles::StackDiagnostic> m_Diagnostics;
    bool m_Valid = false;
    Editor::UndoRedoService::InteractiveEdit m_Gesture;
};

} // namespace GameEngine::ParticleInspectors
