#pragma once

#include "AssetCore/Asset.h"
#include "Particles/CompiledParticleStack.h"
#include "Particles/ParticleStackDocument.h"

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace GameEngine::Particles
{

/// A particle processor stack (.particlestack): the JSON authoring form in a project and the cooked
/// binary form in a packaged game. Emitters reference it by GUID and run its one compiled stack.
///
/// A reload or an edit that does not produce a valid stack keeps the last valid one, so running
/// emitters keep their effect while an author repairs the file; Diagnostics says what is wrong.
class ParticleStackAsset final : public Asset
{
  public:
    ParticleStackAsset(const GUID& guid, const std::filesystem::path& path);

    bool Load() override;
    bool LoadFromData(const Vector<uint8>& data) override;
    void Unload() override;

    /// The stack as last loaded or set, or null before a valid one was.
    const StackDocument* Document() const { return m_Compiled ? m_Compiled->Document.get() : nullptr; }
    std::shared_ptr<const CompiledParticleStack> Compiled() const { return m_Compiled; }
    /// Why the last load, reload or SetDocument was refused; empty after one that succeeded.
    const std::vector<StackDiagnostic>& Diagnostics() const { return m_Diagnostics; }
    /// Expires when this object is destroyed, so work that outlives it (an editor undo entry) can
    /// tell whether it may still reach it.
    std::weak_ptr<const void> Liveness() const { return m_Liveness; }

    /// Replaces the stack in memory, as an edit in progress does before it is saved. Refuses an
    /// invalid document and keeps the current stack.
    bool SetDocument(StackDocument document);
    /// Writes the current stack's authoring form to the asset's path.
    bool Save() const;

    /// Rewrites the authoring form in `file` as the cooked form a packaged game loads.
    static bool CookFile(const std::filesystem::path& file, std::string& error);

  protected:
    bool ReloadFromData(const Vector<uint8>& data) override;

  private:
    bool Adopt(std::span<const uint8> bytes);
    bool Adopt(std::shared_ptr<const StackDocument> document);

    std::shared_ptr<const CompiledParticleStack> m_Compiled;
    std::vector<StackDiagnostic> m_Diagnostics;
    std::shared_ptr<const void> m_Liveness = std::make_shared<const bool>(true);
};

} // namespace GameEngine::Particles
